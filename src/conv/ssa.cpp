#include "ssa.hpp"

#include "../casting.hpp"
#include "../structure/ast.hpp"

struct SsaContext {
  IrProgram *program;
  IrFunc *func;
  BasicBlock *bb;
  // bb stack for (continue, break)
  std::vector<std::pair<BasicBlock *, BasicBlock *>> loop_stk;
};

Value *convert_expr(SsaContext *ctx, Expr *expr) {
  if (auto x = dyn_cast<Binary>(expr)) {
    auto lhs = convert_expr(ctx, x->lhs);
    if (x->tag == Expr::Tag::Mod) {
      auto rhs = convert_expr(ctx, x->rhs);
      // a % b := a - b * a / b (ARM has no MOD instruction)
      auto quotient = new BinaryInst(Value::Tag::Div, lhs, rhs, ctx->bb);
      auto multiple = new BinaryInst(Value::Tag::Mul, rhs, quotient, ctx->bb);
      auto remainder = new BinaryInst(Value::Tag::Sub, lhs, multiple, ctx->bb);
      return remainder;
    } else if (x->tag == Expr::And || x->tag == Expr::Or) {
      auto rhs_bb = new BasicBlock, after_bb = new BasicBlock;
      ctx->func->bb.insertAtEnd(rhs_bb);
      if (x->tag == Expr::And) {
        new BranchInst(lhs, rhs_bb, after_bb, ctx->bb);
      } else {
        auto inv = new BinaryInst(Value::Tag::Eq, lhs, ConstValue::get(0), ctx->bb);
        new BranchInst(inv, rhs_bb, after_bb, ctx->bb);
      }
      // 这里需要pred的大小为2，真正维护pred在最后才做，可以保证是[当前bb, rhs的实际计算bb]的顺序
      // 注意rhs的实际计算bb不一定就是rhs_bb，因为rhs也可能是&& ||
      after_bb->pred.resize(2);
      ctx->bb = rhs_bb;
      auto rhs = convert_expr(ctx, x->rhs);
      new JumpInst(after_bb, ctx->bb);
      ctx->func->bb.insertAtEnd(after_bb);
      ctx->bb = after_bb;
      auto inst = new PhiInst(ctx->bb);
      inst->incoming_values[0].set(lhs);
      inst->incoming_values[1].set(rhs);
      return inst;
    } else {
      auto rhs = convert_expr(ctx, x->rhs);
      // happened to have same tag values
      auto inst = new BinaryInst((Value::Tag)x->tag, lhs, rhs, ctx->bb);
      return inst;
    }
  } else if (auto x = dyn_cast<IntConst>(expr)) {
    return ConstValue::get(x->val);
  } else if (auto x = dyn_cast<Index>(expr)) {
    // evalulate dims first
    std::vector<Value *> dims;
    dims.reserve(x->dims.size());
    for (auto &p : x->dims) {
      auto value = convert_expr(ctx, p);
      dims.push_back(value);
    }

    if (x->dims.size() == x->lhs_sym->dims.size()) {
      // access to element
      if (x->dims.empty()) {
        // direct access
        auto inst = new LoadInst(x->lhs_sym, x->lhs_sym->value, ConstValue::get(0), ctx->bb);
        return inst;
      } else {
        // all levels except last level, emit GetElementPtr
        Value *val = x->lhs_sym->value;
        Inst *res = nullptr;
        for (u32 i = 0; i < x->dims.size(); i++) {
          int size = i + 1 < x->lhs_sym->dims.size() ? x->lhs_sym->dims[i + 1]->result : 1;
          if (i + 1 < x->dims.size()) {
            auto inst = new GetElementPtrInst(x->lhs_sym, val, dims[i], size, ctx->bb);
            res = inst;
            val = inst;
          } else {
            auto inst = new LoadInst(x->lhs_sym, val, dims[i], ctx->bb);
            res = inst;
          }
        }

        return res;
      }
    } else if (x->dims.size()) {
      // access to sub array
      // emit GetElementPtr for each level
      Value *val = x->lhs_sym->value;
      Inst *res = nullptr;
      for (u32 i = 0; i < x->dims.size(); i++) {
        int size = i + 1 < x->lhs_sym->dims.size() ? x->lhs_sym->dims[i + 1]->result : 1;
        auto inst = new GetElementPtrInst(x->lhs_sym, val, dims[i], size, ctx->bb);
        res = inst;
        val = inst;
      }
      return res;
    } else {
      // access to array itself
      auto inst = new GetElementPtrInst(x->lhs_sym, x->lhs_sym->value, ConstValue::get(0), 0, ctx->bb);
      return inst;
    }
  } else if (auto x = dyn_cast<Call>(expr)) {
    // must evaluate args before calling
    std::vector<Value *> args;
    args.reserve(x->args.size());
    for (auto &p : x->args) {
      auto value = convert_expr(ctx, p);
      args.push_back(value);
    }

    auto inst = new CallInst(x->f->val, ctx->bb);

    // args
    inst->args.reserve(x->args.size());
    for (auto &value : args) {
      inst->args.emplace_back(value, inst);
    }
    return inst;
  }
  return nullptr;
}

void convert_stmt(SsaContext *ctx, Stmt *stmt) {
  if (auto x = dyn_cast<DeclStmt>(stmt)) {
    for (auto &decl : x->decls) {
      // local variables
      auto inst = new AllocaInst(&decl, ctx->bb);
      decl.value = inst;

      // handle init expr
      if (decl.has_init) {
        if (decl.init.val1) {
          // assign variable to expr
          auto init = convert_expr(ctx, decl.init.val1);
          new StoreInst(&decl, inst, init, ConstValue::get(0), ctx->bb);
        } else {
          // assign each element of flatten_init
          // heuristic: count how many elements are zero
          int num_zeros = 0;
          std::vector<Value *> values;
          values.reserve(decl.flatten_init.size());
          for (u32 i = 0; i < decl.flatten_init.size(); i++) {
            auto init = convert_expr(ctx, decl.flatten_init[i]);
            values.push_back(init);
            if (auto x = dyn_cast<ConstValue>(init)) {
              if (x->imm == 0) {
                num_zeros++;
              }
            }
          }

          bool emit_memset = false;
          if (num_zeros > 10) {
            emit_memset = true;
            auto call_inst = new CallInst(Func::BUILTIN[8].val, ctx->bb);
            call_inst->args.reserve(3);
            // arr
            call_inst->args.emplace_back(inst, call_inst);
            // ch
            call_inst->args.emplace_back(ConstValue::get(0), call_inst);
            // count = num * 4
            call_inst->args.emplace_back(ConstValue::get(decl.dims[0]->result * 4), call_inst);
          }

          for (u32 i = 0; i < decl.flatten_init.size(); i++) {
            // skip safely
            if (auto x = dyn_cast<ConstValue>(values[i])) {
              if (emit_memset && x->imm == 0) {
                continue;
              }
            }

            new StoreInst(&decl, inst, values[i], ConstValue::get(i), ctx->bb);
          }
        }
      }
    }
  } else if (auto x = dyn_cast<Assign>(stmt)) {
    // evaluate dims first
    std::vector<Value *> dims;
    dims.reserve(x->dims.size());
    for (auto &expr : x->dims) {
      auto dim = convert_expr(ctx, expr);
      dims.push_back(dim);
    }

    // rhs
    auto rhs = convert_expr(ctx, x->rhs);

    if (x->dims.empty()) {
      new StoreInst(x->lhs_sym, x->lhs_sym->value, rhs, ConstValue::get(0), ctx->bb);
    } else {
      // all levels except last level, emit GetElementPtr
      auto last = x->lhs_sym->value;
      for (u32 i = 0; i < x->dims.size(); i++) {
        int size = i + 1 < x->lhs_sym->dims.size() ? x->lhs_sym->dims[i + 1]->result : 1;
        if (i + 1 < x->dims.size()) {
          auto inst = new GetElementPtrInst(x->lhs_sym, last, dims[i], size, ctx->bb);
          last = inst;
        } else {
          new StoreInst(x->lhs_sym, last, rhs, dims[i], ctx->bb);
        }
      }
    }

  } else if (auto x = dyn_cast<If>(stmt)) {
    // 1. check `cond`
    // 2. branch to `then` or `else`
    // 3. jump to `end` at the end of `then` and `else`
    auto cond = convert_expr(ctx, x->cond);
    BasicBlock *bb_then = new BasicBlock;
    BasicBlock *bb_else = new BasicBlock;
    BasicBlock *bb_end = new BasicBlock;
    ctx->func->bb.insertAtEnd(bb_then);
    ctx->func->bb.insertAtEnd(bb_else);
    ctx->func->bb.insertAtEnd(bb_end);

    new BranchInst(cond, bb_then, bb_else, ctx->bb);

    // then
    ctx->bb = bb_then;
    convert_stmt(ctx, x->on_true);
    // jump to end bb
    if (!ctx->bb->valid()) {
      new JumpInst(bb_end, ctx->bb);
    }
    // else
    ctx->bb = bb_else;
    if (x->on_false) {
      convert_stmt(ctx, x->on_false);
    }
    // jump to end bb
    if (!ctx->bb->valid()) {
      new JumpInst(bb_end, ctx->bb);
    }

    ctx->bb = bb_end;
  } else if (auto x = dyn_cast<While>(stmt)) {
    // four bb:
    // cond1: loop or end
    // loop: cond2
    // cond2 : loop or end
    // end
    BasicBlock *bb_cond1 = new BasicBlock;
    BasicBlock *bb_loop = new BasicBlock;
    BasicBlock *bb_cond2 = new BasicBlock;
    BasicBlock *bb_end = new BasicBlock;
    ctx->func->bb.insertAtEnd(bb_cond1);
    ctx->func->bb.insertAtEnd(bb_loop);

    // jump to cond1 bb
    new JumpInst(bb_cond1, ctx->bb);

    // cond1
    ctx->bb = bb_cond1;
    auto cond = convert_expr(ctx, x->cond);
    new BranchInst(cond, bb_loop, bb_end, ctx->bb);

    // loop
    ctx->bb = bb_loop;
    ctx->loop_stk.emplace_back(bb_cond2, bb_end);
    convert_stmt(ctx, x->body);
    ctx->loop_stk.pop_back();
    // jump to cond2 bb
    if (!ctx->bb->valid()) {
      new JumpInst(bb_cond2, ctx->bb);
    }

    // cond2
    ctx->func->bb.insertAtEnd(bb_cond2);
    ctx->bb = bb_cond2;
    cond = convert_expr(ctx, x->cond);
    new BranchInst(cond, bb_loop, bb_end, ctx->bb);

    ctx->func->bb.insertAtEnd(bb_end);
    ctx->bb = bb_end;
  } else if (auto x = dyn_cast<Block>(stmt)) {
    for (auto &stmt : x->stmts) {
      convert_stmt(ctx, stmt);
      if (isa<Continue>(stmt) || isa<Break>(stmt) || isa<Return>(stmt)) {
        break;
      }
    }
  } else if (auto x = dyn_cast<Return>(stmt)) {
    if (x->val) {
      auto value = convert_expr(ctx, x->val);
      new ReturnInst(value, ctx->bb);
    } else {
      new ReturnInst(nullptr, ctx->bb);
    }
  } else if (auto x = dyn_cast<ExprStmt>(stmt)) {
    if (x->val) {
      convert_expr(ctx, x->val);
    }
  } else if (isa<Continue>(stmt)) {
    new JumpInst(ctx->loop_stk.back().first, ctx->bb);
  } else if (isa<Break>(stmt)) {
    new JumpInst(ctx->loop_stk.back().second, ctx->bb);
  }
}

IrProgram *convert_ssa(Program &p) {
  IrProgram *ret = new IrProgram;
  for (Func &builtin : Func::BUILTIN) {
    IrFunc *func = new IrFunc;
    func->builtin = true;
    func->func = &builtin;
    builtin.val = func;
    ret->func.insertAtEnd(func);
  }
  for (auto &g : p.glob) {
    if (Func *f = std::get_if<0>(&g)) {
      IrFunc *func = new IrFunc;
      func->builtin = false;
      func->func = f;
      f->val = func;
      ret->func.insertAtEnd(func);
    }
  }
  for (auto &g : p.glob) {
    if (Func *f = std::get_if<0>(&g)) {
      IrFunc *func = f->val;
      BasicBlock *entryBB = new BasicBlock;
      func->bb.insertAtEnd(entryBB);

      // setup params
      for (auto &p : f->params) {
        if (p.dims.empty()) {
          // alloca for each non-array param
          auto inst = new AllocaInst(&p, entryBB);
          p.value = inst;
          // then copy param into it
          new StoreInst(&p, inst, new ParamRef(&p), ConstValue::get(0), entryBB);
        } else {
          // there is no need to alloca for array param
          p.value = new ParamRef(&p);
        }
      }

      SsaContext ctx = {ret, func, entryBB};
      for (auto &stmt : f->body.stmts) {
        convert_stmt(&ctx, stmt);
      }

      // add extra return statement to avoid undefined behavior
      if (!ctx.bb->valid()) {
        if (func->func->is_int) {
          new ReturnInst(ConstValue::get(0), ctx.bb);
        } else {
          new ReturnInst(nullptr, ctx.bb);
        }
      }

      for (BasicBlock *bb = func->bb.head; bb; bb = bb->next) {
        bb->pred.clear();
      }
      for (BasicBlock *bb = func->bb.head; bb; bb = bb->next) {
        for (BasicBlock *x : bb->succ()) {
          if (x) x->pred.push_back(bb);
        }
      }
    } else {
      Decl *d = std::get_if<1>(&g);
      ret->glob_decl.push_back(d);
      d->value = new GlobalRef(d);
    }
  }
  return ret;
}

#include "ssa.hpp"

#include "ast.hpp"
#include "casting.hpp"

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
    auto rhs = convert_expr(ctx, x->rhs);
    if (x->tag == Expr::Tag::Mod) {
      // a % b := a - b * a / b (ARM has no MOD instruction)
      auto quotient = new BinaryInst(Value::Tag::Div, lhs, rhs, ctx->bb);
      auto multiple = new BinaryInst(Value::Tag::Mul, rhs, quotient, ctx->bb);
      auto remainder = new BinaryInst(Value::Tag::Sub, lhs, multiple, ctx->bb);
      return remainder;
    } else {
      // happened to have same tag values
      auto inst = new BinaryInst((Value::Tag)x->tag, lhs, rhs, ctx->bb);
      return inst;
    }
  } else if (auto x = dyn_cast<IntConst>(expr)) {
    return new ConstValue(x->val);
  } else if (auto x = dyn_cast<Index>(expr)) {
    // evalulate dims first
    std::vector<Value *> dims;
    dims.reserve(x->dims.size());
    for (auto &p : x->dims) {
      auto value = convert_expr(ctx, p);
      dims.push_back(value);
    }

    auto inst = new LoadInst(x->lhs_sym, x->lhs_sym->value, ctx->bb);

    // dims
    inst->dims.reserve(x->dims.size());
    for (auto &dim : dims) {
      inst->dims.emplace_back(dim, inst);
    }
    return inst;
  } else if (auto x = dyn_cast<Call>(expr)) {
    // must evaluate args before calling
    std::vector<Value *> args;
    args.reserve(x->args.size());
    for (auto &p : x->args) {
      auto value = convert_expr(ctx, p);
      args.push_back(value);
    }

    auto inst = new CallInst(x->f, ctx->bb);

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
          auto store_inst = new StoreInst(&decl, inst, init, ctx->bb);
        } else {
          // assign each element of flatten_init
          // heuristic: count how many elements are zero
          int num_zeros = 0;
          std::vector<Value *> values;
          values.reserve(decl.flatten_init.size());
          for (int i = 0; i < decl.flatten_init.size(); i++) {
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
            // get addr, to make llvm happy
            // but we can remove it later
            auto load_inst = new LoadInst(&decl, inst, ctx->bb);

            // dims: all zero except last
            load_inst->dims.reserve(decl.dims.size()-1);
            for (int i = 0;i < decl.dims.size()-1;i++) {
              load_inst->dims.emplace_back(new ConstValue(0), inst);
            }

            auto call_inst = new CallInst(&Func::MEMSET, ctx->bb);
            call_inst->args.reserve(3);
            // arr
            call_inst->args.emplace_back(load_inst, call_inst);
            // ch
            call_inst->args.emplace_back(new ConstValue(0), call_inst);
            // count = num * 4
            call_inst->args.emplace_back(new ConstValue(decl.dims[0]->result * 4), call_inst);
          }

          for (int i = 0; i < decl.flatten_init.size(); i++) {
            // skip safely
            if (auto x = dyn_cast<ConstValue>(values[i])) {
              if (emit_memset && x->imm == 0) {
                continue;
              }
            }

            auto store_inst = new StoreInst(&decl, inst, values[i], ctx->bb);
            int temp = i;
            store_inst->dims.reserve(decl.dims.size());
            for (int j = 0; j < decl.dims.size(); j++) {
              int size = j + 1 < decl.dims.size() ? decl.dims[j + 1]->result : 1;
              int index = temp / size;
              temp %= size;
              store_inst->dims.emplace_back(new ConstValue(index), store_inst);
            }
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
    auto inst = new StoreInst(x->lhs_sym, x->lhs_sym->value, rhs, ctx->bb);

    // dims
    inst->dims.reserve(x->dims.size());
    for (auto &dim : dims) {
      inst->dims.emplace_back(dim, inst);
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

    auto br_inst = new BranchInst(cond, bb_then, bb_else, ctx->bb);

    // then
    ctx->bb = bb_then;
    convert_stmt(ctx, x->on_true);
    // jump to end bb
    if (!ctx->bb->valid()) {
      auto inst_then = new JumpInst(bb_end, ctx->bb);
    }
    // else
    ctx->bb = bb_else;
    if (x->on_false) {
      convert_stmt(ctx, x->on_false);
    }
    // jump to end bb
    if (!ctx->bb->valid()) {
      auto inst_else = new JumpInst(bb_end, ctx->bb);
    }

    ctx->bb = bb_end;
  } else if (auto x = dyn_cast<While>(stmt)) {
    // 1. jump to `cond`
    // 2. branch to `end` or `loop`
    // 3. jump to `cond` at the end of `loop`
    BasicBlock *bb_cond = new BasicBlock;
    BasicBlock *bb_loop = new BasicBlock;
    BasicBlock *bb_end = new BasicBlock;
    ctx->func->bb.insertAtEnd(bb_cond);
    ctx->func->bb.insertAtEnd(bb_loop);
    ctx->func->bb.insertAtEnd(bb_end);

    // jump to cond bb
    auto inst_cond = new JumpInst(bb_cond, ctx->bb);

    // cond
    ctx->bb = bb_cond;
    auto cond = convert_expr(ctx, x->cond);
    auto br_inst = new BranchInst(cond, bb_loop, bb_end, ctx->bb);

    // loop
    ctx->bb = bb_loop;
    ctx->loop_stk.emplace_back(bb_cond, bb_end);
    convert_stmt(ctx, x->body);
    ctx->loop_stk.pop_back();
    // jump to cond bb
    if (!ctx->bb->valid()) {
      auto inst_continue = new JumpInst(bb_cond, ctx->bb);
    }

    ctx->bb = bb_end;
  } else if (auto x = dyn_cast<Block>(stmt)) {
    for (auto &stmt : x->stmts) {
      convert_stmt(ctx, stmt);
    }
  } else if (auto x = dyn_cast<Return>(stmt)) {
    if (x->val) {
      auto value = convert_expr(ctx, x->val);
      auto inst = new ReturnInst(value, ctx->bb);
    } else {
      auto inst = new ReturnInst(nullptr, ctx->bb);
    }
  } else if (auto x = dyn_cast<ExprStmt>(stmt)) {
    if (x->val) {
      convert_expr(ctx, x->val);
    }
  } else if (auto x = dyn_cast<Continue>(stmt)) {
    auto inst = new JumpInst(ctx->loop_stk.back().first, ctx->bb);
  } else if (auto x = dyn_cast<Break>(stmt)) {
    auto inst = new JumpInst(ctx->loop_stk.back().second, ctx->bb);
  }
}

IrProgram *convert_ssa(Program &p) {
  IrProgram *ret = new IrProgram;
  for (auto &g : p.glob) {
    if (Func *f = std::get_if<0>(&g)) {
      IrFunc *func = new IrFunc;
      func->func = f;
      ret->func.insertAtEnd(func);
      BasicBlock *entryBB = new BasicBlock;
      func->bb.insertAtEnd(entryBB);

      // setup params
      for (auto &p : f->params) {
        if (p.dims.empty()) {
          // alloca for each non-array param
          auto inst = new AllocaInst(&p, entryBB);
          p.value = inst;
          // then copy param into it
          auto store_inst = new StoreInst(&p, inst, new ParamRef(&p), entryBB);
        } else {
          // there is no need to alloca for array param
          p.value = new ParamRef(&p);
        }
      }

      SsaContext ctx{.program = ret, .func = func, .bb = entryBB};
      for (auto &stmt : f->body.stmts) {
        convert_stmt(&ctx, stmt);
      }

      // add extra return statement to avoid undefined behavior
      if (!ctx.bb->valid()) {
        if (func->func->is_int) {
          auto value = new ConstValue(0);
          auto inst = new ReturnInst(value, ctx.bb);
        } else {
          auto inst = new ReturnInst(nullptr, ctx.bb);
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

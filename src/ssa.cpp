#include "ssa.hpp"

#include "ast.hpp"
#include "casting.hpp"

struct SsaContext {
  IrProgram *program;
  IrFunc *func;
  BasicBlock *bb;
};

Value *convert_expr(SsaContext *ctx, Expr *expr) {
  if (auto x = dyn_cast<Binary>(expr)) {
    auto lhs = convert_expr(ctx, x->lhs);
    auto rhs = convert_expr(ctx, x->rhs);
    // happened to have same tag values
    auto inst = new BinaryInst((Value::Tag)x->tag, lhs, rhs, ctx->bb);
    return inst;
  } else if (auto x = dyn_cast<IntConst>(expr)) {
    return new ConstValue(x->result);
  } else if (auto x = dyn_cast<Index>(expr)) {
    // TODO dim
    auto value = ctx->func->decls[x->lhs_sym];
    auto inst = new LoadInst(value, ctx->bb);
    return inst;
  } else if (auto x = dyn_cast<Call>(expr)) {
    // TODO args
    auto func = ctx->program->findFunc(x->f);
    auto inst = new CallInst(func, ctx->bb);
    return inst;
  }
  return nullptr;
}

void convert_stmt(SsaContext *ctx, Stmt *stmt) {
  if (auto x = dyn_cast<DeclStmt>(stmt)) {
    // local variables
    for (auto &decl : x->decls) {
      auto inst = new AllocaInst(ctx->bb);
      ctx->func->decls[&decl] = inst;
    }
  } else if (auto x = dyn_cast<Assign>(stmt)) {
    // lhs
    auto value = ctx->func->decls[x->lhs_sym];
    // rhs
    auto rhs = convert_expr(ctx, x->rhs);
    auto inst = new StoreInst(value, rhs, ctx->bb);

    // dims
    inst->dims.reserve(x->dims.size());
    for (auto &expr : x->dims) {
      auto dim = convert_expr(ctx, expr);
      inst->dims.emplace_back(dim, inst);
    }

  } else if (auto x = dyn_cast<If>(stmt)) {
    auto cond = convert_expr(ctx, x->cond);
    BasicBlock *bb_then = new BasicBlock;
    BasicBlock *bb_else = new BasicBlock;
    BasicBlock *bb_end = new BasicBlock;
    ctx->func->bb.insertAtEnd(bb_then);
    ctx->func->bb.insertAtEnd(bb_else);
    ctx->func->bb.insertAtEnd(bb_end);

    auto br_inst = new BranchInst(cond, bb_then, bb_else, ctx->bb);

    ctx->bb = bb_then;
    convert_stmt(ctx, x->on_true);
    if (x->on_false) {
      ctx->bb = bb_else;
      convert_stmt(ctx, x->on_false);
    }

    // jump to end bb
    auto inst_then = new JumpInst(bb_end, bb_then);
    auto inst_else = new JumpInst(bb_end, bb_else);

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
      SsaContext ctx = {
        .program = ret,
        .func = func,
        .bb = entryBB
      };
      for (auto &stmt : f->body.stmts) {
        convert_stmt(&ctx, stmt);
      }
    } else {
      Decl *d = std::get_if<1>(&g);
      // TODO
    }
  }
  return ret;
}

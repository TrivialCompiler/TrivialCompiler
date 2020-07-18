#include "ssa.hpp"

#include "ast.hpp"
#include "casting.hpp"

Value *convert_expr(IrFunc *func, Expr *expr, BasicBlock *bb) { return nullptr; }

void convert_stmt(IrFunc *func, Stmt *stmt, BasicBlock *bb) {
  if (auto x = dyn_cast<DeclStmt>(stmt)) {
    // local variables
    for (auto &decl : x->decls) {
      auto inst = new AllocaInst(bb);
      func->decls[&decl] = inst;
    }
  } else if (auto x = dyn_cast<Assign>(stmt)) {
    // lhs
    auto value = func->decls[x->lhs_sym];
    auto inst = new StoreInst(bb);
    inst->arr = new Use(value, inst);

    // dims
    inst->dims.reserve(x->dims.size());
    for (auto &expr : x->dims) {
      auto dim = convert_expr(func, expr, bb);
      inst->dims.push_back(new Use(dim, inst));
    }

    // rhs
    auto rhs = convert_expr(func, x->rhs, bb);
    inst->data = new Use(rhs, inst);
  }
}

IrProgram *convert_ssa(Program &p) {
  IrProgram *ret = new IrProgram;
  for (auto &g : p.glob) {
    if (Func *f = std::get_if<0>(&g)) {
      dbg(f->name);

      IrFunc *func = new IrFunc;
      func->func = f;
      ret->func.insertAtEnd(func);
      BasicBlock *entryBB = new BasicBlock;
      func->bb.insertAtEnd(entryBB);
      for (auto &stmt : f->body.stmts) {
        convert_stmt(func, stmt, entryBB);
      }
    } else {
      Decl *d = std::get_if<1>(&g);
      // TODO
    }
  }
  return ret;
}

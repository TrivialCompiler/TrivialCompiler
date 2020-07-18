#include "ssa.hpp"

#include "ast.hpp"
#include "casting.hpp"

Value *convert_expr(IrFunc *func, Expr *expr, BasicBlock *bb) {
  if (auto x = dyn_cast<Binary>(expr)) {
    auto lhs = convert_expr(func, x->lhs, bb);
    auto rhs = convert_expr(func, x->rhs, bb);
    // happened to have same tag values
    auto inst = new BinaryInst((Value::Tag)x->tag, lhs, rhs, bb);
    return inst;
  } else if (auto x = dyn_cast<IntConst>(expr)) {
    return new ConstValue(x->result);
  } else if (auto x = dyn_cast<Index>(expr)) {
    // TODO dim
    auto value = func->decls[x->lhs_sym];
    auto inst = new LoadInst(value, bb);
    return inst;
  }
  return nullptr;
}

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
    // rhs
    auto rhs = convert_expr(func, x->rhs, bb);
    auto inst = new StoreInst(value, rhs, bb);

    // dims
    inst->dims.reserve(x->dims.size());
    for (auto &expr : x->dims) {
      auto dim = convert_expr(func, expr, bb);
      inst->dims.emplace_back(dim, inst);
    }

  } else if (auto x = dyn_cast<If>(stmt)) {
    auto cond = convert_expr(func, x->cond, bb);
    BasicBlock *bb_then = new BasicBlock;
    BasicBlock *bb_else = new BasicBlock;
    BasicBlock *bb_end = new BasicBlock;
    func->bb.insertAtEnd(bb_then);
    func->bb.insertAtEnd(bb_else);
    func->bb.insertAtEnd(bb_end);

    auto br_inst = new BranchInst(cond, bb_then, bb_else, bb);

    convert_stmt(func, x->on_true, bb_then);
    if (x->on_false) {
      convert_stmt(func, x->on_false, bb_else);
    }

    // jump to end bb
    auto inst_then = new JumpInst(bb_end, bb_then);
    auto inst_else = new JumpInst(bb_end, bb_else);
  } else if (auto x = dyn_cast<Block>(stmt)) {
    for (auto &stmt : x->stmts) {
      convert_stmt(func, stmt, bb);
    }
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

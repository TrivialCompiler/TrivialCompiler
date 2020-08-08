#include "mark_global_const.hpp"
#include "../../ast.hpp"

static bool has_store(Value *v) {
  for (Use *u = v->uses.head; u; u = u->next) {
    if (isa<LoadInst>(u->user)) continue;
    else if (isa<StoreInst>(u->user)) return true;
    else if (auto x = dyn_cast<GetElementPtrInst>(u->user)) {
      if (has_store(x)) return true;
    } else if (auto x = dyn_cast<CallInst>(u->user)) {
      // todo: 和memdep中的注释一样，这里可以更仔细地分析函数是否修改这个参数
      if (x->has_side_effect()) return true;
    } else {
      // 目前地址只可能以GetElementPtr传递，到Load, Store, Call为止，不可能被别的指令，如Phi之类的使用
      UNREACHABLE();
    }
  }
  return false;
}

void mark_global_const(IrProgram *p) {
  for (Decl *d : p->glob_decl) {
    if (!d->is_const && !has_store(d->value)) {
      d->is_const = true;
      auto global_const = "Marking global variable '" + std::string(d->name) + "' as const";
      dbg(global_const);
    }
  }
}

#include "dead_store_elim.hpp"
#include "memdep.hpp"

void dead_store_elim(IrFunc *f) {
  clear_memdep(f);
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i;) {
      Inst *next = i->next;
      if (auto x = dyn_cast<StoreInst>(i)) {
        Decl *arr = x->lhs_sym;
        for (Inst *j = next; j; j = j->next) {
          if (auto y = dyn_cast<LoadInst>(j); y && alias(arr, y->lhs_sym)) break;
          else if (auto y = dyn_cast<CallInst>(j); y && y->func->has_side_effect && is_arr_call_alias(arr, y)) break;
          else if (auto y = dyn_cast<StoreInst>(j); y && y->lhs_sym == arr && y->arr.value == x->arr.value && y->index.value == x->index.value) {
            bb->insts.remove(x);
            delete x;
            break;
          }
        }
      }
      i = next;
    }
  }
}
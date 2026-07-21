#include "dead_store_elim.hpp"
#include "memdep.hpp"

static bool same_addr(AccessInst *lhs, AccessInst *rhs) {
  return lhs->lhs_sym == rhs->lhs_sym && lhs->arr.value == rhs->arr.value && lhs->index.value == rhs->index.value;
}

static bool store_preserves_loaded_value(StoreInst *store, LoadInst *load) {
  if (!alias(store->lhs_sym, load->lhs_sym)) return true;
  return store->data.value == load;
}

static bool same_value_store(StoreInst *store) {
  auto load = dyn_cast<LoadInst>(store->data.value);
  if (load && load->bb != store->bb) return false;
  if (!load || !same_addr(store, load)) return false;

  for (Inst *i = load->next; i && i != store; i = i->next) {
    if (auto x = dyn_cast<StoreInst>(i)) {
      if (!store_preserves_loaded_value(x, load)) return false;
    } else if (auto x = dyn_cast<CallInst>(i); x && x->func->has_side_effect && is_arr_call_alias(load->lhs_sym, x)) {
      return false;
    }
  }
  return true;
}

void dead_store_elim(IrFunc *f) {
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i;) {
      Inst *next = i->next;
      if (auto x = dyn_cast<StoreInst>(i)) {
        if (same_value_store(x)) {
          bb->insts.remove(x);
          delete x;
          i = next;
          continue;
        }
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

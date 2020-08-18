#include "if_to_cond.hpp"

void if_to_cond(MachineFunc* f) {
  for (auto bb = f->bb.head; bb; bb = bb->next) {
    // find pattern:
    // BB1:
    // b.cond BB3
    // BB2:
    // ...some instructions
    // BB3:
    // converts to:
    // BB1:
    // BB2:
    // ...some instructions + cond
    // BB3:
    if (auto b = dyn_cast_nullable<MIBranch>(bb->insts.tail)) {
      auto bb2 = bb->next;
      auto bb3 = b->target;
      if (bb2 && bb2->next == bb3) {
        bool can_optimize = true;
        if (b->cond == ArmCond::Ge) can_optimize = false;
        for (auto inst = bb2->insts.head; inst; inst = inst->next) {
          if (auto x = dyn_cast<MIAccess>(inst)) {
            if (x->cond != ArmCond::Any) {
              can_optimize = false;
            }
          } else if (auto x = dyn_cast<MIFma>(inst)) {
            if (x->cond != ArmCond::Any) {
              can_optimize = false;
            }
          } else {
            can_optimize = false;
          }
        }

        if (can_optimize) {
          dbg("Optimizing branches to conditional execution");
          bb->insts.remove(b);
          ArmCond cond = opposite_cond(b->cond);
          for (auto inst = bb2->insts.head; inst; inst = inst->next) {
            if (auto x = dyn_cast<MIAccess>(inst)) {
              x->cond = cond;
            } else if (auto x = dyn_cast<MIFma>(inst)) {
              x->cond = cond;
            } else {
              UNREACHABLE();
            }
          }
        }
      }
    }
  }
}

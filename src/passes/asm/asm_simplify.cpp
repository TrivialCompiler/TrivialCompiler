#include "asm_simplify.hpp"

void asm_simplify(MachineFunc* f) {
  for (auto bb = f->bb.head; bb; bb = bb->next) {
    for (auto inst = bb->insts.head; inst; inst = inst->next) {
      if (auto x = dyn_cast<MIMove>(inst)) {
        if (x->dst.is_equiv(x->rhs) && x->is_simple()) {
          dbg("Removed identity move");
          bb->insts.remove(inst);
        } else if (auto y = dyn_cast_nullable<MIMove>(inst->next)) {
          if (y->dst.is_equiv(x->dst) && !y->rhs.is_equiv(x->dst) && y->is_simple()) {
            dbg("Removed useless move");
            bb->insts.remove(inst);
          }
        }
      } else if (auto x = dyn_cast<MIBinary>(inst)) {
        if (x->isIdentity()) {
          dbg("Removed identity binary operation");
          bb->insts.remove(inst);
        }
      } else if (auto x = dyn_cast<MIJump>(inst)) {
        if (x->target == bb->next) {
          dbg("Removed unconditional jump to next bb");
          bb->insts.remove(inst);
        }
      }
    }
  }
}

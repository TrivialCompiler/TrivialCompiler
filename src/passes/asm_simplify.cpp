#include "asm_simplify.hpp"

void asm_simplify(MachineFunc* f) {
  for (auto bb = f->bb.head; bb; bb = bb->next) {
    for (auto inst = bb->insts.head; inst; inst = inst->next) {
      if (auto x = dyn_cast<MIMove>(inst)) {
        if (x->dst == x->rhs) {
          bb->insts.remove(inst);
        }
      }
    }
  }
}

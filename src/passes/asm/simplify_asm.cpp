#include "simplify_asm.hpp"

void simplify_asm(MachineFunc* f) {
  for (auto bb = f->bb.head; bb; bb = bb->next) {
    for (auto inst = bb->insts.head; inst; inst = inst->next) {
      if (auto x = dyn_cast<MIMove>(inst)) {
        if (x->dst.is_equiv(x->rhs) && x->is_simple()) {
          dbg("Removed identity move");
          bb->insts.remove(inst);
        } else if (auto y = dyn_cast_nullable<MIMove>(inst->next)) {
          if (y->dst.is_equiv(x->dst) && !y->rhs.is_equiv(x->dst) && y->is_simple() && x->is_simple()) {
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
      } else if (auto x = dyn_cast<MILoad>(inst)) {
        if (auto y = dyn_cast_nullable<MIStore>(x->prev)) {
          if (x->addr.is_equiv(y->addr) && x->offset == y->offset && x->shift == y->shift && x->mode == y->mode) {
            // match:
            // str r0, [r1, #0]
            // ldr r2, [r1, #0]
            // ldr can be optimized to:
            // mov r2, r0
            dbg("Removed unneeded load");
            auto i = new MIMove(x->next);
            i->dst = x->dst;
            i->rhs = y->data;
            bb->insts.remove(inst);
          }
        }
      } else if (auto x = dyn_cast<MICompare>(inst)) {
        if (auto y = dyn_cast_nullable<MIMove>(x->next)) {
          if (auto z = dyn_cast_nullable<MIMove>(y->next)) {
            if (x->rhs == MachineOperand::I(0) && y->rhs == MachineOperand::I(1) && z->rhs == MachineOperand::I(0) &&
                x->lhs.is_equiv(y->dst) && x->lhs.is_equiv(z->dst) && y->cond == ArmCond::Ne &&
                z->cond == ArmCond::Eq && y->shift.is_none() && z->shift.is_none()) {
              // match:
              // cmp	r1, #0
              // movne	r1, #1
              // moveq	r1, #0
              // the last `moveq` can be removed
              dbg("Simplify vreg != 0");
              bb->insts.remove(z);
            }
          }
        }
      }
    }
  }
}

#include "asm_simplify.hpp"

int ilog2(int n) {
  int res = 0;
  while(n >>= 1) res ++;
  return res;
}

void asm_simplify(MachineFunc* f) {
  for (auto bb = f->bb.head; bb; bb = bb->next) {
    for (auto inst = bb->insts.head; inst; inst = inst->next) {
      if (auto x = dyn_cast<MIMove>(inst)) {
        if (x->dst.is_equiv(x->rhs) && x->is_simple()) {
          dbg("Removed identity move");
          bb->insts.remove(inst);
        }
      } else if (auto x = dyn_cast<MIBinary>(inst)) {
        if (auto y = dyn_cast_nullable<MIMove>(inst->prev)) {
          if (x->dst.is_equiv(x->rhs) && !x->lhs.is_equiv(x->rhs) && x->rhs.is_equiv(x->dst) && y->rhs.is_imm() && y->is_simple()) {
            // mov r1, imm
            // mul r1, r2, r1
            // case1: imm is power of 2
            int imm = y->rhs.value;
            if (imm > 0 && (imm & (imm - 1)) == 0) {
              auto mul_opt = "Optimize multiplication of " + std::to_string(imm);
              dbg(mul_opt);
              int power = ilog2(imm);
              // replace by:
              // mov r1, r2, lsl #ilog2(imm)
              bb->insts.remove(x);
              bb->insts.remove(y);
              auto new_inst = new MIMove(x->next);
              new_inst->dst = x->dst;
              new_inst->rhs = x->lhs;
              new_inst->shift.type = ArmShift::Lsl;
              new_inst->shift.shift = power;
            }
          }
        }
      }
    }
  }
}

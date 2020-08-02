#include "compute_stack_info.hpp"

#include "../../codegen.hpp"

void compute_stack_info(MachineFunc *f) {
  for (auto bb = f->bb.head; bb; bb = bb->next) {
    for (auto inst = bb->insts.head; inst; inst = inst->next) {
      auto def = std::get<0>(get_def_use(inst));
      for (const auto &reg : def) {
        if ((int)ArmReg::r4 <= reg.value && reg.value <= (int)ArmReg::r11) {
          f->used_callee_saved_regs.insert((ArmReg)reg.value);
        }
      }
    }
  }
  dbg(f->func->func->name, f->used_callee_saved_regs);

  // fixup stack access
  for (auto &sp_inst : f->sp_fixup) {
    if (auto x = dyn_cast<MIAccess>(sp_inst)) {
      x->offset.value += f->sp_offset;
    } else if (auto x = dyn_cast<MIBinary>(sp_inst)) {
      assert(x->tag == MachineInst::Tag::Add);
      if (x->rhs.is_imm()) {
        // add r1, r0, imm
        x->rhs.value += f->sp_offset;
      } else if (auto y = dyn_cast_nullable<MIMove>(sp_inst->prev)) {
        // mv r0, imm
        // add r2, r1, r0
        assert(y->rhs.is_imm());
        y->rhs.value += f->sp_offset;
      } else {
        UNREACHABLE();
      }
    } else {
      UNREACHABLE();
    }
  }

  // fixup arg access
  // r4-r11, lr
  int saved_regs = f->used_callee_saved_regs.size() + 1;
  for (auto &sp_arg_inst : f->sp_arg_fixup) {
    if (auto x = dyn_cast<MIAccess>(sp_arg_inst)) {
      x->offset.value += f->sp_offset + 4 * saved_regs;
    } else {
      UNREACHABLE();
    }
  }
}

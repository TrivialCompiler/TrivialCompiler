#include "compute_stack_info.hpp"
#include "allocate_register.hpp"

void compute_stack_info(MachineFunc *f) {
  for (auto bb = f->bb.head; bb; bb = bb->next) {
    for (auto inst = bb->insts.head; inst; inst = inst->next) {
      auto def = std::get<0>(get_def_use(inst));
      for (const auto &reg : def) {
        if ((i32)ArmReg::r4 <= reg.value && reg.value <= (i32)ArmReg::r11) {
          f->used_callee_saved_regs.insert((ArmReg)reg.value);
        }
        if (reg.value == (i32)ArmReg::lr) {
          f->use_lr = true;
        }
      }
    }
  }
  dbg(f->func->func->name, f->used_callee_saved_regs);

  // fixup arg access
  // r4-r11, lr
  int saved_regs = f->used_callee_saved_regs.size() + (int) f->use_lr;

  for (auto &sp_arg_inst : f->sp_arg_fixup) {
    // mv r0, imm
    // ldr, [sp, r0]
    if (auto x = dyn_cast_nullable<MIMove>(sp_arg_inst)) {
      assert(x->rhs.is_imm());
      x->rhs.value += f->stack_size + 4 * saved_regs;
    } else {
      UNREACHABLE();
    }
  }
}

#include "compute_stack_info.hpp"
#include "../../codegen.hpp"

void compute_stack_info(MachineFunc* f) {
  for (auto bb = f->bb.head; bb; bb = bb->next) {
    for (auto inst = bb->insts.head; inst; inst = inst->next) {
      auto def = std::get<0>(get_def_use(inst));
      for (const auto &reg : def) {
        if ((int)ArmReg::r4 <= reg.value && reg.value <= (int)ArmReg::r11) {
          f->used_caller_saved_regs.insert((ArmReg) reg.value);
        }
      }
    }
  }
  dbg(f->func->func->name, f->used_caller_saved_regs);
}

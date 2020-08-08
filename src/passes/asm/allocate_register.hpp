#pragma once


#include "../../structure/ir.hpp"
#include "../../structure/machine_code.hpp"

void allocate_register(MachineProgram *p);
std::pair<std::vector<MachineOperand>, std::vector<MachineOperand>> get_def_use(MachineInst *inst);

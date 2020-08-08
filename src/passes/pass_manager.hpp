#pragma once

#include "../structure/ir.hpp"
#include "../structure/machine_code.hpp"

void run_ir_passes(IrProgram *p, bool opt);
void run_asm_passes(MachineProgram *p, bool opt);

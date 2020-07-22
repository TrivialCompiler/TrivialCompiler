#pragma once

#include "ir.hpp"
#include "machine_code.hpp"

MachineProgram *machine_code_selection(IrProgram *p);
void register_allocate(MachineProgram *p);

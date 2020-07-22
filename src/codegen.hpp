#pragma once

#include "machine_code.hpp"
#include "ir.hpp"

MachineProgram *machine_code_selection(IrProgram *p);
void register_allocate(MachineProgram *p);

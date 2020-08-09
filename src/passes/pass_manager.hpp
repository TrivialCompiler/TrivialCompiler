#pragma once

#include "../structure/ir.hpp"
#include "../structure/machine_code.hpp"

using IntermediateProgram = std::variant<IrProgram *, MachineProgram *>;

void run_passes(IntermediateProgram p, bool opt);

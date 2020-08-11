#pragma once

#include "../../structure/machine_code.hpp"

// schedule instructions to utilize cpu pipeline
void instruction_schedule(MachineFunc* f);
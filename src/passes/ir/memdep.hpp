#pragma once

#include "../../ir.hpp"

// 删除所有MemPhi和MemOp，并且保证Load.mem_token.value为空
void clear_memdep(IrFunc *f);

void compute_memdep(IrFunc *f);

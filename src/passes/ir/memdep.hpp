#pragma once

#include "../../structure/ir.hpp"

bool alias(Decl *arr1, Decl *arr2);

bool is_arr_call_alias(Decl *arr, CallInst *y);

// 删除所有MemPhi和MemOp，并且保证Load.mem_token.value为空
void clear_memdep(IrFunc *f);

void compute_memdep(IrFunc *f);

#pragma once

#include "../../structure/ir.hpp"

bool alias(Decl *arr1, Decl *arr2);

bool is_arr_call_alias(Decl *arr, CallInst *y);

// 删除所有MemPhi和MemOp，并且保证Load.mem_token.value为空
void clear_memdep(IrFunc *f);

// 目前除了clear_memdep外所有的pass(包括compute_memdep本身)都假定f中没有memdep信息
// 即指令序列中不存在MemPhi和MemOp，也没有任何指令use它们
// 所以compute_memdep的调用者还要负责调用clear_memdep
void compute_memdep(IrFunc *f);

#pragma once

#include "../../ir.hpp"

// 移除不可达的bb，同时重新填充BasicBlock::pred
void fill_pred(IrFunc *f);

#pragma once

#include "../structure/ast.hpp"
#include "../structure/ir.hpp"

IrProgram *convert_ssa(Program &p);

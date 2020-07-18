#pragma once

#include "ast.hpp"
#include "ir.hpp"

IrProgram *convert_ssa(Program &p);

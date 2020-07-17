#pragma once

#include "common.hpp"
#include <cstdint>
#include <string_view>
#include <vector>

#define REG_NONE 0
typedef u32 RegIndex;

struct Ir {
  enum {
    Add, Sub, Mul, Div, Mod, Lt, Le, Ge, Gt, Eq, Ne, And, Or, // Binary
    Neg, Not, Mv, // Unary
    Call, Index, LoadAddr
  } tag;
  RegIndex dest; // 目标寄存器编号，0表示没有
};

struct IrOperand {
  enum { R, C } kind;
  union { RegIndex reg; u32 imm; };
};

struct IrBinary : Ir {
  DEFINE_CLASSOF(Ir, Add <= p->tag && p->tag <= Or);
  IrOperand o1;
  IrOperand o2;
};

struct IrUnary : Ir {
  DEFINE_CLASSOF(Ir, Neg <= p->tag && p->tag <= Mv);
  IrOperand o1;
};

// call function
struct IrCall : Ir {
  DEFINE_CLASSOF(Ir, p->tag == Call);
  std::string_view func; // label of the function called
  std::vector<IrOperand *> args;
};

// get index of array
struct IrIndex : Ir {
  DEFINE_CLASSOF(Ir, p->tag == Index);
  RegIndex arr;
  std::vector<IrOperand *> dims;
};

// get addr of label in data section
struct IrLoadAddr : Ir {
  DEFINE_CLASSOF(Ir, p->tag == LoadAddr);
  std::string_view func;
};
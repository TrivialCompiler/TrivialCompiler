#pragma once

#include "common.hpp"
#include <cstdint>
#include <string_view>
#include <vector>

#define REG_NONE 0
typedef u32 RegIndex;

struct Value;
struct Instruction;

struct Use {
  Value *value;
  Inst *user;
};

struct Value {
  // value is used by ...
  std::vector<Use> uses;
  // lhs name
  std::string_view name;
};

struct IrFunc {};

struct BasicBlock {};

struct Const: Value {
  u32 imm;
};

struct Inst : Value {
  // operands
  std::vector<Use> operands;
  // instruction linked list
  Inst *prev;
  Inst *next;
  // basic block
  BasicBlock *bb;
};

struct BinaryInst: Inst {
  enum {
    Add, Sub, Mul, Div, Mod, Lt, Le, Ge, Gt, Eq, Ne, And, Or, // Binary
  } op;
};

struct UnaryInst: Inst {
  enum {
    Neg, Not, Mv, // Unary
  } op;
};

struct LoadInst: Inst {
};

struct StoreInst: Inst {
};

struct CallInst: Inst {
  IrFunc *func;
};

struct LoadAddrInst: Inst {
  std::string_view label;
};
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
  // tag
  enum {
    Add, Sub, Mul, Div, Mod, Lt, Le, Ge, Gt, Eq, Ne, And, Or, // Binary
    Neg, Not, Mv, // Unary
    Branch, Return,
    Load, Store, Call, LoadAddr,
    Const
  } tag;
};

struct IrFunc {};

struct BasicBlock {};

struct Const: Value {
  DEFINE_CLASSOF(Value, p->tag == Const);
  u32 imm;
};

struct Inst : Value {
  DEFINE_CLASSOF(Value, Add <= p->tag && p->tag <= LoadAddr);
  // operands
  std::vector<Use> operands;
  // instruction linked list
  Inst *prev;
  Inst *next;
  // basic block
  BasicBlock *bb;
};

struct BinaryInst: Inst {
  DEFINE_CLASSOF(Value, Add <= p->tag && p->tag <= Or);
};

struct UnaryInst: Inst {
  DEFINE_CLASSOF(Value, Neg <= p->tag && p->tag <= Mv);
};

struct LoadInst: Inst {
  DEFINE_CLASSOF(Value, p->tag == Load);
};

struct StoreInst: Inst {
  DEFINE_CLASSOF(Value, p->tag == Store);
};

struct CallInst: Inst {
  DEFINE_CLASSOF(Value, p->tag == Call);
  IrFunc *func;
};

struct LoadAddrInst: Inst {
  DEFINE_CLASSOF(Value, p->tag == LoadAddr);
  std::string_view label;
};

struct BranchInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Branch);
  BasicBlock *left;
  BasicBlock *right;
};

struct ReturnInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Return);
};
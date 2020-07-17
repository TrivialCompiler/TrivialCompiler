#pragma once

#include "common.hpp"
#include <cstdint>
#include <string_view>
#include <vector>

#define REG_NONE 0
typedef u32 RegIndex;

struct Value;
struct Inst;

struct Use {
  Value *value;
  Inst *user;

  Use(Value *v, Inst *u);
};

struct Value {
  // value is used by ...
  std::vector<Use> uses;
  // tag
  enum Tag {
    Add, Sub, Mul, Div, Mod, Lt, Le, Ge, Gt, Eq, Ne, And, Or, // Binary
    Neg, Not, Mv, // Unary
    Branch, Return,
    Load, Store, Call, LoadAddr,
    Const
  } tag;

  Value(Tag tag): tag(tag) {}

  void addUse(const Use &u);
  void killUse(const Use &u);
};

struct IrFunc {};

struct BasicBlock {
  Inst *first;
  Inst *last;
};

struct ConstValue: Value {
  DEFINE_CLASSOF(Value, p->tag == Const);
  u32 imm;
};

struct Inst : Value {
  DEFINE_CLASSOF(Value, Add <= p->tag && p->tag <= LoadAddr);
  // instruction linked list
  Inst *prev;
  Inst *next;
  // basic block
  BasicBlock *bb;

  // insert this inst before `insertBefore`
  Inst(Tag tag, Inst *insertBefore);

  // insert this inst at the end of `insertAtEnd`
  Inst(Tag tag, BasicBlock *insertAtEnd);
};

struct BinaryInst: Inst {
  DEFINE_CLASSOF(Value, Add <= p->tag && p->tag <= Or);
  // operands
  Use lhs;
  Use rhs;

  BinaryInst(Tag tag, Value *lhs, Value *rhs, Inst *insertBefore);
};

struct UnaryInst: Inst {
  DEFINE_CLASSOF(Value, Neg <= p->tag && p->tag <= Mv);
  // operands
  Use operand;
};

struct LoadInst: Inst {
  DEFINE_CLASSOF(Value, p->tag == Load);
  Use arr;
  std::vector<Use> dims;
};

struct StoreInst: Inst {
  DEFINE_CLASSOF(Value, p->tag == Store);
  Use arr;
  std::vector<Use> dims;
  Use data;
};

struct CallInst: Inst {
  DEFINE_CLASSOF(Value, p->tag == Call);
  IrFunc *func;
  std::vector<Use> args;
};

struct LoadAddrInst: Inst {
  DEFINE_CLASSOF(Value, p->tag == LoadAddr);
  std::string_view label;
};

struct BranchInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Branch);
  Use lhs;
  Use rhs;
  // eq
  BasicBlock *left;
  // ne
  BasicBlock *right;
};

struct ReturnInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Return);
  Use ret;
};
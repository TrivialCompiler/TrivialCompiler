#pragma once

#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

#include "ast.hpp"
#include "common.hpp"
#include "ilist.hpp"

struct Value;
struct Inst;
struct BasicBlock;
struct IrFunc;

struct Use {
  DEFINE_ILIST(Use)

  Value *value;
  Inst *user;

  Use(Value *v, Inst *u);
  ~Use();
};

struct Value {
  // value is used by ...
  ilist<Use> uses;
  // tag
  enum Tag {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Lt,
    Le,
    Ge,
    Gt,
    Eq,
    Ne,
    And,
    Or,  // Binary
    Neg,
    Not,
    Mv,  // Unary
    Branch,
    Return,
    Load,
    Store,
    Call,
    LoadAddr,
    Const,
    Alloca,
  } tag;

  Value(Tag tag) : tag(tag) {}

  void addUse(Use *u);
  void killUse(Use *u);
};

struct IrProgram {
  ilist<IrFunc> func;
};

struct IrFunc {
  DEFINE_ILIST(IrFunc)
  ilist<BasicBlock> bb;
  // mapping from decl to its value in this function
  std::map<Decl *, Value *> decls;
};

struct BasicBlock {
  DEFINE_ILIST(BasicBlock)
  ilist<Inst> insts;
};

struct ConstValue : Value {
  DEFINE_CLASSOF(Value, p->tag == Const);
  u32 imm;
};

struct Inst : Value {
  DEFINE_CLASSOF(Value, Add <= p->tag && p->tag <= LoadAddr);
  // instruction linked list
  DEFINE_ILIST(Inst)
  // basic block
  BasicBlock *bb;

  // insert this inst before `insertBefore`
  Inst(Tag tag, Inst *insertBefore);

  // insert this inst at the end of `insertAtEnd`
  Inst(Tag tag, BasicBlock *insertAtEnd);
};

struct BinaryInst : Inst {
  DEFINE_CLASSOF(Value, Add <= p->tag && p->tag <= Or);
  // operands
  Use lhs;
  Use rhs;

  BinaryInst(Tag tag, Value *lhs, Value *rhs, Inst *insertBefore);
  BinaryInst(Tag tag, Value *lhs, Value *rhs, BasicBlock *insertAtEnd);
};

struct UnaryInst : Inst {
  DEFINE_CLASSOF(Value, Neg <= p->tag && p->tag <= Mv);
  // operands
  Use operand;
};

struct LoadInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Load);
  Use arr;
  std::vector<Use> dims;
};

struct StoreInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Store);
  Use *arr;
  std::vector<Use *> dims;
  Use *data;

  StoreInst(BasicBlock *insertAtEnd)
      : Inst(Store, insertAtEnd), arr(nullptr), data(nullptr) {}
};

struct CallInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Call);
  IrFunc *func;
  std::vector<Use> args;
};

struct LoadAddrInst : Inst {
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

struct AllocaInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Alloca);

  AllocaInst(BasicBlock *insertBefore) : Inst(Alloca, insertBefore) {}
};

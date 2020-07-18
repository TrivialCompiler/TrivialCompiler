#pragma once

#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

#include "ast.hpp"
#include "common.hpp"
#include "ilist.hpp"
#include "typeck.hpp"

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
    Jump,
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

  IrFunc *findFunc(Func *func);
};

struct IrFunc {
  DEFINE_ILIST(IrFunc)
  Func *func;
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
  i32 imm;

  ConstValue(i32 imm) : Value(Const), imm(imm) {}
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

  BinaryInst(Tag tag, Value *lhs, Value *rhs, Inst *insertBefore)
      : Inst(tag, insertBefore), lhs(lhs, this), rhs(rhs, this) {}
  BinaryInst(Tag tag, Value *lhs, Value *rhs, BasicBlock *insertAtEnd)
      : Inst(tag, insertAtEnd), lhs(lhs, this), rhs(rhs, this) {}
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
  LoadInst(Value *arr, BasicBlock *insertAtEnd) : Inst(Load, insertAtEnd), arr(arr, this) {}
};

struct StoreInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Store);
  Use arr;
  std::vector<Use> dims;
  Use data;

  StoreInst(Value *arr, Value *data, BasicBlock *insertAtEnd)
      : Inst(Store, insertAtEnd), arr(arr, this), data(data, this) {}
};

struct CallInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Call);
  IrFunc *func;
  std::vector<Use> args;
  CallInst(IrFunc *func, BasicBlock *insertAtEnd) : Inst(Call, insertAtEnd), func(func) {}
};

struct LoadAddrInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == LoadAddr);
  std::string_view label;

  LoadAddrInst(Decl *decl, BasicBlock *insertAtEnd) : Inst(LoadAddr, insertAtEnd), label(decl->name) {}
};

struct BranchInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Branch);
  Use cond;
  // true
  BasicBlock *left;
  // false
  BasicBlock *right;

  BranchInst(Value *cond, BasicBlock *left, BasicBlock *right, BasicBlock *insertAtEnd)
      : Inst(Branch, insertAtEnd), cond(cond, this), left(left), right(right) {}
};

struct JumpInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Jump);
  BasicBlock *next;

  JumpInst(BasicBlock *next, BasicBlock *insertAtEnd) : Inst(Jump, insertAtEnd), next(next) {}
};

struct ReturnInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Return);
  Use ret;

  ReturnInst(Value *ret, BasicBlock *insertAtEnd) : Inst(Return, insertAtEnd), ret(ret, this) {}
};

struct AllocaInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Alloca);

  AllocaInst(BasicBlock *insertBefore) : Inst(Alloca, insertBefore) {}
};

void debug_print(IrProgram *p);

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <unordered_set>
#include <string_view>
#include <vector>

#include "ast.hpp"
#include "casting.hpp"
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
    Return,  // Control flow
    Load,
    Store,  // Memory
    Const,
    Global,
    Param,  // Reference
    Call,
    Alloca,
  } tag;

  Value(Tag tag) : tag(tag) {}

  void addUse(Use *u);
  void killUse(Use *u);
};

struct IrProgram {
  ilist<IrFunc> func;
  std::vector<Decl *> glob_decl;

  IrFunc *findFunc(Func *func);
  friend std::ostream& operator<<(std::ostream& os, const IrProgram& dt);
};

std::ostream& operator<<(std::ostream& os, const IrProgram& dt);

struct IrFunc {
  DEFINE_ILIST(IrFunc)
  Func *func;
  ilist<BasicBlock> bb;
  // mapping from decl to its value in this function
  std::map<Decl *, Value *> decls;
};

struct BasicBlock {
  DEFINE_ILIST(BasicBlock)
  std::vector<BasicBlock *> pred;
  std::unordered_set<BasicBlock *> dom; // 支配它的节点集
  BasicBlock *idom; // 直接支配它的节点
  ilist<Inst> insts;

  inline std::array<BasicBlock *, 2> succ();
};

struct ConstValue : Value {
  DEFINE_CLASSOF(Value, p->tag == Const);
  i32 imm;

  ConstValue(i32 imm) : Value(Const), imm(imm) {}
};

struct GlobalRef : Value {
  DEFINE_CLASSOF(Value, p->tag == Global);
  Decl *decl;

  GlobalRef(Decl *decl) : Value(Global), decl(decl) {}
};

struct ParamRef : Value {
  DEFINE_CLASSOF(Value, p->tag == Param);
  Decl *decl;

  ParamRef(Decl *decl) : Value(Param), decl(decl) {}
};

struct Inst : Value {
  DEFINE_CLASSOF(Value, Add <= p->tag && p->tag <= Alloca);
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
  Use rhs;
  UnaryInst(Tag tag, Value *rhs, BasicBlock *insertAtEnd) : Inst(tag, insertAtEnd), rhs(rhs, this) {}
};

struct LoadInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Load);
  Decl *lhs_sym;
  Use arr;
  std::vector<Use> dims;
  LoadInst(Decl *lhs_sym, Value *arr, BasicBlock *insertAtEnd) : Inst(Load, insertAtEnd), lhs_sym(lhs_sym), arr(arr, this) {}
};

struct StoreInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Store);
  Decl *lhs_sym;
  Use arr;
  std::vector<Use> dims;
  Use data;

  StoreInst(Decl *lhs_sym, Value *arr, Value *data, BasicBlock *insertAtEnd)
      : Inst(Store, insertAtEnd), lhs_sym(lhs_sym), arr(arr, this), data(data, this) {}
};

struct CallInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Call);
  // FIXME: IrFunc and Func 是什么关系？
  Func *func;
  std::vector<Use> args;
  CallInst(Func *func, BasicBlock *insertAtEnd) : Inst(Call, insertAtEnd), func(func) {}
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

std::array<BasicBlock *, 2> BasicBlock::succ() {
  Inst *end = insts.tail; // 必须非空
  if (auto x = dyn_cast<BranchInst>(end))return {x->left, x->right};
  else if (auto x = dyn_cast<JumpInst>(end)) return {x->next, nullptr};
  else if (auto x = dyn_cast<ReturnInst>(end)) return {nullptr, nullptr};
  else UNREACHABLE();
}
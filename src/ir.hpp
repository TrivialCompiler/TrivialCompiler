#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "casting.hpp"
#include "common.hpp"
#include "ilist.hpp"

// 声明ast中用到的类型，从而让这里不需要include "ast.hpp"。真正需要访问字段的文件里自己include
struct Func;
struct Decl;

struct Inst;
struct IrFunc;
struct Use;
struct MemPhiInst;

struct Value {
  // value is used by ...
  ilist<Use> uses;
  // tag
  enum Tag {
#include "op.inc"  // Binary
    Branch,
    Jump,
    Return,  // Control flow
    Load,
    Store,  // Memory
    Call,
    Alloca,
    Phi,
    MemOp,
    MemPhi, // 虚拟的MemPhi指令，保证不出现在指令序列中，只出现在BasicBlock::mem_phis中
    Const,
    Global,
    Param,
    Undef,  // Const ~ Undef: Reference
  } tag;

  Value(Tag tag) : tag(tag) {}

  void addUse(Use *u) { uses.insertAtEnd(u); }
  void killUse(Use *u) { uses.remove(u); }

  // 将对自身所有的使用替换成对v的使用
  inline void replaceAllUseWith(Value *v);
  // 调用deleteValue语义上相当于delete掉它，但是按照现在的实现不能直接delete它
  void deleteValue();
};

struct Use {
  DEFINE_ILIST(Use)

  Value *value;
  Inst *user;

  // 这个构造函数没有初始化prev和next，这没有关系
  // 因为prev和next永远不会从一个Use开始被主动使用，而是在遍历Use链表的时候用到
  // 而既然这个Use已经被加入了一个链表，它的prev和next也就已经被赋值了
  Use(Value *v, Inst *u) : value(v), user(u) {
    if (v) v->addUse(this);
  }

  void set(Value *v) {
    if (value) value->killUse(this);
    value = v;
    if (v) v->addUse(this);
  }

  ~Use() {
    if (value) value->killUse(this);
  }
};

void Value::replaceAllUseWith(Value *v) {
  // head->set会将head从链表中移除
  while (uses.head) uses.head->set(v);
}

struct IrProgram {
  ilist<IrFunc> func;
  std::vector<Decl *> glob_decl;

  friend std::ostream &operator<<(std::ostream &os, const IrProgram &dt);
};

std::ostream &operator<<(std::ostream &os, const IrProgram &dt);

struct BasicBlock {
  DEFINE_ILIST(BasicBlock)
  std::vector<BasicBlock *> pred;
  BasicBlock *idom;
  std::unordered_set<BasicBlock *> dom_by;  // 支配它的节点集
  std::vector<BasicBlock *> doms;           // 它支配的节点集
  u32 dom_level;                            // dom树中的深度，根深度为0
  bool vis;  // 各种算法中用到，标记是否访问过，算法开头应把所有vis置false(调用IrFunc::clear_all_vis)
  ilist<Inst> insts;
  ilist<Inst> mem_phis; // 元素都是MemPhiInst

  inline std::array<BasicBlock *, 2> succ();
  inline std::array<BasicBlock **, 2> succ_ref();  // 想修改succ时使用
  inline bool valid();
  inline ~BasicBlock();
};

struct IrFunc {
  DEFINE_ILIST(IrFunc)
  Func *func;
  ilist<BasicBlock> bb;
  // mapping from decl to its value in this function
  std::map<Decl *, Value *> decls;

  // 将所有bb的vis置false
  void clear_all_vis() {
    for (BasicBlock *b = bb.head; b; b = b->next) b->vis = false;
  }
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

struct UndefValue : Value {
  DEFINE_CLASSOF(Value, p->tag == Undef);

  UndefValue() : Value(Undef) {}
  // 这是一个全局可变变量，不过反正也不涉及多线程，不会有冲突
  static UndefValue INSTANCE;
};

struct Inst : Value {
  DEFINE_CLASSOF(Value, Add <= p->tag && p->tag <= MemPhi);
  // instruction linked list
  DEFINE_ILIST(Inst)
  // basic block
  BasicBlock *bb;

  // insert this inst before `insertBefore`
  Inst(Tag tag, Inst *insertBefore) : Value(tag), bb(insertBefore->bb) { bb->insts.insertBefore(this, insertBefore); }

  // insert this inst at the end of `insertAtEnd`
  Inst(Tag tag, BasicBlock *insertAtEnd) : Value(tag), bb(insertAtEnd) { bb->insts.insertAtEnd(this); }

  // 只初始化tag，没有加入到链表中，调用者手动加入
  Inst(Tag tag) : Value(tag) {}
};

struct BinaryInst : Inst {
  DEFINE_CLASSOF(Value, Add <= p->tag && p->tag <= Or);
  // operands
  Use lhs;
  Use rhs;

  BinaryInst(Tag tag, Value *lhs, Value *rhs, BasicBlock *insertAtEnd)
      : Inst(tag, insertAtEnd), lhs(lhs, this), rhs(rhs, this) {}
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

struct AccessInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Load || p->tag == Store);
  Decl *lhs_sym;
  Use arr;
  std::vector<Use> dims;
  AccessInst(Inst::Tag tag, Decl *lhs_sym, Value *arr, BasicBlock *insertAtEnd)
      : Inst(tag, insertAtEnd), lhs_sym(lhs_sym), arr(arr, this) {}
};

struct LoadInst : AccessInst {
  DEFINE_CLASSOF(Value, p->tag == Load);
  // 由memdep pass计算
  // 记录本条指令依赖的指令，这条指令不能在它依赖的指令之前执行，但是有可能它依赖的指令从来没有被执行
  // 这种依赖关系算作某种意义上的operand，所以用Use来表示(不完全一样，一般的operand的计算必须在本条指令之前)
  // LoadInst只可能依赖StoreInst, CallInst
//  std::vector<Inst *> dep;
//  std::unordered_set<Inst *> alias;
  Use mem_token;
  LoadInst(Decl *lhs_sym, Value *arr, BasicBlock *insertAtEnd) : AccessInst(Load, lhs_sym, arr, insertAtEnd), mem_token(nullptr, this) {}
};

struct StoreInst : AccessInst {
  DEFINE_CLASSOF(Value, p->tag == Store);
  Use data;
  StoreInst(Decl *lhs_sym, Value *arr, Value *data, BasicBlock *insertAtEnd)
      : AccessInst(Store, lhs_sym, arr, insertAtEnd), data(data, this) {}
};

struct CallInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Call);
  // FIXME: IrFunc and Func 是什么关系？
  Func *func;
  std::vector<Use> args;
  CallInst(Func *func, BasicBlock *insertAtEnd) : Inst(Call, insertAtEnd), func(func) {}
};

struct AllocaInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Alloca);

  Decl *sym;
  AllocaInst(Decl *sym, BasicBlock *insertBefore) : Inst(Alloca, insertBefore), sym(sym) {}
};

struct PhiInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == Phi);

  // incoming_values.size() == incoming_bbs.size()
  std::vector<Use> incoming_values;
  std::vector<BasicBlock *> *incoming_bbs;  // todo: 指向拥有它的bb的pred，这是正确的吗？

  explicit PhiInst(BasicBlock *insertAtFront)
      : Inst(Phi, insertAtFront->insts.head), incoming_bbs(&insertAtFront->pred) {
    incoming_values.reserve(insertAtFront->pred.size());
    for (u32 i = 0; i < insertAtFront->pred.size(); ++i) {
      // 在new PhiInst的时候还不知道它用到的value是什么，先填nullptr，后面再用Use::set填上
      incoming_values.emplace_back(nullptr, this);
    }
  }
};

struct MemOpInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == MemOp);
  Use mem_token;
  LoadInst *load;
  MemOpInst(LoadInst *load, Inst *insertBefore) : Inst(MemOp, insertBefore), load(load), mem_token(nullptr, this) {}
};

// 它的前几个字段和PhiInst是兼容的，所以可以当成PhiInst用(也许理论上有隐患，但是实际上应该没有问题)
// 我不希望让它继承PhiInst，这也许会影响以前的一些对PhiInst的使用
struct MemPhiInst : Inst {
  DEFINE_CLASSOF(Value, p->tag == MemPhi);

  // incoming_values.size() == incoming_bbs.size()
  std::vector<Use> incoming_values;
  std::vector<BasicBlock *> *incoming_bbs;

  // load依赖store和store依赖load两种依赖用到的MemPhiInst不一样
  // 前者的load_or_arr来自于load的数组地址，后者的load_or_arr来自于LoadInst
  Value *load_or_arr;

  explicit MemPhiInst(Value *load_or_arr, BasicBlock *insertAtFront)
    : Inst(MemPhi), incoming_bbs(&insertAtFront->pred), load_or_arr(load_or_arr) {
    bb = insertAtFront;
    bb->mem_phis.insertAtBegin(this);
    incoming_values.reserve(insertAtFront->pred.size());
    for (u32 i = 0; i < insertAtFront->pred.size(); ++i) {
      // 在new PhiInst的时候还不知道它用到的value是什么，先填nullptr，后面再用Use::set填上
      incoming_values.emplace_back(nullptr, this);
    }
  }
};

std::array<BasicBlock *, 2> BasicBlock::succ() {
  Inst *end = insts.tail;  // 必须非空
  if (auto x = dyn_cast<BranchInst>(end))
    return {x->left, x->right};
  else if (auto x = dyn_cast<JumpInst>(end))
    return {x->next, nullptr};
  else if (auto x = dyn_cast<ReturnInst>(end))
    return {nullptr, nullptr};
  else
    UNREACHABLE();
}

std::array<BasicBlock **, 2> BasicBlock::succ_ref() {
  Inst *end = insts.tail;
  if (auto x = dyn_cast<BranchInst>(end))
    return {&x->left, &x->right};
  else if (auto x = dyn_cast<JumpInst>(end))
    return {&x->next, nullptr};
  else if (auto x = dyn_cast<ReturnInst>(end))
    return {nullptr, nullptr};
  else
    UNREACHABLE();
}

bool BasicBlock::valid() {
  Inst *end = insts.tail;
  return end && (isa<BranchInst>(end) || isa<JumpInst>(end) || isa<ReturnInst>(end));
}

BasicBlock::~BasicBlock() {
  for (Inst *i = insts.head; i;) {
    Inst *next = i->next;
    i->deleteValue();
    i = next;
  }
}
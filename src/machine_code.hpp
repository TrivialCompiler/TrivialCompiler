#pragma once

#include <iostream>
#include <set>

#include "ast.hpp"
#include "common.hpp"
#include "ilist.hpp"
#include "ir.hpp"

struct MachineFunc;
struct MachineBB;
struct MachineInst;
struct MachineOperand;

// ref: https://en.wikipedia.org/wiki/Calling_convention#ARM_(A32)
enum ArmReg {
  // args and return value (caller saved)
  r0,
  r1,
  r2,
  r3,
  // local variables (callee saved)
  r4,
  r5,
  r6,
  r7,
  r8,
  r9,
  r10,
  r11,
  // ipc scratch register, what was that?
  r12,
  // stack pointer
  r13,
  // link register (caller saved)
  r14,
  // program counter
  r15,
};

enum ArmCond { Any, Eq, Ne, Ge, Gt, Le, Lt };

static std::ostream &operator<<(std::ostream &os, const ArmCond &cond) {
  if (cond == Eq) {
    os << "eq";
  } else if (cond == Ne) {
    os << "ne";
  } else if (cond == Any) {
    os << "";
  } else if (cond == Gt) {
    os << "gt";
  } else if (cond == Ge) {
    os << "ge";
  } else if (cond == Lt) {
    os << "lt";
  } else if (cond == Le) {
    os << "le";
  } else {
    UNREACHABLE();
  }
  return os;
}

struct MachineProgram {
  ilist<MachineFunc> func;
  std::vector<Decl *> glob_decl;
  friend std::ostream &operator<<(std::ostream &os, const MachineProgram &dt);
};

std::ostream &operator<<(std::ostream &os, const MachineProgram &dt);

struct MachineFunc {
  DEFINE_ILIST(MachineFunc)
  ilist<MachineBB> bb;
  IrFunc *func;
  // number of virtual registers allocated
  i32 virtual_max;
};

struct MachineBB {
  DEFINE_ILIST(MachineBB)
  ilist<MachineInst> insts;
  // predecessor and successor
  std::vector<MachineBB *> pred;
  std::array<MachineBB *, 2> succ;
  // branch is translated into multiple instructions
  // points to the first one
  MachineInst *control_transter_inst;
  // liveness analysis
  // maybe we should use bitset when performance is bad
  std::set<MachineOperand> liveuse;
  std::set<MachineOperand> def;
  std::set<MachineOperand> livein;
  std::set<MachineOperand> liveout;
};

struct MachineOperand {
  enum {
    PreColored,
    Allocated,
    Virtual,
    Immediate,
  } state;
  i32 value;

  bool operator<(const MachineOperand &other) const {
    if (state != other.state) {
      return state < other.state;
    } else {
      return value < other.value;
    }
  }

  bool operator==(const MachineOperand &other) const { return state == other.state && value == other.value; }

  bool operator!=(const MachineOperand &other) const { return state != other.state || value != other.value; }

  bool is_virtual() const { return state == Virtual; }
  bool is_precolored() const { return state == PreColored; }
  bool needs_color() const { return state == Virtual || state == PreColored; }

  explicit operator std::string() const {
    char prefix = '?';
    switch (this->state) {
      case PreColored:
      case Allocated:
        prefix = 'r';
        break;
      case Virtual:
        prefix = 'v';
        break;
      case Immediate:
        prefix = '#';
        break;
      default:
        UNREACHABLE();
    }
    return prefix + std::to_string(this->value);
  }

  friend std::ostream &operator<<(std::ostream &os, const MachineOperand &op) {
    os << std::string(op);
    return os;
  }
};

struct MachineInst {
  DEFINE_ILIST(MachineInst)
  MachineBB *bb;

  enum Tag {
#include "op.inc"
    // Binary
    Neg,
    Not,  // Unary
    Mv,
    Branch,
    Jump,
    Return,  // Control flow
    Load,
    Store,  // Memory
    Compare,
    Call,
    Global,
  } tag;

  MachineInst(Tag tag, MachineBB *insertAtEnd) : tag(tag), bb(insertAtEnd) { insertAtEnd->insts.insertAtEnd(this); }
  MachineInst(Tag tag, MachineInst *insertBefore) : tag(tag), bb(insertBefore->bb) {
    bb->insts.insertBefore(this, insertBefore);
  }
  MachineInst(Tag tag) : tag(tag) {}
};

struct MIBinary : MachineInst {
  DEFINE_CLASSOF(MachineInst, Add <= p->tag && p->tag <= Or);
  MachineOperand dst;
  MachineOperand lhs;
  MachineOperand rhs;

  MIBinary(Tag tag, MachineBB *insertAtEnd) : MachineInst(tag, insertAtEnd) {}
};

struct MIUnary : MachineInst {
  DEFINE_CLASSOF(MachineInst, Neg <= p->tag && p->tag <= Not);
  MachineOperand dst;
  MachineOperand rhs;

  MIUnary(Tag tag, MachineBB *insertAtEnd) : MachineInst(tag, insertAtEnd) {}
};

struct MIMove : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Mv);
  ArmCond cond;
  MachineOperand dst;
  MachineOperand rhs;

  MIMove(MachineBB *insertAtEnd) : MachineInst(Mv, insertAtEnd), cond(Any) {}
  MIMove(MachineInst *insertBefore) : MachineInst(Mv, insertBefore), cond(Any) {}
};

struct MIBranch : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Branch);
  ArmCond cond;
  MachineBB *target;
  MIBranch(MachineBB *insertAtEnd) : MachineInst(Branch, insertAtEnd) {}
};

struct MIJump : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Jump);
  MachineBB *target;

  MIJump(MachineBB *target, MachineBB *insertAtEnd) : MachineInst(Jump, insertAtEnd), target(target) {}
};

struct MIReturn : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Return);
  MIReturn(MachineBB *insertAtEnd) : MachineInst(Return, insertAtEnd) {}
};

struct MIAccess : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Load || p->tag == Store);
  enum Mode {
    Offset,
    Prefix,
    Postfix,
  } mode;
  MachineOperand addr;
  MachineOperand offset;
  i32 shift;
  MIAccess(MachineInst::Tag tag, MachineBB *insertAtEnd) : MachineInst(tag, insertAtEnd) {}
};

struct MILoad : MIAccess {
  DEFINE_CLASSOF(MachineInst, p->tag == Load);
  MachineOperand dst;

  MILoad(MachineBB *insertAtEnd) : MIAccess(Load, insertAtEnd) {}
};

struct MIStore : MIAccess {
  DEFINE_CLASSOF(MachineInst, p->tag == Store);
  MachineOperand data;

  MIStore(MachineBB *insertAtEnd) : MIAccess(Store, insertAtEnd) {}
};

struct MICompare : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Compare);
  MachineOperand lhs;
  MachineOperand rhs;

  MICompare(MachineBB *insertAtEnd) : MachineInst(Compare, insertAtEnd) {}
};

struct MICall : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Call);
  Func *func;

  MICall(MachineBB *insertAtEnd) : MachineInst(Call, insertAtEnd) {}
};

struct MIGlobal : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Global);
  MachineOperand dst;
  Decl *sym;

  MIGlobal(Decl *sym, MachineBB *insertAtBegin) : MachineInst(Global), sym(sym) {
    insertAtBegin->insts.insertAtBegin(this);
  }
};

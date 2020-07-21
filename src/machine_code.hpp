#pragma once

#include <iostream>

#include "ast.hpp"
#include "common.hpp"
#include "ilist.hpp"
#include "ir.hpp"

struct MachineFunc;
struct MachineBB;
struct MachineInst;

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
};

struct MachineBB {
  DEFINE_ILIST(MachineBB)
  ilist<MachineInst> insts;
};

struct MachineOperand {
  enum {
    PreColored,
    Allocated,
    Virtual,
    Immediate,
  } state;
  i32 value;

  friend std::ostream &operator<<(std::ostream &os, MachineOperand &op) {
    if (op.state == PreColored || op.state == Allocated) {
      os << "r" << op.value;
    } else if (op.state == op.Virtual) {
      os << "v" << op.value;
    } else if (op.state == Immediate) {
      os << "#" << op.value;
    }
    return os;
  }
};

struct MachineInst {
  DEFINE_ILIST(MachineInst)

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

  MachineInst(Tag tag, MachineBB *insertAtEnd) : tag(tag) { insertAtEnd->insts.insertAtEnd(this); }
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

struct MILoad : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Load);
  enum Mode {
    Offset,
    Prefix,
    Postfix,
  } mode;
  MachineOperand dst;
  MachineOperand addr;
  MachineOperand offset;
  i32 shift;

  MILoad(MachineBB *insertAtEnd) : MachineInst(Load, insertAtEnd) {}
};

struct MIStore : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Store);
  enum Mode {
    Offset,
    Prefix,
    Postfix,
  } mode;
  MachineOperand data;
  MachineOperand addr;
  MachineOperand offset;

  MIStore(MachineBB *insertAtEnd) : MachineInst(Store, insertAtEnd) {}
};

struct MICompare : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Compare);
  MachineOperand lhs;
  MachineOperand rhs;

  MICompare(MachineBB *insertAtEnd) : MachineInst(Compare, insertAtEnd) {}
};

struct MICall : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Call);
  MachineFunc *func;
};

struct MIGlobal : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Global);
  MachineOperand dst;
  Decl *sym;

  MIGlobal(Decl *sym, MachineBB *insertAtBegin) : MachineInst(Global), sym(sym) {
    insertAtBegin->insts.insertAtBegin(this);
  }
};
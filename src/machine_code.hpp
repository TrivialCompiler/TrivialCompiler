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
    Not,
    Mv,  // Unary
    Branch,
    Jump,
    Return,  // Control flow
    Load,
    Store,  // Memory
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
};

struct MIUnary : MachineInst {
  DEFINE_CLASSOF(MachineInst, Neg <= p->tag && p->tag <= Mv);
  MachineOperand dst;
  MachineOperand rhs;

  MIUnary(Tag tag, MachineBB *insertAtEnd) : MachineInst(tag, insertAtEnd) {}
};

struct MIBranch : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Branch);
  MachineOperand cond;
  // TODO: condition code
  MachineBB *target;
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
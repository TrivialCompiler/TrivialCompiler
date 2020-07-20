#pragma once

#include "ast.hpp"
#include "common.hpp"
#include "ilist.hpp"

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
};

struct MachineFunc {
  DEFINE_ILIST(MachineFunc)
  ilist<MachineBB> bb;
};

struct MachineBB {
  DEFINE_ILIST(MachineBB)
  ilist<MachineInst> bb;
};

struct MachineOperand {
  enum State {
    PreColored,
    Allocated,
    Virtual,
    Immediate,
  } state;
  i32 value;
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
    Return,  // Control flow
    Load,
    Store,  // Memory
    Call,
    Global,
  } tag;
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
};

struct MIBranch : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Branch);
  MachineOperand cond;
  // TODO: condition code
  MachineBB *target;
};

struct MIReturn : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Return);
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
};

struct MICall : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Call);
  MachineFunc *func;
};

struct MIGlobal : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Global);
  Decl *sym;
};
#pragma once

#include <cassert>
#include <iomanip>
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
enum class ArmReg {
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
  // frame pointer
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

const ArmReg fp = ArmReg::r11;
const ArmReg sp = ArmReg::r13;

enum class ArmCond { Any, Eq, Ne, Ge, Gt, Le, Lt };
struct ArmShift {
  enum {
    // no shifting
    None,
    // arithmetic right
    Asr,
    // logic left
    Lsl,
    // logic right
    Lsr,
    // rotate right
    Ror,
    // rotate right one bit with extend
    Rrx
  } type;
  int shift;

  ArmShift() {
    shift = 0;
    type = None;
  }
};

static std::ostream &operator<<(std::ostream &os, const ArmCond &cond) {
  if (cond == ArmCond::Eq) {
    os << "eq";
  } else if (cond == ArmCond::Ne) {
    os << "ne";
  } else if (cond == ArmCond::Any) {
    os << "";
  } else if (cond == ArmCond::Gt) {
    os << "gt";
  } else if (cond == ArmCond::Ge) {
    os << "ge";
  } else if (cond == ArmCond::Lt) {
    os << "lt";
  } else if (cond == ArmCond::Le) {
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
  i32 virtual_max = 0;
  // size of stack allocated for local alloca and spilled registers
  i32 sp_offset = 0;
};

struct MachineBB {
  DEFINE_ILIST(MachineBB)
  ilist<MachineInst> insts;
  // predecessor and successor
  std::vector<MachineBB *> pred;
  std::array<MachineBB *, 2> succ;
  // branch is translated into multiple instructions
  // points to the first one
  MachineInst *control_transfer_inst = nullptr;
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

  inline static MachineOperand R(ArmReg r) {
    auto n = (int)r;
    assert(n >= 0 && n <= 16);
    return MachineOperand{PreColored, n};
  }

  inline static MachineOperand V(int n) { return MachineOperand{Virtual, n}; }

  inline static MachineOperand I(int imm) { return MachineOperand{Immediate, imm}; }

  // both are PreColored or Allocated, and has the same value
  bool is_equiv(const MachineOperand &other) const {
    return (state == PreColored || state == Allocated) && (other.state == PreColored || state == Allocated) &&
           value == other.value;
  }

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
  bool is_imm() const { return state == Immediate; }
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

  enum class Tag {
#include "op.inc"
    // Binary
    LongMul,
    Mv,
    Branch,
    Jump,
    Return,  // Control flow
    Load,
    Store,  // Memory
    Compare,
    Call,
    Global,
    Comment,  // for printing comments
  } tag;

  MachineInst(Tag tag, MachineBB *insertAtEnd) : tag(tag), bb(insertAtEnd) {
    if (insertAtEnd) {
      insertAtEnd->insts.insertAtEnd(this);
    }
  }
  MachineInst(Tag tag, MachineInst *insertBefore) : tag(tag), bb(insertBefore->bb) {
    if (bb) {
      bb->insts.insertBefore(this, insertBefore);
    }
  }
  MachineInst(Tag tag) : tag(tag) {}
};

struct MIBinary : MachineInst {
  DEFINE_CLASSOF(MachineInst, Tag::Add <= p->tag && p->tag <= Tag::Or);
  MachineOperand dst;
  MachineOperand lhs;
  MachineOperand rhs;

  MIBinary(Tag tag, MachineBB *insertAtEnd) : MachineInst(tag, insertAtEnd) {}
};

// 应该有四种，但是现在只用到一种UMULL，所以也没有额外定义来区分它们
// 我们只用到UMULL结果的高32位，低32位的结果写入r12，这个寄存器的值我们不在乎
struct MILongMul : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::LongMul);
  MachineOperand dst_hi;
  MachineOperand lhs;
  MachineOperand rhs;

  MILongMul(MachineBB *insertAtEnd) : MachineInst(Tag::LongMul, insertAtEnd) {}
};

struct MIMove : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::Mv);
  ArmCond cond;
  MachineOperand dst;
  MachineOperand rhs;
  ArmShift shift;

  bool is_simple() { return cond == ArmCond::Any && shift.type == ArmShift::None; }

  MIMove(MachineBB *insertAtEnd) : MachineInst(Tag::Mv, insertAtEnd), cond(ArmCond::Any) {}
  MIMove(MachineBB *insertAtBegin, int) : MachineInst(Tag::Mv), cond(ArmCond::Any) {
    if (insertAtBegin) {
      bb = insertAtBegin;
      insertAtBegin->insts.insertAtBegin(this);
    }
  }
  MIMove(MachineInst *insertBefore) : MachineInst(Tag::Mv, insertBefore), cond(ArmCond::Any) {}
};

struct MIBranch : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::Branch);
  ArmCond cond;
  MachineBB *target;
  MIBranch(MachineBB *insertAtEnd) : MachineInst(Tag::Branch, insertAtEnd) {}
};

struct MIJump : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::Jump);
  MachineBB *target;

  MIJump(MachineBB *target, MachineBB *insertAtEnd) : MachineInst(Tag::Jump, insertAtEnd), target(target) {}
};

struct MIReturn : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::Return);
  MIReturn(MachineBB *insertAtEnd) : MachineInst(Tag::Return, insertAtEnd) {}
};

struct MIAccess : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::Load || p->tag == Tag::Store);
  enum class Mode {
    Offset,
    Prefix,
    Postfix,
  } mode;
  MachineOperand addr;
  MachineOperand offset;
  i32 shift;
  MIAccess(MachineInst::Tag tag, MachineBB *insertAtEnd) : MachineInst(tag, insertAtEnd) {}
  MIAccess(MachineInst::Tag tag, MachineInst *insertBefore) : MachineInst(tag, insertBefore) {}
  MIAccess(MachineInst::Tag tag) : MachineInst(tag) {}
};

struct MILoad : MIAccess {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::Load);
  MachineOperand dst;

  MILoad(MachineBB *insertAtEnd) : MIAccess(Tag::Load, insertAtEnd) {}
  MILoad(MachineInst *insertBefore) : MIAccess(Tag::Load, insertBefore) {}
  MILoad(MachineBB *insertAtBegin, int) : MIAccess(Tag::Load) {
    bb = insertAtBegin;
    insertAtBegin->insts.insertAtBegin(this);
  }
};

struct MIStore : MIAccess {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::Store);
  MachineOperand data;

  MIStore(MachineBB *insertAtEnd) : MIAccess(Tag::Store, insertAtEnd) {}
  MIStore() : MIAccess(Tag::Store) {}
};

struct MICompare : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::Compare);
  MachineOperand lhs;
  MachineOperand rhs;

  MICompare(MachineBB *insertAtEnd) : MachineInst(Tag::Compare, insertAtEnd) {}
};

struct MICall : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::Call);
  Func *func;

  MICall(MachineBB *insertAtEnd) : MachineInst(Tag::Call, insertAtEnd) {}
};

struct MIGlobal : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::Global);
  MachineOperand dst;
  Decl *sym;

  MIGlobal(Decl *sym, MachineBB *insertAtBegin) : MachineInst(Tag::Global), sym(sym) {
    insertAtBegin->insts.insertAtBegin(this);
  }
};

struct MIComment : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::Comment);
  std::string content;

  MIComment(std::string &&content, MachineBB *insertAtEnd) : MachineInst(Tag::Comment, insertAtEnd), content(content) {}
};

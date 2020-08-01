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
  // special purposes
  r11,
  r12,
  r13,
  r14,
  r15,
  // some aliases
  fp = 11,  // frame pointer
  ip = r12, // ipc scratch register, used in some instructions (caller saved)
  sp = r13, // stack pointer
  lr = r14, // link register (caller saved)
  pc = r15, // program counter
};

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

  explicit operator std::string() const{
    const char* name;
    switch (type) {
      case ArmShift::Asr:
        name = "asr";
        break;
      case ArmShift::Lsl:
        name = "lsl";
        break;
      case ArmShift::Lsr:
        name = "lsr";
        break;
      case ArmShift::Ror:
        name = "ror";
        break;
      case ArmShift::Rrx:
        name = "rrx";
        break;
      default:
        UNREACHABLE();
    }
    return std::string(name) + " #" + std::to_string(shift);
  }
};

static std::ostream &operator<<(std::ostream &os, const ArmShift &shift) {
  os << std::string(shift);
  return os;
}


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
  enum class State {
    PreColored,
    Allocated,
    Virtual,
    Immediate,
  } state;
  i32 value;

  inline static MachineOperand R(ArmReg r) {
    auto n = (int)r;
    assert(n >= 0 && n <= 16);
    return MachineOperand{State::PreColored, n};
  }

  inline static MachineOperand V(int n) { return MachineOperand{State::Virtual, n}; }

  inline static MachineOperand I(int imm) { return MachineOperand{State::Immediate, imm}; }

  // both are PreColored or Allocated, and has the same value
  bool is_equiv(const MachineOperand &other) const {
    return (state == State::PreColored || state == State::Allocated) && (other.state == State::PreColored || other.state == State::Allocated) &&
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

  bool is_virtual() const { return state == State::Virtual; }
  bool is_imm() const { return state == State::Immediate; }
  bool is_precolored() const { return state == State::PreColored; }
  bool needs_color() const { return state == State::Virtual || state == State::PreColored; }

  explicit operator std::string() const {
    char prefix = '?';
    switch (this->state) {
      case State::PreColored:
      case State::Allocated:
        prefix = 'r';
        break;
      case State::Virtual:
        prefix = 'v';
        break;
      case State::Immediate:
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
    FMA,
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
  // Add, Sub, Rsb, Mul, Div, Mod, Lt, Le, Ge, Gt, Eq, Ne, And, Or, LongMul, FMA
  DEFINE_CLASSOF(MachineInst, Tag::Add <= p->tag && p->tag <= Tag::Or);
  MachineOperand dst;
  MachineOperand lhs;
  MachineOperand rhs;
  ArmShift shift;

  MIBinary(Tag tag, MachineBB *insertAtEnd) : MachineInst(tag, insertAtEnd) {}

  bool isIdentity() {
    switch (tag) {
      case Tag::Add:
      case Tag::Sub:
        return dst.is_equiv(lhs) && rhs == MachineOperand::I(0) && shift.type == ArmShift::None;
      default:
        return false;
    }
  }
};

struct MITernary : MachineInst {
  // LongMul, FMA
  DEFINE_CLASSOF(MachineInst, Tag::LongMul <= p->tag && p->tag <= Tag::FMA);
  MachineOperand lhs;
  MachineOperand rhs;

  MITernary(Tag tag, MachineBB *insertAtEnd) : MachineInst(tag, insertAtEnd) {}
};

struct MILongMul : MITernary {
  DEFINE_CLASSOF(MachineInst, Tag::LongMul == p->tag);
  MachineOperand dst_lo;
  MachineOperand dst_hi;

  explicit MILongMul(MachineBB *insertAtEnd) : MITernary(Tag::LongMul, insertAtEnd) {
    dst_lo = MachineOperand::R(ArmReg::ip);
  }
};

// FIXME: may not be correct if final result is negative
struct MIFma : MITernary {
  DEFINE_CLASSOF(MachineInst, Tag::FMA == p->tag);
  MachineOperand acc;
  MachineOperand dst;

  explicit MIFma(MachineBB *insertAtEnd) : MITernary(Tag::FMA, insertAtEnd) {}
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

  explicit MIStore(MachineBB *insertAtEnd) : MIAccess(Tag::Store, insertAtEnd) {}
  MIStore() : MIAccess(Tag::Store) {}
};

struct MICompare : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::Compare);
  MachineOperand lhs;
  MachineOperand rhs;

  explicit MICompare(MachineBB *insertAtEnd) : MachineInst(Tag::Compare, insertAtEnd) {}
};

struct MICall : MachineInst {
  DEFINE_CLASSOF(MachineInst, p->tag == Tag::Call);
  Func *func;

  explicit MICall(MachineBB *insertAtEnd) : MachineInst(Tag::Call, insertAtEnd) {}
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

#include "ir.hpp"

Use::Use(Value *v, Inst *u) : value(v), user(u) {
  if (value) {
    value->addUse(this);
  }
}

Use::~Use() {
  if (value) {
    value->killUse(this);
  }
}

void Value::addUse(Use *u) { uses.insertAtEnd(u); }

void Value::killUse(Use *u) {
  uses.remove(u);
}

Inst::Inst(Tag tag, Inst *insertBefore) : Value(tag) {
  bb = insertBefore->bb;
  bb->insts.insertBefore(this, insertBefore);
}

Inst::Inst(Tag tag, BasicBlock *insertAtEnd) : Value(tag) {
  bb = insertAtEnd;
  bb->insts.insertAtEnd(this);
}

BinaryInst::BinaryInst(Tag tag, Value *lhs, Value *rhs, Inst *insertBefore)
    : Inst(tag, insertBefore), lhs(lhs, this), rhs(rhs, this) {}
BinaryInst::BinaryInst(Tag tag, Value *lhs, Value *rhs, BasicBlock *insertAtEnd)
    : Inst(tag, insertAtEnd), lhs(lhs, this), rhs(rhs, this) {}

#include "ir.hpp"

Use::Use(Value *v, Inst *u) : value(v), user(u) {
  if (value) {
    value->addUse(*this);
  }
}

Use::~Use() {
  if (value) {
    value->killUse(*this);
  }
}

void Value::addUse(const Use &u) { uses.push_back(u); }

void Value::killUse(const Use &u) {
  for (auto it = uses.cbegin(); it != uses.cend(); it++) {
    if (it->value == u.value && it->user == u.user) {
      uses.erase(it);
      break;
    }
  }
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

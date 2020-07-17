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
  prev = insertBefore->prev;
  next = insertBefore;
  insertBefore->prev = this;

  if (bb->first == insertBefore) {
    bb->first = this;
  }
}

Inst::Inst(Tag tag, BasicBlock *insertAtEnd) : Value(tag) {
  bb = insertAtEnd;
  prev = bb->last;
  next = nullptr;

  // only this instruction
  if (bb->last == nullptr) {
    bb->first = bb->last = this;
  } else {
    bb->last->next = this;
    bb->last = this;
  }
}

BinaryInst::BinaryInst(Tag tag, Value *lhs, Value *rhs, Inst *insertBefore)
    : Inst(tag, insertBefore), lhs(lhs, this), rhs(rhs, this) {}
BinaryInst::BinaryInst(Tag tag, Value *lhs, Value *rhs, BasicBlock *insertAtEnd)
    : Inst(tag, insertAtEnd), lhs(lhs, this), rhs(rhs, this) {}
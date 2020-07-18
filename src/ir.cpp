#include "ir.hpp"

#include "casting.hpp"

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

void Value::killUse(Use *u) { uses.remove(u); }

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

template <class T>
struct IndexMapper {
  std::map<T *, u32> mapping;
  u32 index_max = 0;

  u32 get(T *t) {
    auto it = mapping.find(t);
    if (it != mapping.end()) {
      return it->second;
    } else {
      return mapping[t] = index_max++;
    }
  }
};

void debug_print(IrProgram *p) {
  using namespace std;
  for (auto f = p->func.head; f != nullptr; f = f->next) {
    cout << "function " << f->func->name << " {" << endl;

    IndexMapper<BasicBlock> bb_index;
    IndexMapper<Value> v_index;
    for (auto bb = f->bb.head; bb != nullptr; bb = bb->next) {
      u32 index = bb_index.get(bb);
      cout << "_" << index << ":" << endl;
      for (auto inst = bb->insts.head; inst != nullptr; inst = inst->next) {
        u32 index = v_index.get(inst);
        cout << "\t";
        if (auto x = dyn_cast<AllocaInst>(inst)) {
          cout << "%" << index << " = alloca" << endl;
        } else if (auto x = dyn_cast<StoreInst>(inst)) {
          cout << "store %" << v_index.get(x->arr->value) << ", %" << v_index.get(x->data->value) << endl;
        } else {
          UNREACHABLE();
        }
      }
    }

    cout << "}" << endl;
  }
}

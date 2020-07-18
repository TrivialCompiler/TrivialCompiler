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
          // TODO: dims
          cout << "store %" << v_index.get(x->data.value) << ", %" << v_index.get(x->arr.value) << endl;
        } else if (auto x = dyn_cast<BinaryInst>(inst)) {
          const char *op = "unknown";
          switch (x->tag) {
            case Value::Add:
              op = "+";
              break;
            case Value::Sub:
              op = "+";
              break;
            case Value::Mul:
              op = "*";
              break;
            case Value::Div:
              op = "/";
              break;
            case Value::Mod:
              op = "%";
              break;
            case Value::Lt:
              op = "<";
              break;
            case Value::Le:
              op = "<=";
              break;
            case Value::Ge:
              op = ">=";
              break;
            case Value::Gt:
              op = ">";
              break;
            case Value::Eq:
              op = "==";
              break;
            case Value::Ne:
              op = "!=";
              break;
            case Value::And:
              op = "&&";
              break;
            case Value::Or:
              op = "||";
              break;
            default:
              break;
          }

          cout << "%" << v_index.get(inst) << " = %" << v_index.get(x->lhs.value) << " " << op << " "
               << v_index.get(x->rhs.value) << endl;
        } else if (auto x = dyn_cast<JumpInst>(inst)) {
          cout << "j _" << bb_index.get(x->next) << endl;
        } else if (auto x = dyn_cast<BranchInst>(inst)) {
          cout << "br %" << v_index.get(x->cond.value) << ", _" << bb_index.get(x->left) << ", _"
               << bb_index.get(x->right) << endl;
        } else {
          UNREACHABLE();
        }
      }
    }

    cout << "}" << endl << endl;
  }
}

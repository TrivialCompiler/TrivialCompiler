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

IrFunc *IrProgram::findFunc(Func *f) {
  for (auto p = func.head; p; p = p->next) {
    if (p->func == f) {
      return p;
    }
  }
  return nullptr;
}

// print value
const char *pv(IndexMapper<Value> &v_index, Value *v) {
  if (auto x = dyn_cast<ConstValue>(v)) {
    std::cout << x->imm;
  } else if (auto x = dyn_cast<GlobalRef>(v)) {
    std::cout << "@" << x->decl->name;
  } else {
    std::cout << "%x" << v_index.get(v);
  }
  return "";
}

void debug_print(IrProgram *p) {
  using namespace std;
  for (auto &d : p->glob_decl) {
    cout << "@" << d->name << " = global i32 0" << endl;
  }

  for (auto f = p->func.head; f != nullptr; f = f->next) {
    if (f->func->is_int) {
      cout << "define i32 @";
    } else {
      cout << "define void @";
    }
    cout << f->func->name << "() {" << endl;

    IndexMapper<BasicBlock> bb_index;
    IndexMapper<Value> v_index;
    for (auto bb = f->bb.head; bb != nullptr; bb = bb->next) {
      u32 index = bb_index.get(bb);
      cout << "_" << index << ":" << endl;
      for (auto inst = bb->insts.head; inst != nullptr; inst = inst->next) {
        cout << "\t";
        if (auto x = dyn_cast<AllocaInst>(inst)) {
          cout << pv(v_index, inst) << " = alloca i32, align 4" << endl;
        } else if (auto x = dyn_cast<StoreInst>(inst)) {
          // TODO: dims
          cout << "store i32 " << pv(v_index, x->data.value) << ", i32 *" << pv(v_index, x->arr.value) << ", align 4"
               << endl;
        } else if (auto x = dyn_cast<LoadInst>(inst)) {
          // TODO: dims
          cout << pv(v_index, inst) << " = load i32, i32* " << pv(v_index, x->arr.value) << ", align 4" << endl;
        } else if (auto x = dyn_cast<BinaryInst>(inst)) {
          const char *op = "unknown";
          switch (x->tag) {
            case Value::Add:
              op = "add";
              break;
            case Value::Sub:
              op = "sub";
              break;
            case Value::Mul:
              op = "mul";
              break;
            case Value::Div:
              op = "div";
              break;
            case Value::Mod:
              op = "mod";
              break;
            case Value::Lt:
              op = "icmp lt";
              break;
            case Value::Le:
              op = "icmp le";
              break;
            case Value::Ge:
              op = "icmp ge";
              break;
            case Value::Gt:
              op = "icmp gt";
              break;
            case Value::Eq:
              op = "icmp eq";
              break;
            case Value::Ne:
              op = "icmp ne";
              break;
            case Value::And:
              op = "and";
              break;
            case Value::Or:
              op = "or";
              break;
            default:
              break;
          }

          cout << pv(v_index, inst) << " = " << op << " i32 " << pv(v_index, x->lhs.value) << ", "
               << pv(v_index, x->rhs.value) << endl;
        } else if (auto x = dyn_cast<JumpInst>(inst)) {
          cout << "br label %_" << bb_index.get(x->next) << endl;
        } else if (auto x = dyn_cast<BranchInst>(inst)) {
          cout << "br i1 " << pv(v_index, x->cond.value) << ", label %_" << bb_index.get(x->left) << ", label %_"
               << bb_index.get(x->right) << endl;
        } else if (auto x = dyn_cast<ReturnInst>(inst)) {
          if (x->ret.value) {
            cout << "ret i32 " << pv(v_index, x->ret.value) << endl;
          } else {
            cout << "ret void" << endl;
          }
        } else if (auto x = dyn_cast<CallInst>(inst)) {
          cout << pv(v_index, inst) << " = call i32 @" << x->func->func->name << "()" << endl;
        } else {
          UNREACHABLE();
        }
      }
    }

    cout << "}" << endl << endl;
  }
}

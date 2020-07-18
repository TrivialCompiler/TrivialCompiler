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

  u32 alloc() { return index_max++; }

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
  } else if (auto x = dyn_cast<ParamRef>(v)) {
    std::cout << "%" << x->decl->name;
  } else {
    std::cout << "%x" << v_index.get(v);
  }
  return "";
}

void debug_print(IrProgram *p) {
  using namespace std;
  // builtin functions
  cout << "declare i32 @getint()" << endl;
  cout << "declare void @putint(i32)" << endl;
  for (auto &d : p->glob_decl) {
    if (d->has_init) {
      if (d->init.val1) {
        cout << "@" << d->name << " = global i32 " << d->init.val1->result << endl;
      }
    } else {
      // default 0 initialized
      cout << "@" << d->name << " = global i32 0" << endl;
    }
  }

  for (auto f = p->func.head; f != nullptr; f = f->next) {
    if (f->func->is_int) {
      cout << "define i32 @";
    } else {
      cout << "define void @";
    }
    cout << f->func->name << "(";
    for (auto &p : f->func->params) {
      cout << "i32 %" << p.name;
      if (&p != &f->func->params.back()) {
        // not last element
        cout << ", ";
      }
    }
    cout << ") {" << endl;

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
          cout << "store i32 " << pv(v_index, x->data.value) << ", i32* " << pv(v_index, x->arr.value) << ", align 4"
               << endl;
        } else if (auto x = dyn_cast<LoadInst>(inst)) {
          // TODO: dims
          cout << pv(v_index, inst) << " = load i32, i32* " << pv(v_index, x->arr.value) << ", align 4" << endl;
        } else if (auto x = dyn_cast<BinaryInst>(inst)) {
          const char *op = "unknown";
          bool conversion = false;
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
              op = "sdiv";
              break;
            case Value::Mod:
              op = "srem";
              break;
            case Value::Lt:
              op = "icmp slt";
              conversion = true;
              break;
            case Value::Le:
              op = "icmp sle";
              conversion = true;
              break;
            case Value::Ge:
              op = "icmp sge";
              conversion = true;
              break;
            case Value::Gt:
              op = "icmp sgt";
              conversion = true;
              break;
            case Value::Eq:
              op = "icmp eq";
              conversion = true;
              break;
            case Value::Ne:
              op = "icmp ne";
              conversion = true;
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
          if (conversion) {
            u32 temp = v_index.alloc();
            cout << "%t" << temp << " = " << op << " i32 " << pv(v_index, x->lhs.value) << ", "
                 << pv(v_index, x->rhs.value) << endl;
            cout << "\t" << pv(v_index, inst) << " = "
                 << "zext i1 "
                 << "%t" << temp << " to i32" << endl;
          } else {
            cout << pv(v_index, inst) << " = " << op << " i32 " << pv(v_index, x->lhs.value) << ", "
                 << pv(v_index, x->rhs.value) << endl;
          }
        } else if (auto x = dyn_cast<UnaryInst>(inst)) {
          switch (x->tag) {
            case Value::Neg:
              cout << pv(v_index, inst) << " = sub i32 0, " << pv(v_index, x->rhs.value) << endl;
              break;
            default:
              UNREACHABLE();
              break;
          }
        } else if (auto x = dyn_cast<JumpInst>(inst)) {
          cout << "br label %_" << bb_index.get(x->next) << endl;
        } else if (auto x = dyn_cast<BranchInst>(inst)) {
          u32 temp = v_index.alloc();
          cout << "%t" << temp << " = icmp ne i32 " << pv(v_index, x->cond.value) << ", 0" << endl;
          cout << "\tbr i1 %t" << temp << ", label %_" << bb_index.get(x->left) << ", label %_"
               << bb_index.get(x->right) << endl;
        } else if (auto x = dyn_cast<ReturnInst>(inst)) {
          if (x->ret.value) {
            cout << "ret i32 " << pv(v_index, x->ret.value) << endl;
          } else {
            cout << "ret void" << endl;
          }
        } else if (auto x = dyn_cast<CallInst>(inst)) {
          if (x->func->is_int) {
            cout << pv(v_index, inst) << " = call i32";
          } else {
            cout << "call void";
          }
          cout << " @" << x->func->name << "(";
          for (auto &p : x->args) {
            cout << "i32 " << pv(v_index, p.value);
            if (&p != &x->args.back()) {
              // not last element
              cout << ", ";
            }
          }
          cout << ")" << endl;
        } else {
          UNREACHABLE();
        }
      }
    }

    cout << "}" << endl << endl;
  }
}

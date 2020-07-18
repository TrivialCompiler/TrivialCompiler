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
    auto [it, inserted] = mapping.insert({t, index_max});
    index_max += inserted;
    return it->second;
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


// output IR
std::ostream& operator<<(std::ostream& os, const IrProgram& p) {
  using std::endl;
  // builtin functions
  os << "declare i32 @getint()" << endl;
  os << "declare void @putint(i32)" << endl;
  for (auto &d : p.glob_decl) {
    if (d->has_init) {
      if (d->init.val1) {
        os << "@" << d->name << " = global i32 " << d->init.val1->result << endl;
      }
    } else {
      // default 0 initialized
      os << "@" << d->name << " = global i32 0" << endl;
    }
  }

  for (auto f = p.func.head; f != nullptr; f = f->next) {
    if (f->func->is_int) {
      os << "define i32 @";
    } else {
      os << "define void @";
    }
    os << f->func->name << "(";
    for (auto &p : f->func->params) {
      os << "i32 %" << p.name;
      if (&p != &f->func->params.back()) {
        // not last element
        os << ", ";
      }
    }
    os << ") {" << endl;

    IndexMapper<BasicBlock> bb_index;
    IndexMapper<Value> v_index;
    for (auto bb = f->bb.head; bb != nullptr; bb = bb->next) {
      u32 index = bb_index.get(bb);
      os << "_" << index << ":" << endl;
      for (auto inst = bb->insts.head; inst != nullptr; inst = inst->next) {
        os << "\t";
        if (auto x = dyn_cast<AllocaInst>(inst)) {
          os << pv(v_index, inst) << " = alloca i32, align 4" << endl;
        } else if (auto x = dyn_cast<StoreInst>(inst)) {
          // TODO: dims
          os << "store i32 " << pv(v_index, x->data.value) << ", i32* " << pv(v_index, x->arr.value) << ", align 4"
               << endl;
        } else if (auto x = dyn_cast<LoadInst>(inst)) {
          // TODO: dims
          os << pv(v_index, inst) << " = load i32, i32* " << pv(v_index, x->arr.value) << ", align 4" << endl;
        } else if (auto x = dyn_cast<BinaryInst>(inst)) {
          const static char *OPS[] = {
              [Value::Add] = "add",     [Value::Sub] = "sub",     [Value::Mul] = "mul",     [Value::Div] = "sdiv",
              [Value::Mod] = "srem",    [Value::Lt] = "icmp slt", [Value::Le] = "icmp sle", [Value::Ge] = "icmp sge",
              [Value::Gt] = "icmp sgt", [Value::Eq] = "icmp eq",  [Value::Ne] = "icmp ne",  [Value::And] = "and",
              [Value::Or] = "or",
          };
          const char *op = OPS[x->tag];
          bool conversion = Value::Lt <= x->tag && x->tag <= Value::Ne;
          // add comment
          os << "; " << pv(v_index, inst) << " = " << pv(v_index, x->lhs.value) << " " << op << " "
               << pv(v_index, x->rhs.value) << endl;
          if (conversion) {
            u32 temp = v_index.alloc();
            os << "\t%t" << temp << " = " << op << " i32 " << pv(v_index, x->lhs.value) << ", "
                 << pv(v_index, x->rhs.value) << endl;
            os << "\t" << pv(v_index, inst) << " = "
                 << "zext i1 "
                 << "%t" << temp << " to i32" << endl;
          } else {
            os << "\t" << pv(v_index, inst) << " = " << op << " i32 " << pv(v_index, x->lhs.value) << ", "
                 << pv(v_index, x->rhs.value) << endl;
          }
        } else if (auto x = dyn_cast<UnaryInst>(inst)) {
          switch (x->tag) {
            case Value::Neg:
              os << pv(v_index, inst) << " = sub i32 0, " << pv(v_index, x->rhs.value) << endl;
              break;
            default:
              UNREACHABLE();
              break;
          }
        } else if (auto x = dyn_cast<JumpInst>(inst)) {
          os << "br label %_" << bb_index.get(x->next) << endl;
        } else if (auto x = dyn_cast<BranchInst>(inst)) {
          // add comment
          os << "; if " << pv(v_index, x->cond.value) << " then _" << bb_index.get(x->left) << " else _"
               << bb_index.get(x->right) << endl;
          u32 temp = v_index.alloc();
          os << "\t%t" << temp << " = icmp ne i32 " << pv(v_index, x->cond.value) << ", 0" << endl;
          os << "\tbr i1 %t" << temp << ", label %_" << bb_index.get(x->left) << ", label %_"
               << bb_index.get(x->right) << endl;
        } else if (auto x = dyn_cast<ReturnInst>(inst)) {
          if (x->ret.value) {
            os << "ret i32 " << pv(v_index, x->ret.value) << endl;
          } else {
            os << "ret void" << endl;
          }
        } else if (auto x = dyn_cast<CallInst>(inst)) {
          if (x->func->is_int) {
            os << pv(v_index, inst) << " = call i32";
          } else {
            os << "call void";
          }
          os << " @" << x->func->name << "(";
          for (auto &p : x->args) {
            os << "i32 " << pv(v_index, p.value);
            if (&p != &x->args.back()) {
              // not last element
              os << ", ";
            }
          }
          os << ")" << endl;
        } else {
          UNREACHABLE();
        }
      }
    }

    os << "}" << endl << endl;
  }

  return os;
}

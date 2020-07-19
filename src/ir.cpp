#include "casting.hpp"
#include "ir.hpp"

#include <sstream>

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

void print_dims(std::ostream &os, Expr **dims, Expr **dims_end) {
  if (dims == dims_end) {
    os << "i32 ";
  } else if (dims[0] == nullptr) {
    // just skip
    print_dims(os, dims + 1, dims_end);
  } else {
    for (Expr **d = dims; d != dims_end; d++) {
      int dim = d[0]->result;
      if (d + 1 != dims_end) {
        dim /= d[1]->result;
      }

      os << "[" << dim << " x ";
    }
    os << "i32";
    for (Expr **d = dims; d != dims_end; d++) {
      os << "]";
    }
    os << " ";
  }
}

void print_flatten_init(std::ostream &os, Expr **dims, Expr **dims_end, Expr **flatten_init, Expr **flatten_init_end) {
  if (dims == dims_end) {
    // last dim
    os << flatten_init[0]->result;
  } else {
    // one or more dims
    os << "[";
    int count = dims[0]->result;
    int element = 1;
    if (dims + 1 != dims_end) {
      count /= dims[1]->result;
      element = dims[1]->result;
    }

    for (int i = 0; i < count; i++) {
      // print type from dims[1]
      print_dims(os, dims + 1, dims_end);
      // recursive
      print_flatten_init(os, dims + 1, dims_end, flatten_init + i * element, flatten_init_end);

      if (i + 1 < count) {
        os << ", ";
      }
    }
    os << "]";
  }
}

// output IR
std::ostream &operator<<(std::ostream &os, const IrProgram &p) {
  using std::endl;

  // print value according to type
  auto pv = [&os](IndexMapper<Value> &v_index, Value *v) -> std::string {
    std::stringstream ss;
    if (auto x = dyn_cast<ConstValue>(v)) {
      ss << x->imm;
    } else if (auto x = dyn_cast<GlobalRef>(v)) {
      ss << "@" << x->decl->name;
    } else if (auto x = dyn_cast<ParamRef>(v)) {
      ss << "%" << x->decl->name;
    } else {
      ss << "%x" << v_index.get(v);
    }
    return ss.str();
  };

  // builtin functions
  os << "declare i32 @getint()" << endl;
  os << "declare void @putint(i32)" << endl;
  os << "declare void @putch(i32)" << endl;
  for (auto &d : p.glob_decl) {
    os << "@" << d->name << " = global ";
    // type
    print_dims(os, d->dims.data(), d->dims.data() + d->dims.size());
    if (d->has_init) {
      print_flatten_init(os, d->dims.data(), d->dims.data() + d->dims.size(), d->flatten_init.data(),
                         d->flatten_init.data() + d->flatten_init.size());
    } else {
      // default 0 initialized
      os << "zeroinitializer" << endl;
    }
    os << endl;
  }

  for (auto f = p.func.head; f != nullptr; f = f->next) {
    if (f->func->is_int) {
      os << "define i32 @";
    } else {
      os << "define void @";
    }
    os << f->func->name << "(";
    for (auto &p : f->func->params) {
      if (p.dims.size() == 0) {
        // simple case
        os << "i32 ";
      } else {
        // array arg
        // the first dimension becomes pointer
        print_dims(os, p.dims.data() + 1, p.dims.data() + p.dims.size());
        os << "* ";
      }

      os << "%" << p.name;
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
          os << pv(v_index, inst) << " = alloca ";
          print_dims(os, x->sym->dims.data(), x->sym->dims.data() + x->sym->dims.size());
          os << ", align 4 " << endl;
        } else if (auto x = dyn_cast<StoreInst>(inst)) {
          if (x->dims.size() == 0) {
            // simple case
            os << "store i32 " << pv(v_index, x->data.value) << ", i32* " << pv(v_index, x->arr.value) << ", align 4"
               << endl;
          } else {
            // dimension
            // comments
            os << "; " << pv(v_index, x->arr.value);
            for (auto &dim : x->dims) {
              os << "[" << pv(v_index, dim.value) << "]";
            }
            os << " = " << pv(v_index, x->data.value) << endl;

            // temp ptr
            u32 temp = v_index.alloc();
            os << "\t%t" << temp << " = getelementptr inbounds";
            print_dims(os, x->lhs_sym->dims.data(), x->lhs_sym->dims.data() + x->lhs_sym->dims.size());
            os << ", ";
            print_dims(os, x->lhs_sym->dims.data(), x->lhs_sym->dims.data() + x->lhs_sym->dims.size());
            os << "* " << pv(v_index, x->arr.value) << ",";
            // first dimension is always 0
            os << " i32 0";
            for (auto &dim : x->dims) {
              os << ", i32 " << pv(v_index, dim.value);
            }
            os << endl;
            os << "\tstore i32 " << pv(v_index, x->data.value) << ", i32* %t" << temp << ", align 4" << endl;
          }
        } else if (auto x = dyn_cast<LoadInst>(inst)) {
          if (x->dims.size() == x->lhs_sym->dims.size()) {
            // first case: access to item
            if (x->dims.size() == 0) {
              // simple case
              os << pv(v_index, inst) << " = load i32, i32* " << pv(v_index, x->arr.value) << ", align 4" << endl;
            } else {
              // handle dimension
              u32 temp = v_index.alloc();
              os << "%t" << temp << " = getelementptr inbounds ";
              print_dims(os, x->lhs_sym->dims.data(), x->lhs_sym->dims.data() + x->lhs_sym->dims.size());
              os << ", ";
              print_dims(os, x->lhs_sym->dims.data(), x->lhs_sym->dims.data() + x->lhs_sym->dims.size());
              os << "* " << pv(v_index, x->arr.value) << ", ";
              if (x->lhs_sym->dims.size() > 0 && x->lhs_sym->dims[0] == nullptr) {
                // array param
                for (auto &dim : x->dims) {
                  os << "i32 " << pv(v_index, dim.value);
                  if (&dim != &x->dims.back()) {
                    os << ", ";
                  }
                }
              } else {
                // first dimension is always 0
                os << "i32 0";
                for (auto &dim : x->dims) {
                  os << ", i32 " << pv(v_index, dim.value);
                }
              }
              os << endl;
              os << "\t" << pv(v_index, inst) << " = load i32, i32* "
                 << "%t" << temp << ", align 4" << endl;
            }
          } else {
            // second case: item is an array, get array address
            os << pv(v_index, inst) << " = getelementptr inbounds ";
            print_dims(os, x->lhs_sym->dims.data(), x->lhs_sym->dims.data() + x->lhs_sym->dims.size());
            os << ", ";
            print_dims(os, x->lhs_sym->dims.data(), x->lhs_sym->dims.data() + x->lhs_sym->dims.size());
            os << "* " << pv(v_index, x->arr.value) << ", ";

            if (auto p = dyn_cast<AllocaInst>(x->arr.value)) {
              // if it's a local array
              // first dimension is 0
              os << "i32 0, ";
            }

            for (int i = 0; i < x->lhs_sym->dims.size(); i++) {
              if (i < x->dims.size()) {
                // use index from inst
                os << "i32 " << pv(v_index, x->dims[i].value);
              } else {
                // zero otherwise
                os << "i32 0";
              }

              if (i + 1 < x->lhs_sym->dims.size()) {
                os << ", ";
              }
            }
            os << endl;
          }
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
          os << "\tbr i1 %t" << temp << ", label %_" << bb_index.get(x->left) << ", label %_" << bb_index.get(x->right)
             << endl;
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
          for (int i = 0; i < x->args.size(); i++) {
            // type
            if (x->func->params[i].dims.size() == 0) {
              // simple
              os << "i32 ";
            } else {
              // array param
              print_dims(os, x->func->params[i].dims.data(),
                         x->func->params[i].dims.data() + x->func->params[i].dims.size());
              os << "* ";
            }
            // arg
            os << pv(v_index, x->args[i].value);
            if (i + 1 < x->args.size()) {
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

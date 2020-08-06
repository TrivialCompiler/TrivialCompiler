#include "callgraph.hpp"

#include "../../ast.hpp"

void compute_callgraph(IrProgram *p) {
  // collect mapping of Func* => IrFunc *
  std::map<Func *, IrFunc *> func_map;
  for (auto f = p->func.head; f; f = f->next) {
    func_map[f->func] = f;
    // init
    f->callee_func.clear();
    f->caller_func.clear();
    f->impure = false;
  }

  for (auto f = p->func.head; f; f = f->next) {
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        if (auto x = dyn_cast<CallInst>(inst)) {
          auto it = func_map.find(x->func);
          if (it != func_map.end()) {
            // not builtin
            f->callee_func.insert(it->second);
            it->second->caller_func.insert(f);
          } else {
            // builtin functions has side effect
            f->impure = true;
          }
        } else if (auto x = dyn_cast<StoreInst>(inst)) {
          // store to global is a side effect
          if (x->lhs_sym->is_glob) {
            f->impure = true;
          } else if (!x->lhs_sym->dims.empty() && x->lhs_sym->dims[0] == nullptr) {
            // store to param array is a side effect
            f->impure = true;
          }
        }
      }
    }
  }

  // propagate impure from callees to callers
  std::vector<IrFunc *> work_list;
  for (auto f = p->func.head; f; f = f->next) {
    if (f->impure) {
      work_list.push_back(f);
    }
  }

  while (!work_list.empty()) {
    auto *f = work_list.back();
    work_list.pop_back();
    for (auto caller : f->caller_func) {
      if (!caller->impure) {
        caller->impure = true;
        work_list.push_back(caller);
      }
    }
  }

  for (auto f = p->func.head; f; f = f->next) {
    auto func_purity = "function " + std::string(f->func->name) + " is " + (f->impure ? "impure" : "pure");
    dbg(func_purity);
  }
}
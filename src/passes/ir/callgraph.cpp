#include "callgraph.hpp"

void compute_callgraph(IrProgram *p) {
  // collect mapping of Func* => IrFunc *
  std::map<Func *, IrFunc *> func_map;
  for (auto f = p->func.head; f; f = f->next) {
    func_map[f->func] = f;
    // init
    f->called_func.clear();
  }

  for (auto f = p->func.head; f; f = f->next) {
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        if (auto x = dyn_cast<CallInst>(inst)) {
          auto it = func_map.find(x->func);
          if (it != func_map.end()) {
            // not builtin
            f->called_func.insert(it->second);
          }
        }
      }
    }
  }
}
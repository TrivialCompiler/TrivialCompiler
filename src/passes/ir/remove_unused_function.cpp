#include "remove_unused_function.hpp"
#include "../../structure/ast.hpp"

void remove_unused_function(IrProgram *p) {
  for (auto f = p->func.head; f; f = f->next) {
    if (f->caller_func.empty() && f->func->name != "main") {
      auto remove_func = "Function " + std::string(f->func->name) + " not used thus removed from IR";
      dbg(remove_func);
      p->func.remove(f);
    }
  }
}

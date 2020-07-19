#include "pass_manager.hpp"

#include "fill_pred.hpp"
#include "mem2reg.hpp"

typedef void (*FuncPass)(IrFunc *f);

static FuncPass passes[] = {fill_pred, mem2reg};

void run_opt_passes(IrProgram *p) {
  for (auto &pass : passes) {
    for (auto *f = p->func.head; f != nullptr; f = f->next) {
      pass(f);
    }
  }
}
#include "dce.hpp"

void dce(IrFunc *f) {
  while (true) {
    bool changed = false;
    for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
      for (Inst *i = bb->insts.head; i;) {
        Inst *next = i->next;
        if (i->uses.head == nullptr && (isa<BinaryInst>(i) || isa<LoadInst>(i) || isa<AllocaInst>(i) || isa<PhiInst>(i))) {
          bb->insts.remove(i);
          i->deleteValue();
          changed = true;
        }
        i = next;
      }
    }
    if (!changed) break;
  }
}
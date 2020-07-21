#include "fill_pred.hpp"

static void dfs(BasicBlock *bb) {
  if (!bb->vis) {
    bb->vis = true;
    for (BasicBlock *x : bb->succ()) {
      if (x) dfs(x);
    }
  }
}

void fill_pred(IrFunc *f) {
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (BasicBlock **x : bb->succ_ref()) {
      if (x) {
        BasicBlock *x1 = *x;
        while (true) {
          // 空基本块，且以跳转终止，那么直接跳转到它的目标处。可以迭代进行
          // 这里假定不存在死循环
          if (auto y = dyn_cast<JumpInst>(x1->insts.tail); y && x1->insts.head == x1->insts.tail) {
            x1 = *x = y->next;
          } else {
            break;
          }
        }
      }
    }
  }
  f->clear_all_vis();
  dfs(f->bb.head);
  for (BasicBlock *bb = f->bb.head; bb;) {
    BasicBlock *next = bb->next;
    if (!bb->vis) {
      f->bb.remove(bb);
      delete bb;
    } else {
      bb->pred.clear();
    }
    bb = next;
  }
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (BasicBlock *x : bb->succ()) {
      if (x) x->pred.push_back(bb);
    }
  }
}
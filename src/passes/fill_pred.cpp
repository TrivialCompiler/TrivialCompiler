#include "fill_pred.hpp"
#include <unordered_set>

static void dfs(std::unordered_set<BasicBlock *> &vis, BasicBlock *bb) {
  if (vis.insert(bb).second) { // 如果insert成功，证明未曾访问过
    for (BasicBlock *x : bb->succ()) {
      if (x) {
        dfs(vis, x);
      }
    }
  }
}

void fill_pred(IrFunc *f) {
  std::unordered_set<BasicBlock *> vis;
  dfs(vis, f->bb.head);
  for (BasicBlock *bb = f->bb.head; bb;) {
    BasicBlock * next = bb->next;
    if (vis.find(bb) == vis.end()) {
      f->bb.remove(bb);
      delete bb;
    } else {
      bb->pred.clear();
    }
    bb = next;
  }
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (BasicBlock *x : bb->succ()) {
      if (x) {
        x->pred.push_back(bb);
      }
    }
  }
}
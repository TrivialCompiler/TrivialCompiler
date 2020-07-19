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
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    if (vis.find(bb) == vis.end()) {
      f->bb.remove(bb);
      // 这里可以delete它了(不过要注意bb = bb->next，需要先把next读出来再delete)，但是也没必要delete，不差这点内存
    } else {
      bb->pred.clear();
    }
  }
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (BasicBlock *x : bb->succ()) {
      if (x) {
        x->pred.push_back(bb);
      }
    }
  }
}
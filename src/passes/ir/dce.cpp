#include "dce.hpp"
#include "remove_useless_loop.hpp"

static void dfs(std::unordered_set<Inst *> &vis, Inst *i) {
  if (!vis.insert(i).second) return;
  for (auto[it, end] = i->operands(); it < end; ++it) {
    if (auto x = dyn_cast_nullable<Inst>(it->value)) dfs(vis, x);
  }
}

void dce(IrFunc *f) {
  std::unordered_set<Inst *> vis;
  again:
  vis.clear();
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i; i = i->next) {
      if (i->has_side_effect()) dfs(vis, i);
    }
  }
  // 无用的指令间可能相互使用，所以需要先清空operand，否则delete的时候会试图维护已经delete掉的指令的uses
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i; i = i->next) {
      if (vis.find(i) == vis.end()) {
        for (auto[it, end] = i->operands(); it < end; ++it) it->set(nullptr);
      }
    }
  }
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i;) {
      Inst *next = i->next;
      if (vis.find(i) == vis.end()) {
        bb->insts.remove(i);
        i->deleteValue();
      }
      i = next;
    }
  }
  if (remove_useless_loop(f)) goto again;
}
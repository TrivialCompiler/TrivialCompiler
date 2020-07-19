#include "mem2reg.hpp"
#include <algorithm>
#include <cassert>

void mem2reg(IrFunc *f) {
  // 假定dom都是空的
  BasicBlock *entry = f->bb.head;
  entry->dom.insert(entry);
  std::unordered_set<BasicBlock *> all; // 全部基本块，除entry外的dom的初值
  for (BasicBlock *bb = entry; bb; bb = bb->next) { all.insert(bb); }
  for (BasicBlock *bb = entry->next; bb; bb = bb->next) { bb->dom = all; }
  while (true) {
    bool changed = false;
    for (BasicBlock *bb = entry->next; bb; bb = bb->next) {
      for (auto it = bb->dom.begin(); it != bb->dom.end();) {
        BasicBlock *x = *it;
        // 如果bb的任何一个pred的dom不包含x，那么bb的dom也不应该包含x
        if (std::any_of(bb->pred.begin(), bb->pred.end(), [x](BasicBlock *p) { return p->dom.find(x) == p->dom.end(); })) {
          changed = true;
          it = bb->dom.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (!changed) { break; }
  }
  // 计算idom
  entry->idom = nullptr;
  for (BasicBlock *bb = entry->next; bb; bb = bb->next) {
    bb->idom = nullptr;
    for (BasicBlock *d : bb->dom) {
      // strictly dom bb && doesn't strictly dom any x that x strictly dom bb
      if (d != bb && std::all_of(bb->dom.begin(), bb->dom.end(), [d, bb](BasicBlock *x) {
        return x == bb || x == d || x->dom.find(d) == x->dom.end();
      })) {
        bb->idom = d;
        break;
      }
    }
    assert(bb->idom != nullptr);
  }
}
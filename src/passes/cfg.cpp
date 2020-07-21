#include "cfg.hpp"

// 计算dom_level
static void dfs(BasicBlock *bb, u32 dom_level) {
  bb->dom_level = dom_level;
  for (BasicBlock *ch : bb->doms) {
    dfs(ch, dom_level + 1);
  }
}

void compute_dom_info(IrFunc *f) {
  BasicBlock *entry = f->bb.head;
  // 计算dom_by
  entry->dom_by = {entry};
  std::unordered_set<BasicBlock *> all; // 全部基本块，除entry外的dom的初值
  for (BasicBlock *bb = entry; bb; bb = bb->next) {
    all.insert(bb);
    bb->doms.clear(); // 顺便清空doms，与计算dom_by无关
  }
  for (BasicBlock *bb = entry->next; bb; bb = bb->next) { bb->dom_by = all; }
  while (true) {
    bool changed = false;
    for (BasicBlock *bb = entry->next; bb; bb = bb->next) {
      for (auto it = bb->dom_by.begin(); it != bb->dom_by.end();) {
        BasicBlock *x = *it;
        // 如果bb的任何一个pred的dom不包含x，那么bb的dom也不应该包含x
        if (x != bb && std::any_of(bb->pred.begin(), bb->pred.end(), [x](BasicBlock *p) { return p->dom_by.find(x) == p->dom_by.end(); })) {
          changed = true;
          it = bb->dom_by.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (!changed) { break; }
  }
  // 计算idom，顺便填充doms
  entry->idom = nullptr;
  for (BasicBlock *bb = entry->next; bb; bb = bb->next) {
    for (BasicBlock *d : bb->dom_by) {
      // 已知d dom bb，若d != bb，则d strictly dom bb
      // 若还有：d不strictly dom任何strictly dom bb的节点，则d idom bb
      if (d != bb && std::all_of(bb->dom_by.begin(), bb->dom_by.end(), [d, bb](BasicBlock *x) {
        return x == bb || x == d || x->dom_by.find(d) == x->dom_by.end();
      })) {
        bb->idom = d; // 若实现正确，这里恰好会执行一次(即使没有break)
        d->doms.push_back(bb);
        break;
      }
    }
  }
  dfs(entry, 0);
}
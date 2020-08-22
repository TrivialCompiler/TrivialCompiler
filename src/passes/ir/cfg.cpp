#include "cfg.hpp"
#include <cassert>

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

// 在dom tree上后序遍历，识别所有循环
// 在所有递归调用中用的是同一个worklist，这只是为了减少内存申请，它们之间没有任何关系
static void collect_loops(LoopInfo &info, std::vector<BasicBlock *> &worklist, BasicBlock *header) {
  for (BasicBlock *s : header->doms) {
    if (s) collect_loops(info, worklist, s);
  }
  assert(worklist.empty());
  for (BasicBlock *p : header->pred) { // 存在p到header的边
    if (p->dom_by.find(header) != p->dom_by.end()) { // ...且header支配p，这是回边
      worklist.push_back(p);
    }
  }
  if (!worklist.empty()) {
    Loop *l = new Loop(header);
    while (!worklist.empty()) {
      BasicBlock *pred = worklist.back();
      worklist.pop_back();
      if (auto[it, inserted] = info.loop_of_bb.insert({pred, l}); inserted) {
        // 插入成功意味着pred原先不属于任何loop，现在它属于这个loop了
        if (pred != header) {
          worklist.insert(worklist.end(), pred->pred.begin(), pred->pred.end());
        }
      } else {
        // 这是一个已经发现的loop
        Loop *sub = it->second;
        while (Loop *p = sub->parent) sub = p; // 找到已发现的最外层的loop
        if (sub != l) {
          sub->parent = l;
          // 只需考虑sub的header的pred，因为根据循环的性质，循环中其他bb的pred必然都在循环内
          for (BasicBlock *pred : sub->header()->pred) {
            auto it = info.loop_of_bb.find(pred);
            if (it == info.loop_of_bb.end() || it->second != sub) {
              worklist.push_back(pred);
            }
          }
        }
      }
    }
  }
}

// 填充Loop::bbs, sub_loops, LoopInfo::top_level
// todo: llvm是依据bb的后序遍历来填的，这个顺序不会影响任何内容的存在与否，只会影响内容的顺序，那么这个顺序重要吗？
static void populate(LoopInfo &info, BasicBlock *bb) {
  if (bb->vis) return;
  bb->vis = true;
  for (BasicBlock *s : bb->succ()) {
    if (s) populate(info, s);
  }
  auto it = info.loop_of_bb.find(bb);
  Loop *sub = it == info.loop_of_bb.end() ? nullptr : it->second;
  if (sub && sub->header() == bb) {
    (sub->parent ? sub->parent->sub_loops : info.top_level).push_back(sub);
    std::reverse(sub->bbs.begin() + 1, sub->bbs.end());
    std::reverse(sub->sub_loops.begin(), sub->sub_loops.end());
    sub = sub->parent;
  }
  for (; sub; sub = sub->parent)
    sub->bbs.push_back(bb);
}

LoopInfo compute_loop_info(IrFunc *f) {
  compute_dom_info(f);
  LoopInfo info;
  std::vector<BasicBlock *> worklist;
  collect_loops(info, worklist, f->bb.head);
  f->clear_all_vis();
  populate(info, f->bb.head);
  return info;
}

static void dfs(std::vector<BasicBlock *> &po, BasicBlock *bb) {
  if (!bb->vis) {
    bb->vis = true;
    for (BasicBlock *x : bb->succ()) {
      if (x) dfs(po, x);
    }
    po.push_back(bb);
  }
}

std::vector<BasicBlock *> compute_rpo(IrFunc *f) {
  std::vector<BasicBlock *> ret;
  f->clear_all_vis();
  dfs(ret, f->bb.head);
  std::reverse(ret.begin(), ret.end());
  return ret;
}

std::unordered_map<BasicBlock *, std::unordered_set<BasicBlock *>> compute_df(IrFunc *f) {
  std::unordered_map<BasicBlock *, std::unordered_set<BasicBlock *>> df;
  for (BasicBlock *from = f->bb.head; from; from = from->next) {
    for (BasicBlock *to : from->succ()) {
      if (to) { // 枚举所有边(from, to)
        BasicBlock *x = from;
        while (x == to || to->dom_by.find(x) == to->dom_by.end()) { // while x不strictly dom to
          df[x].insert(to);
          x = x->idom;
        }
      }
    }
  }
  return df;
}
#include "mem2reg.hpp"
#include <algorithm>
#include <cassert>
#include <unordered_map>

// 计算支配边界DF，这里用一个map来存每个bb的df，其实是很随意的选择，把它放在BasicBlock里面也不是不行
static std::unordered_map<BasicBlock *, std::unordered_set<BasicBlock *>> compute_df(BasicBlock *entry) {
  // 计算dom
  entry->dom = {entry};
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
      // 已知d dom bb，若d != bb，则d strictly dom bb
      // 若还有：d不strictly dom任何strictly dom bb的节点，则d idom bb
      if (d != bb && std::all_of(bb->dom.begin(), bb->dom.end(), [d, bb](BasicBlock *x) {
        return x == bb || x == d || x->dom.find(d) == x->dom.end();
      })) {
        bb->idom = d;
        break;
      }
    }
    assert(bb->idom != nullptr);
  }
  std::unordered_map<BasicBlock *, std::unordered_set<BasicBlock *>> df;
  for (BasicBlock *from = entry; from; from = from->next) {
    for (BasicBlock *to : from->succ()) {
      if (to) { // 枚举所有边(from, to)
        BasicBlock *x = from;
        while (x == to || to->dom.find(x) == to->dom.end()) { // while x不strictly dom to
          df[x].insert(to);
          x = x->idom;
        }
      }
    }
  }
  return std::move(df);
}

void mem2reg(IrFunc *f) {
  std::unordered_map<AllocaInst *, std::unordered_set<BasicBlock *>> defs;
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i; i = i->next) {
      if (auto a = dyn_cast<AllocaInst>(i)) {
        if (a->sym->dims.empty()) { // 局部int变量
          defs.insert({a, {}});
        }
      }
    }
  }
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i; i = i->next) {
      if (auto x = dyn_cast<StoreInst>(i)) {
        if (auto a = dyn_cast<AllocaInst>(x->arr.value)) { // store的目标是我们考虑的地址
          auto it = defs.find(a);
          if (it != defs.end()) {
            assert(x->dims.empty());
            it->second.insert(bb);
          }
        }
      }
    }
  }
  auto df = compute_df(f->bb.head);
  // mem2reg算法第一步：放置phi节点
  // 这两个变量定义在循环外面，减少申请内存的次数
  std::unordered_set<BasicBlock *> vis;
  std::vector<BasicBlock *> worklist; // 用stack还是queue在这里没有本质区别
  for (auto &[alloca, def_bbs] : defs) {
    vis.clear();
    for (BasicBlock *bb : def_bbs) { worklist.push_back(bb); }
    while (!worklist.empty()) {
      BasicBlock *x = worklist.back();
      worklist.pop_back();
      for (BasicBlock *y : df[x]) {
        if (vis.insert(y).second) {
          new PhiInst(y); // todo: 怎么把它和这个alloca关联上？
          worklist.push_back(y);
        }
      }
    }
  }
}
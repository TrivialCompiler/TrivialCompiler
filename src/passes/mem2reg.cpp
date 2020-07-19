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

struct AllocaInfo {
  std::unordered_set<BasicBlock *> defs; // 用于阶段1，记录所有包含store给这个alloca的基本快
  Value *recent_value = &UndefValue::INSTANCE; // 用于阶段2，记录这个地址当前的值，一开始是未定义
};

void mem2reg(IrFunc *f) {
  std::unordered_map<AllocaInst *, AllocaInfo> allocas;
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i; i = i->next) {
      if (auto a = dyn_cast<AllocaInst>(i)) {
        if (a->sym->dims.empty()) { // 局部int变量
          allocas.insert({a, AllocaInfo()});
        }
      }
    }
  }
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i; i = i->next) {
      if (auto x = dyn_cast<StoreInst>(i)) {
        if (auto a = dyn_cast<AllocaInst>(x->arr.value)) { // store的目标是我们考虑的地址
          auto it = allocas.find(a);
          if (it != allocas.end()) {
            assert(x->dims.empty());
            it->second.defs.insert(bb);
          }
        }
      }
    }
  }
  auto df = compute_df(f->bb.head);
  // mem2reg算法阶段1：放置phi节点
  // 这两个变量定义在循环外面，而且在两步中都用到了，其实没有任何关系，只是为了减少申请内存的次数
  std::unordered_set<BasicBlock *> vis;
  std::vector<BasicBlock *> worklist; // 用stack还是queue在这里没有本质区别
  std::unordered_map<PhiInst *, AllocaInst *> phis; // 记录加入的phi属于的alloca
  for (auto &[alloca, info] : allocas) {
    for (BasicBlock *bb : info.defs) { worklist.push_back(bb); }
    while (!worklist.empty()) {
      BasicBlock *x = worklist.back();
      worklist.pop_back();
      for (BasicBlock *y : df[x]) {
        if (vis.insert(y).second) {
          phis.insert({new PhiInst(y), alloca});
          worklist.push_back(y);
        }
      }
    }
    vis.clear();
  }
  // mem2reg算法阶段2：变量重命名，即删除Load，把对Load结果的引用换成对寄存器的引用，把Store改成寄存器赋值
  worklist.push_back(f->bb.head);
  while (!worklist.empty()) {
    BasicBlock *bb = worklist.back();
    worklist.pop_back();
    if (vis.insert(bb).second) {
      for (Inst *i = bb->insts.head; i; i = i->next) {
        if (auto l = dyn_cast<LoadInst>(i)) {
          if (auto a = dyn_cast<AllocaInst>(l->arr.value)) {
            auto it = allocas.find(a);
            if (it != allocas.end()) {
              l->replaceAllUseWith(it->second.recent_value);
              bb->insts.remove(l);
            }
          }
        } else if (auto s = dyn_cast<StoreInst>(i)) {
          if (auto a = dyn_cast<AllocaInst>(s->arr.value)) {
            auto it = allocas.find(a);
            if (it != allocas.end()) {
              it->second.recent_value = s->data.value;
              bb->insts.remove(s);
              // todo: remove掉的指令会影响到use-def关系，是不是确实需要执行delete呢？
            }
          }
        }
      }
      for (BasicBlock *x : bb->succ()) {
        if (x) {
          worklist.push_back(x);
          for (Inst *i = x->insts.head; i; i = i->next) {
            if (auto p = dyn_cast<PhiInst>(i)) {
              auto it = phis.find(p); // 也许程序中本来就存在phi，所以phis不一定包含了所有的phi
              if (it != phis.end()) {
                u32 idx = std::find(x->pred.begin(), x->pred.end(), bb) - x->pred.begin(); // bb是x的哪个pred?
                p->incoming_values[idx].set(allocas.find(it->second)->second.recent_value);
              }
            } else {
              break; // PhiInst一定是在指令序列的最前面，所以遇到第一个非PhiInst的指令就可以break了
            }
          }
        }
      }
    }
  }
}
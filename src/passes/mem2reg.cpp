#include "mem2reg.hpp"
#include "../ast.hpp"
#include <algorithm>
#include <unordered_map>

// 这里假定dom树已经造好了
void mem2reg(IrFunc *f) {
  std::unordered_map<void *, u32> alloca_ids; // 把alloca映射到整数，后面有好几个vector用这个做下标
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i; i = i->next) {
      if (auto a = dyn_cast<AllocaInst>(i)) {
        if (a->sym->dims.empty()) { // 局部int变量
          alloca_ids.insert({a, (u32) alloca_ids.size()});
        }
      }
    }
  }
  std::vector<std::unordered_set<BasicBlock *>> alloca_defs(alloca_ids.size());
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i; i = i->next) {
      if (auto x = dyn_cast<StoreInst>(i)) {
        if (auto a = dyn_cast<AllocaInst>(x->arr.value)) { // store的目标是我们考虑的地址
          auto it = alloca_ids.find(a);
          if (it != alloca_ids.end()) {
            alloca_defs[it->second].insert(bb);
          }
        }
      }
    }
  }
  // 计算支配边界DF，这里用一个map来存每个bb的df，其实是很随意的选择，把它放在BasicBlock里面也不是不行
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
  // mem2reg算法阶段1：放置phi节点
  // worklist定义在循环外面，只是为了减少申请内存的次数
  std::vector<BasicBlock *> worklist; // 用stack还是queue在这里没有本质区别
  std::unordered_map<PhiInst *, u32> phis; // 记录加入的phi属于的alloca的id
  for (auto &[alloca, id] : alloca_ids) {
    f->clear_all_vis();
    for (BasicBlock *bb : alloca_defs[id]) { worklist.push_back(bb); }
    while (!worklist.empty()) {
      BasicBlock *x = worklist.back();
      worklist.pop_back();
      for (BasicBlock *y : df[x]) {
        if (!y->vis) {
          y->vis = true;
          phis.insert({new PhiInst(y), id});
          worklist.push_back(y);
        }
      }
    }
  }
  // mem2reg算法阶段2：变量重命名，即删除Load，把对Load结果的引用换成对寄存器的引用，把Store改成寄存器赋值
  std::vector<std::pair<BasicBlock *, std::vector<Value *>>> worklist2{{f->bb.head, std::vector<Value *>(alloca_ids.size(), &UndefValue::INSTANCE)}};
  f->clear_all_vis();
  while (!worklist2.empty()) {
    BasicBlock *bb = worklist2.back().first;
    std::vector<Value *> values = std::move(worklist2.back().second);
    worklist2.pop_back();
    if (!bb->vis) {
      bb->vis = true;
      for (Inst *i = bb->insts.head; i;) {
        Inst *next = i->next;
        // 如果一个value在alloca_ids中，它的实际类型必然是AllocaInst，无需再做dyn_cast
        if (auto it = alloca_ids.find(i); it != alloca_ids.end()) {
          bb->insts.remove(i);
          delete static_cast<AllocaInst *>(i); // 但是这里就可以确定它是AllocaInst了
        } else if (auto x = dyn_cast<LoadInst>(i)) {
          // 这里不能，也不用再看x->arr.value是不是AllocaInst了
          // 不能的原因是上面的if分支会delete掉alloca；不用的原因是只要alloca_ids里有，它就一定是AllocaInst
          auto it = alloca_ids.find(x->arr.value);
          if (it != alloca_ids.end()) {
            x->replaceAllUseWith(values[it->second]);
            bb->insts.remove(x);
            x->arr.value = nullptr; // 它用到被delete的AllocaInst，已经不能再访问了
            delete x; // 跟i->deleteValue()作用是一样的(但是跟delete i不一样)，逻辑上节省了一次dispatch的过程
          }
        } else if (auto x = dyn_cast<StoreInst>(i)) {
          auto it = alloca_ids.find(x->arr.value);
          if (it != alloca_ids.end()) {
            values[it->second] = x->data.value;
            bb->insts.remove(x);
            x->arr.value = nullptr;
            delete x;
          }
        } else if (auto x = dyn_cast<PhiInst>(i)) {
          auto it = phis.find(x); // 也许程序中本来就存在phi，所以phis不一定包含了所有的phi
          if (it != phis.end()) {
            values[it->second] = x;
          }
        }
        i = next;
      }
      for (BasicBlock *x : bb->succ()) {
        if (x) {
          worklist2.emplace_back(x, values);
          for (Inst *i = x->insts.head; i; i = i->next) {
            if (auto p = dyn_cast<PhiInst>(i)) {
              auto it = phis.find(p);
              if (it != phis.end()) {
                u32 idx = std::find(x->pred.begin(), x->pred.end(), bb) - x->pred.begin(); // bb是x的哪个pred?
                p->incoming_values[idx].set(values[it->second]);
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
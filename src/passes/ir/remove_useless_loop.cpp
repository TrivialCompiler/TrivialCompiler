#include "remove_useless_loop.hpp"
#include "cfg.hpp"

bool remove_useless_loop(IrFunc *f) {
  std::vector<Loop *> deepest = compute_loop_info(f).deepest_loops();
  bool changed = false;
  for (Loop *l : deepest) {
    // 变量需要提前定义，因为goto不能跳过变量初始化
    BasicBlock *pre_header = nullptr;
    BasicBlock *unique_exit = nullptr;
    std::vector<BasicBlock *> exiting; // 包含于unique_exit的pred
    for (BasicBlock *p : l->bbs[0]->pred) {
      if (std::find(l->bbs.begin(), l->bbs.end(), p) == l->bbs.end()) {
        if (pre_header) goto fail; // 有多于一个从循环外跳转到循环头的bb，失败
        pre_header = p;
      }
    }
    assert(pre_header != nullptr);
    for (BasicBlock *bb : l->bbs) {
      for (BasicBlock *s : bb->succ()) {
        if (s && std::find(l->bbs.begin(), l->bbs.end(), s) == l->bbs.end()) {
          if (unique_exit && unique_exit != s) goto fail; // 有多于一个出口bb，失败
          unique_exit = s;
          exiting.push_back(bb);
        }
      }
    }
    if (!unique_exit) goto fail; // 没有出口，确定是死循环，不考虑
    for (Inst *i = unique_exit->insts.head;; i = i->next) {
      if (auto x = dyn_cast<PhiInst>(i)) {
        Value *fst = x->incoming_values[std::find(unique_exit->pred.begin(), unique_exit->pred.end(), exiting[0]) - unique_exit->pred.begin()].value;
        for (auto it = exiting.begin() + 1; it < exiting.end(); ++it) {
          if (fst != x->incoming_values[std::find(unique_exit->pred.begin(), unique_exit->pred.end(), *it) - unique_exit->pred.begin()].value) {
            goto fail; // 循环出口处phi依赖于从循环中哪个bb退出，失败
          }
        }
      } else break;
    }
    for (BasicBlock *bb : l->bbs) {
      for (Inst *i = bb->insts.head; i; i = i->next) {
        if (isa<ReturnInst>(i) || isa<StoreInst>(i))
          goto fail;
        if (auto x = dyn_cast<CallInst>(i); x && x->func->has_side_effect)
          goto fail;
        // 检查是否被循环外的指令使用
        // LLVM的LoopDeletion Pass不检查这个，而是在上面一个循环中，检查fst是否是循环中定义的
        // 这是因为它保证当前的IR是LCSSA的形式，任何循环中定义的值想要被被外界使用，都需要经过PHI
        // 我们没有这个保证，所以不能这样检查
        for (Use *u = i->uses.head; u; u = u->next) {
          if (std::find(l->bbs.begin(), l->bbs.end(), u->user->bb) == l->bbs.end())
            goto fail;
        }
      }
    }

    dbg("Removing useless loop");
    changed = true;
    {
      bool found = false;
      for (BasicBlock **s : pre_header->succ_ref()) {
        if (s && *s == l->bbs[0]) {
          found = true;
          *s = unique_exit;
          break;
        }
      }
      assert(found);
    }
    unique_exit->pred.push_back(pre_header);
    for (auto it = exiting.begin(); it < exiting.end(); ++it) {
      u32 idx = std::find(unique_exit->pred.begin(), unique_exit->pred.end(), *it) - unique_exit->pred.begin();
      unique_exit->pred.erase(unique_exit->pred.begin() + idx);
      for (Inst *i = unique_exit->insts.head;; i = i->next) {
        if (auto x = dyn_cast<PhiInst>(i)) {
          if (it == exiting.begin()) {
            x->incoming_values.emplace_back(x->incoming_values[idx].value, x);
          }
          x->incoming_values.erase(x->incoming_values.begin() + idx);
        } else break;
      }
    }
    for (BasicBlock *bb : l->bbs) {
      for (Inst *i = bb->insts.head; i; i = i->next) {
        for (auto[it, end] = i->operands(); it < end; ++it) it->set(nullptr);
      }
      f->bb.remove(bb);
      delete bb;
    }
    fail:;
  }
  return changed;
}
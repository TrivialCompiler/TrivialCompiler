#include "gvn_gcm.hpp"
#include "cfg.hpp"
#include <cassert>

// 把i放到new_bb的末尾。这个bb中的位置不重要，因为后续还会再调整它在bb中的位置
static void transfer_inst(Inst *i, BasicBlock *new_bb) {
  i->bb->insts.remove(i);
  i->bb = new_bb;
  new_bb->insts.insertBefore(i, new_bb->insts.tail);
}

// 目前只考虑移动BinaryInst的位置，其他都不允许移动
static void schedule_early(std::unordered_set<Inst *> &vis, BasicBlock *entry, Inst *i) {
  if (vis.insert(i).second) {
    if (auto x = dyn_cast<BinaryInst>(i)) {
      transfer_inst(x, entry);
      for (Use *op : {&x->lhs, &x->rhs}) {
        if (auto op1 = dyn_cast<Inst>(op->value)) {
          schedule_early(vis, entry, op1);
          if (x->bb->dom_level < op1->bb->dom_level) {
            transfer_inst(x, op1->bb);
          }
        }
      }
    }
  }
}

static BasicBlock *find_lca(BasicBlock *a, BasicBlock *b) {
  while (b->dom_level < a->dom_level) a = a->idom;
  while (a->dom_level < b->dom_level) b = b->idom;
  while (a != b) {
    a = a->idom;
    b = b->idom;
  }
  return a;
}

static void schedule_late(std::unordered_set<Inst *> &vis, LoopInfo &info, Inst *i) {
  if (vis.insert(i).second) {
    if (auto x = dyn_cast<BinaryInst>(i)) {
      BasicBlock *lca = nullptr;
      for (Use *u = x->uses.head; u; u = u->next) {
        Inst *u1 = u->user;
        schedule_late(vis, info, u1);
        BasicBlock *use = u1->bb;
        if (auto y = dyn_cast<PhiInst>(u1)) {
          auto it = std::find_if(y->incoming_values.begin(), y->incoming_values.end(), [x](const Use &u) {
            return u.value == x;
          });
          use = (*y->incoming_bbs)[it - y->incoming_values.begin()];
        }
        lca = lca ? find_lca(lca, use) : use;
      }
      // 如果lca为nullptr，意味着uses为空，意味着这条指令可以删掉，但是这个pass不负责处理无用代码
      if (lca) {
        BasicBlock *best = lca;
        u32 best_loop_depth = info.depth_of(best);
        while (lca != x->bb) {
          u32 cur_loop_depth = info.depth_of(lca);
          if (cur_loop_depth < best_loop_depth) {
            best = lca;
            best_loop_depth = cur_loop_depth;
          }
          lca = lca->idom;
        }
        transfer_inst(x, best);
        for (Use *u = x->uses.head; u; u = u->next) {
          // 如果use是PhiInst，一定不用调整位置，因为即使这条指令在bb的最后也能满足这个use
          if (!isa<PhiInst>(u->user) && u->user->bb == best) {
            // 只有当x和u->user在同一个bb，且前者在后者后面时，把它移动到后者的前面一条指令的位置
            // 这里需要判断链表中节点的相对位置，理论上是可以把它卡到非常耗时的，但是应该不会有这么无聊的测例
            for (Inst *i = u->user; i; i = i->next) {
              if (i == x) {
                best->insts.remove(x);
                best->insts.insertBefore(x, u->user);
                break;
              }
            }
          }
        }
      }
    }
  }
}

void gvn_gcm(IrFunc *f) {
  BasicBlock *entry = f->bb.head;
  // 阶段1，gvn
  std::vector<BasicBlock *> rpo = compute_rpo(f);
  std::vector<Value *> vn; // 逻辑上是一个hashmap，以后可以实现Value的hash函数，把它变成真正的hashmap
  for (BasicBlock *bb : rpo) {
    for (Inst *i = bb->insts.head; i;) {
      Inst *next = i->next;
      if (auto x = dyn_cast<BinaryInst>(i)) {
        if (auto l = dyn_cast<ConstValue>(x->lhs.value), r = dyn_cast<ConstValue>(x->rhs.value); l && r) {

        }
      } else if (auto x = dyn_cast<PhiInst>(i)) {

      } else {

      }
      i = next;
    }
  }
  // 阶段2，gcm
  LoopInfo info = compute_loop_info(f);
  std::unordered_set<Inst *> vis;
  for (BasicBlock *bb = entry; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i;) {
      Inst *next = i->next;
      schedule_early(vis, entry, i);
      i = next;
    }
  }
  vis.clear();
  for (BasicBlock *bb = entry; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i;) {
      Inst *next = i->next;
      schedule_late(vis, info, i);
      i = next;
    }
  }
}
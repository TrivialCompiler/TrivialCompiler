#include "gvn_gcm.hpp"
#include "cfg.hpp"
#include "../op.hpp"

static Value *vn_of(std::unordered_map<Value *, Value *> &vn, Value *x);

static Value *find_eq(std::unordered_map<Value *, Value *> &vn, BinaryInst *x) {
  using namespace op;
  Op t1 = (Op) x->tag;
  Value *l1 = vn_of(vn, x->lhs.value), *r1 = vn_of(vn, x->rhs.value);
  for (auto &[k, v] : vn) {
    // 这时的vn已经插入了{x, x}，所以如果遍历的时候遇到x要跳过
    if (auto y = dyn_cast<BinaryInst>(k); y && y != x) {
      Op t2 = (Op) y->tag;
      Value *l2 = vn_of(vn, y->lhs.value), *r2 = vn_of(vn, y->rhs.value);
      bool same = (t1 == t2 &&
                   ((l1 == l2 && r1 == r2) ||
                    (l1 == r2 && r1 == l2 && (t1 == Add || t1 == Mul || t1 == Eq || t1 == Ne || t1 == And || t1 == Or)))) ||
                  (l1 == r2 && r1 == l2 && ((t1 == Lt && t2 == Gt) || (t1 == Gt && t2 == Lt) || (t1 == Le && t2 == Ge) || (t1 == Ge && t2 == Le)));
      if (same) return v;
    }
  }
  return x;
}

static Value *find_eq(std::unordered_map<Value *, Value *> &vn, ConstValue *x) {
  for (auto &[k, v] : vn) {
    if (auto y = dyn_cast<ConstValue>(k); y && y != x && y->imm == x->imm) return v;
  }
  return x;
}

static Value *vn_of(std::unordered_map<Value *, Value *> &vn, Value *x) {
  auto it = vn.insert({x, x});
  if (it.second) {
    // 插入成功了意味着没有指针相等的，但是仍然要找是否存在实际相等的，如果没有的话它的vn就是x
    if (auto y = dyn_cast<BinaryInst>(x)) it.first->second = find_eq(vn, y);
    else if (auto y = dyn_cast<ConstValue>(x)) it.first->second = find_eq(vn, y);
    // 其余情况一定要求指针相等
  }
  return it.first->second;
}

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
          auto it = std::find_if(y->incoming_values.begin(), y->incoming_values.end(), [u](const Use &u2) {
            // 这里必须比较Use的地址，而不能比较u2.value == x
            // 因为一个Phi可以有多个相同的输入，例如 phi [0, bb1] [%x0, bb2] [%x0, bb3]
            // 如果找u2.value == x的，那么两次查找都会返回[%x0, bb2]，导致后面认为只有一个bb用到了%x0
            return &u2 == u;
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
  std::unordered_map<Value *, Value *> vn;
  auto replace = [&vn](Inst *o, Value *n) {
    if (o != n) {
      o->replaceAllUseWith(n);
      o->bb->insts.remove(o);
      vn.erase(o);
      o->deleteValue();
    }
  };
  for (BasicBlock *bb : rpo) {
    for (Inst *i = bb->insts.head; i;) {
      Inst *next = i->next;
      if (auto x = dyn_cast<BinaryInst>(i)) {
        if (auto l = dyn_cast<ConstValue>(x->lhs.value), r = dyn_cast<ConstValue>(x->rhs.value); l && r) {
          replace(x, new ConstValue(op::eval((op::Op) x->tag, l->imm, r->imm)));
        } else {
          // todo: 还可以加入一些别的优化，乘1，乘0之类的
          replace(x, vn_of(vn, x));
        }
      } else if (auto x = dyn_cast<PhiInst>(i)) {
        Value *fst = vn_of(vn, x->incoming_values[0].value);
        bool all_same = true;
        for (u32 i = 1, sz = x->incoming_values.size(); i < sz && all_same; ++i) {
          all_same = fst == vn_of(vn, x->incoming_values[i].value);
        }
        if (all_same) replace(x, fst);
      }
      // 没有必要主动把其他指令加入vn，如果它们被用到的话自然会被加入的
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
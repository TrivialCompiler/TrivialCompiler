#include "gvn_gcm.hpp"

#include "../../structure/ast.hpp"
#include "../../structure/op.hpp"
#include "cfg.hpp"
#include "dce.hpp"
#include "memdep.hpp"

using VN = std::vector<std::pair<Value *, Value *>>;

static Value *vn_of(VN &vn, Value *x);

static Value *find_eq(VN &vn, BinaryInst *x) {
  using namespace op;
  Op t1 = (Op) x->tag;
  Value *l1 = vn_of(vn, x->lhs.value), *r1 = vn_of(vn, x->rhs.value);
  // 不能用迭代器，因为vn_of会往vn中push元素
  for (u32 i = 0; i < vn.size(); ++i) {
    auto[k, v] = vn[i];
    // 这时的vn已经插入了{x, x}，所以如果遍历的时候遇到x要跳过
    if (auto y = dyn_cast<BinaryInst>(k); y && y != x) {
      Op t2 = (Op) y->tag;
      Value *l2 = vn_of(vn, y->lhs.value), *r2 = vn_of(vn, y->rhs.value);
      bool same = (t1 == t2 &&
                   ((l1 == l2 && r1 == r2) ||
                    (l1 == r2 && r1 == l2 && (t1 == Add || t1 == Mul || t1 == Eq || t1 == Ne || t1 == And || t1 == Or)))) ||
                  (l1 == r2 && r1 == l2 && isrev(t1, t2));
      if (same) return v;
    }
  }
  return x;
}

// GetElementPtrInst和LoadInst的find_eq中，对x->arr.value的递归搜索最终会终止于AllocaInst, ParamRef, GlobalRef，它们直接用指针比较
static Value *find_eq(VN &vn, GetElementPtrInst *x) {
  for (u32 i = 0; i < vn.size(); ++i) {
    auto[k, v] = vn[i];
    if (auto y = dyn_cast<GetElementPtrInst>(k); y && y != x) {
      bool same = vn_of(vn, x->arr.value) == vn_of(vn, y->arr.value) &&
                  vn_of(vn, x->index.value) == vn_of(vn, y->index.value);
      if (same) return v;
    }
  }
  return x;
}

static Value *find_eq(VN &vn, LoadInst *x) {
  for (u32 i = 0; i < vn.size(); ++i) {
    auto[k, v] = vn[i];
    if (auto y = dyn_cast<LoadInst>(k); y && y != x) {
      bool same = vn_of(vn, x->arr.value) == vn_of(vn, y->arr.value) &&
                  vn_of(vn, x->index.value) == vn_of(vn, y->index.value) &&
                  x->mem_token.value == y->mem_token.value;
      if (same) return v;
    } else if (auto y = dyn_cast<StoreInst>(k)) {
      // a[0] = 1; b = a[0]可以变成b = 1
      bool same = vn_of(vn, x->arr.value) == vn_of(vn, y->arr.value) &&
                  vn_of(vn, x->index.value) == vn_of(vn, y->index.value) &&
                  x->mem_token.value == y; // 这意味着这个store dominates了这个load
      if (same) return y->data.value;
    }
  }
  return x;
}

static Value *find_eq(VN &vn, CallInst *x) {
  if (!x->func->pure()) return x;
  for (u32 i = 0; i < vn.size(); ++i) {
    auto[k, v] = vn[i];
    if (auto y = dyn_cast<CallInst>(k); y && y->func == x->func) {
      assert(x->args.size() == y->args.size());
      for (u32 j = 0; j < x->args.size(); ++j) {
        if (vn_of(vn, x->args[j].value) != vn_of(vn, y->args[j].value)) goto fail;
      }
      return v;
      fail:;
    }
  }
  return x;
}

static Value *vn_of(VN &vn, Value *x) {
  auto it = std::find_if(vn.begin(), vn.end(), [x](std::pair<Value *, Value *> kv) { return kv.first == x; });
  if (it != vn.end()) return it->second;
  // 此时没有指针相等的，但是仍然要找是否存在实际相等的，如果没有的话它的vn就是x
  u32 idx = vn.size();
  vn.emplace_back(x, x);
  if (auto y = dyn_cast<BinaryInst>(x)) vn[idx].second = find_eq(vn, y);
  else if (auto y = dyn_cast<GetElementPtrInst>(x)) vn[idx].second = find_eq(vn, y);
  else if (auto y = dyn_cast<LoadInst>(x)) vn[idx].second = find_eq(vn, y);
  else if (auto y = dyn_cast<CallInst>(x)) vn[idx].second = find_eq(vn, y);
  // 其余情况一定要求指针相等
  return vn[idx].second;
}

// 把形如b = a + 1; c = b + 1的c转化成a + 2
// 乘法：b = a * C1, c = b * C2 => a * (C1 * C2)
// 加减总共9种情况，b和c都可以是Add, Sub, Rsb，首先把Sub都变成Add负值，剩下四种情况
// a + C1; b + C2 => a + (C1 + C2)
// a + C1; rsb b C2 => rsb a (C2 - C1)
// rsb a C1; b + C2 => rsb a (C1 + C2)
// rsb a C1; rsb b C2 => a + (C2 - C1)
// 故两个操作符相同时结果为Add，否则为Rsb; c为Add是C2前是正号，否则是负号
static void try_fold_lhs(BinaryInst *x) {
  if (auto r = dyn_cast<ConstValue>(x->rhs.value)) {
    if (x->tag == Value::Tag::Sub) x->rhs.set(r = ConstValue::get(-r->imm)), x->tag = Value::Tag::Add;
    if (auto l = dyn_cast<BinaryInst>(x->lhs.value)) {
      if (auto lr = dyn_cast<ConstValue>(l->rhs.value)) {
        if (l->tag == Value::Tag::Sub) l->rhs.set(lr = ConstValue::get(-lr->imm)), l->tag = Value::Tag::Add;
        if ((x->tag == Value::Tag::Add || x->tag == Value::Tag::Rsb) && (l->tag == Value::Tag::Add || l->tag == Value::Tag::Rsb)) {
          x->lhs.set(l->lhs.value);
          x->rhs.set(ConstValue::get(r->imm + (x->tag == Value::Tag::Add ? lr->imm : -lr->imm)));
          x->tag = (x->tag == Value::Tag::Add) == (l->tag == Value::Tag::Add) ? Value::Tag::Add : Value::Tag::Rsb;
        } else if (x->tag == Value::Tag::Mul && r->tag == Value::Tag::Mul) {
          x->lhs.set(l->lhs.value);
          x->rhs.set(ConstValue::get(lr->imm * r->imm));
        }
      }
    }
  }
}

// 把i放到new_bb的末尾。这个bb中的位置不重要，因为后续还会再调整它在bb中的位置
static void transfer_inst(Inst *i, BasicBlock *new_bb) {
  i->bb->insts.remove(i);
  i->bb = new_bb;
  new_bb->insts.insertBefore(i, new_bb->insts.tail);
}

static bool is_pure_call(Inst *i) {
  if (auto x = dyn_cast<CallInst>(i); x && x->func->pure()) return true;
  return false;
}

// 目前只考虑移动BinaryInst的位置，其他都不允许移动
static void schedule_early(std::unordered_set<Inst *> &vis, BasicBlock *entry, Inst *i) {
  auto schedule_op = [&vis, entry](Inst *x, Value *op) {
    if (auto op1 = dyn_cast<Inst>(op)) {
      schedule_early(vis, entry, op1);
      if (x->bb->dom_level < op1->bb->dom_level) {
        transfer_inst(x, op1->bb);
      }
    }
  };
  if (vis.insert(i).second) {
    if (auto x = dyn_cast<BinaryInst>(i)) {
      transfer_inst(x, entry);
      for (Use *op : {&x->lhs, &x->rhs}) {
        schedule_op(x, op->value);
      }
    } else if (auto x = dyn_cast<GetElementPtrInst>(i)) {
      transfer_inst(x, entry);
      schedule_op(x, x->arr.value);
      schedule_op(x, x->index.value);
    } else if (auto x = dyn_cast<LoadInst>(i)) {
      transfer_inst(x, entry);
      schedule_op(x, x->arr.value);
      schedule_op(x, x->index.value);
      schedule_op(x, x->mem_token.value);
    } else if (auto x = dyn_cast<CallInst>(i); x && x->func->pure()) {
      transfer_inst(x, entry);
      for (Use &arg : x->args) {
        schedule_op(x, arg.value);
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
    if (isa<BinaryInst>(i) || isa<GetElementPtrInst>(i) || isa<LoadInst>(i) || is_pure_call(i)) {
      BasicBlock *lca = nullptr;
      for (Use *u = i->uses.head; u; u = u->next) {
        Inst *u1 = u->user;
        schedule_late(vis, info, u1);
        BasicBlock *use = u1->bb;
        // MemPhiInst的注释中解释了，这里把MemPhiInst当成PhiInst用
        if (isa<PhiInst>(u1) || isa<MemPhiInst>(u1)) {
          auto y = static_cast<PhiInst *>(u1);
          auto it = std::find_if(y->incoming_values.begin(), y->incoming_values.end(), [u](const Use &u2) {
            // 这里必须比较Use的地址，而不能比较u2.value == i
            // 因为一个Phi可以有多个相同的输入，例如 phi [0, bb1] [%x0, bb2] [%x0, bb3]
            // 如果找u2.value == i的，那么两次查找都会返回[%x0, bb2]，导致后面认为只有一个bb用到了%x0
            return &u2 == u;
          });
          use = y->incoming_bbs()[it - y->incoming_values.begin()];
        }
        lca = lca ? find_lca(lca, use) : use;
      }
      // lca为空即没有人使用它，这并不是什么有意义的情形，而且也没法知道这条指令该放在哪里了
      // 因此在gvn_gcm pass前需要先运行一遍dce pass，保证没有这种情形
      assert(lca != nullptr);
      BasicBlock *best = lca;
      u32 best_loop_depth = info.depth_of(best);
      // 论文里是while (lca != i->bb)，但是我觉得放在x->bb也是可以的，所以改成了考虑完lca后再判断是否等于x->bb
      while (true) {
        u32 cur_loop_depth = info.depth_of(lca);
        if (cur_loop_depth < best_loop_depth) {
          best = lca;
          best_loop_depth = cur_loop_depth;
        }
        if (lca == i->bb) break;
        lca = lca->idom;
      }
      transfer_inst(i, best);
      for (Inst *j = best->insts.head; j; j = j->next) {
        if (!isa<PhiInst>(j)) {
          for (Use *u = i->uses.head; u; u = u->next) {
            if (u->user == j) {
              best->insts.remove(i);
              best->insts.insertBefore(i, j);
              goto out;
            }
          }
        }
      }
      out:;
    }
  }
}

// 现在的依赖关系有点复杂，gvn_gcm的第一阶段需要memdep，第二阶段需要memdep和dce, 而dce会清除memdep的结果
// 不是很方便在pass manager里表示这种关系，所以只能在第一阶段前手动调用memdep，第二阶段前手动依次调用dce和memdep
void gvn_gcm(IrFunc *f) {
  BasicBlock *entry = f->bb.head;
  // 阶段1，gvn
  compute_memdep(f);
  std::vector<BasicBlock *> rpo = compute_rpo(f);
  VN vn;
  auto replace = [&vn](Inst *o, Value *n) {
    if (o != n) {
      o->replaceAllUseWith(n);
      o->bb->insts.remove(o);
      auto it = std::find_if(vn.begin(), vn.end(), [o](std::pair<Value *, Value *> kv) { return kv.first == o; });
      if (it != vn.end()) {
        std::swap(*it, vn.back());
        vn.pop_back();
      }
      o->deleteValue();
    }
  };
  for (BasicBlock *bb : rpo) {
    for (Inst *i = bb->insts.head; i;) {
      Inst *next = i->next;
      if (auto x = dyn_cast<BinaryInst>(i)) {
        if (isa<ConstValue>(x->lhs.value) && x->swapOperand()) {
          dbg("IMM operand moved from lhs to rhs");
        }
        auto l = dyn_cast<ConstValue>(x->lhs.value), r = dyn_cast<ConstValue>(x->rhs.value);
        // for most instructions reach here, rhs is IMM
        if (l && r) {
          // both constant, evaluate and eliminate
          replace(x, ConstValue::get(op::eval((op::Op) x->tag, l->imm, r->imm)));
        } else {
          try_fold_lhs(x);
          if (auto value = x->optimizedValue()) {
            // can be (arithmetically) replaced with one single value (constant or one side of operands)
            replace(x, value);
          } else {
            replace(x, vn_of(vn, x));
          }
        }
      } else if (auto x = dyn_cast<PhiInst>(i)) {
        Value *fst = vn_of(vn, x->incoming_values[0].value);
        bool all_same = true;
        for (u32 i = 1, sz = x->incoming_values.size(); i < sz && all_same; ++i) {
          all_same = fst == vn_of(vn, x->incoming_values[i].value);
        }
        if (all_same) replace(x, fst);
      } else if (auto x = dyn_cast<LoadInst>(i)) {
        bool replaced = false;
        if (x->lhs_sym->is_glob && x->lhs_sym->is_const) {
          replaced = true;
          u32 offset = 0;
          AccessInst *a = x;
          for (u32 dim_pos = x->lhs_sym->dims.size(); dim_pos >= 1; --dim_pos) {
            if (auto idx = dyn_cast<ConstValue>(a->index.value)) {
              assert(a != nullptr);
              offset += idx->imm * (dim_pos == x->lhs_sym->dims.size() ? 1 : x->lhs_sym->dims[dim_pos]->result);
              a = dyn_cast<GetElementPtrInst>(a->arr.value);
            } else {
              replaced = false;
              break;
            }
          }
          if (replaced) replace(x, ConstValue::get(x->lhs_sym->flatten_init[offset]->result));
        }
        if (!replaced) replace(i, vn_of(vn, i));
      } else if (isa<GetElementPtrInst>(i) || is_pure_call(i)) {
        replace(i, vn_of(vn, i));
      } else if (isa<StoreInst>(i)) {
        // 这里没有必要做替换，把StoreInst放进vn的目的是让LoadInst可以用store的右手项
        // vn中一定不含这个i，因为没有人用到StoreInst(唯一用到StoreInst的地方是mem_token，但是没有加入vn)
        vn.emplace_back(i, i);
      }
      // 没有必要主动把其他指令加入vn，如果它们被用到的话自然会被加入的
      i = next;
    }
  }
  clear_memdep(f), dce(f), compute_memdep(f);
  // 阶段2，gcm
  LoopInfo info = compute_loop_info(f);
  std::vector<Inst *> insts;
  for (BasicBlock *bb = entry; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i; i = i->next) insts.push_back(i);
  }
  std::unordered_set<Inst *> vis;
  for (Inst *i : insts) schedule_early(vis, entry, i);
  vis.clear();
  for (Inst *i : insts) schedule_late(vis, info, i);
  // fixme: 其实本质上这个算法是没办法决定一个bb内的任何顺序的，需要别的调度策略，这里简单做一个，激活cmp + branch的优化
  for (BasicBlock *bb = entry; bb; bb = bb->next) {
    if (auto x = dyn_cast<BranchInst>(bb->insts.tail)) {
      if (x->cond.value->tag >= Value::Tag::Lt && x->cond.value->tag <= Value::Tag::Ne) {
        auto c = static_cast<Inst *>(x->cond.value);
        if (c->bb == bb && c->uses.head == c->uses.tail) { // 要求只被这个branch使用
          bb->insts.remove(c);
          bb->insts.insertBefore(c, x);
        }
      }
    }
  }
  clear_memdep(f);
}

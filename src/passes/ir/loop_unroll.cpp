#include "loop_unroll.hpp"

#include <cassert>

#include "../../structure/op.hpp"
#include "cfg.hpp"

static void get_deepest(std::vector<Loop *> &deepest, Loop *l) {
  if (l->sub_loops.empty()) deepest.push_back(l);
  else for (Loop *x : l->sub_loops) get_deepest(deepest, x);
}

static void clone_inst(Inst *x, BasicBlock *bb, std::unordered_map<Value *, Value *> &map) {
  auto get = [&map](const Use &u) {
    Value *v = u.value;
    auto it = map.find(v);
    return it != map.end() ? it->second : v;
  };
  Inst *res;
  if (auto y = dyn_cast<BinaryInst>(x)) {
    res = new BinaryInst(y->tag, get(y->lhs), get(y->rhs), bb);
  } else if (auto y = dyn_cast<GetElementPtrInst>(x)) {
    res = new GetElementPtrInst(y->lhs_sym, get(y->arr), get(y->index), y->multiplier, bb);
  } else if (auto y = dyn_cast<LoadInst>(x)) {
    res = new LoadInst(y->lhs_sym, get(y->arr), get(y->index), bb); // 不维护内存依赖
  } else if (auto y = dyn_cast<StoreInst>(x)) {
    res = new StoreInst(y->lhs_sym, get(y->arr), get(y->data), get(y->index), bb);
  } else {
    // 不可能是Branch, Jump, resurn, CallInst, Alloca, Phi, MemOp, MemPhi
    UNREACHABLE();
  }
  map.insert_or_assign(x, res);
}

// 这个pass不能处理memdep信息，需要保证调用它时没有memdep信息
void loop_unroll(IrFunc *f) {
  LoopInfo info = compute_loop_info(f);
  std::vector<Loop *> deepest;
  for (Loop *l : info.top_level) get_deepest(deepest, l);
  for (Loop *l : deepest) {
    // 只考虑这样的循环，前端生成这样的while循环如果body内没有跳转，会被bbopt优化成这样
    // bb_cond:
    //   ...
    //   if (i(0) < n) br bb_body else br bb_end
    //   或者 if (const 非0) br bb_body else br bb_end
    // bb_body: ; preds = [bb_cond, bb_body]
    //   i = phi [i(0), bb_cond] [i(x), bb_body]
    //   ...
    //   i(1) = i + C1
    //   ...
    //   i(x) = i(x-1) + Cx
    //   ...
    //   if (i(x) < n) br bb_body else br bb_end // 注意这里是i(x)，不是i
    // bb_end:; preds = [bb_cond, bb_body]
    //   ...
    // 其中i由循环中的一个phi定值，且来源仅有两个：初值i0和i+常数，n在循环外定值
    // 大小关系可以是除了==, !=外的四个
    if (l->bbs.size() != 1) continue;
    BasicBlock *bb_body = l->bbs[0];
    if (bb_body->pred.size() != 2) continue;
    u32 idx_in_body = bb_body == bb_body->pred[1];
    BasicBlock *bb_cond = bb_body->pred[!idx_in_body];
    auto br0 = dyn_cast<BranchInst>(bb_cond->insts.tail);
    if (!br0 || br0->left != bb_body) continue;
    BinaryInst *cond0; // cond0可能是nullptr，此时bb_cond中以if (const 非0) br bb_body else br bb_end结尾
    {
      Value *v = br0->cond.value;
      if ((v->tag < Value::Tag::Lt || v->tag > Value::Tag::Gt) && (v->tag != Value::Tag::Const || static_cast<ConstValue *>(v)->imm == 0)) continue;
      cond0 = dyn_cast<BinaryInst>(v);
    }
    // cond0的lhs和rhs不可能是循环中定义的，不然就不dominate它的use了
    auto br = dyn_cast<BranchInst>(bb_body->insts.tail);
    if (!br || br->left != bb_body || br->cond.value->tag < Value::Tag::Lt || br->cond.value->tag > Value::Tag::Gt) continue;
    auto cond = static_cast<BinaryInst *>(br->cond.value);
    BasicBlock *bb_end = br0->right;
    if (bb_end->pred.size() != 2 || br->right != bb_end) continue;
    u32 idx_in_end = bb_body == bb_end->pred[1];

    int step = 0;
    int cond0_i0 = -1, cond_ix = -1; // 结果为0/1时继续处理，表示i0/ix在cond0/cond的lhs还是rhs
    Value *i0 = nullptr;
    PhiInst *phi_ix = nullptr;
    for (int pos = 0; cond0_i0 == -1 && pos < 2; ++pos) { // 考虑i < n和n > i两种写法
      step = 0, cond_ix = pos;
      Value *ix = (&cond->lhs)[cond_ix].value, *n = (&cond->lhs)[!cond_ix].value, *i = ix;
      while (true) {
        if (auto x = dyn_cast<BinaryInst>(i)) {
          // 这里只考虑Add，因为gvn_gcm pass会把减常数变成加相反数
          if (auto r = dyn_cast<ConstValue>(x->rhs.value); r && x->tag == Value::Tag::Add) {
            step += r->imm;
            i = x->lhs.value;
          } else break;
        } else if (auto x = dyn_cast<PhiInst>(i)) {
          if (x->bb != bb_body) break;
          assert(x->incoming_values.size() == 2);
          i0 = x->incoming_values[!idx_in_body].value;
          phi_ix = x;
          if (x->incoming_values[idx_in_body].value != ix) break;
          if (cond0) {
            // 两个cond的n必须是同一个，这可以保证bb_body中的cond的n不是循环中定义的
            if (cond->tag == cond0->tag && i0 == cond0->lhs.value && n == cond0->rhs.value) {
              cond0_i0 = 0;
              break;
            } else if (op::isrev((op::Op) cond->tag, (op::Op) cond0->tag) && i0 == cond0->rhs.value && n == cond0->lhs.value) {
              cond0_i0 = 1;
              break;
            } else break;
          } else {
            // 如果cond0是ConstValue，则还是需要检查n的定义位置
            auto n1 = dyn_cast<Inst>(n);
            if (!n1 || n1->bb != bb_body) {
              cond0_i0 = 0; // 这个值没什么用了，只是和-1区分一下
              break;
            } else break;
          }
        } else break;
      }
    }
    if (cond0_i0 == -1 || step == 0) continue;

    bool inst_ok = true;
    int inst_cnt = 0;
    for (Inst *i = bb_body->insts.head; inst_ok && i; i = i->next) {
      if (isa<PhiInst>(i) || isa<BranchInst>(i)) continue;
        // 包含call的循环没有什么展开的必要
        // 目前不考虑有局部数组的情形，memdep应该不能处理多个局部数组对应同一个Decl
      else if (isa<CallInst>(i) || isa<AllocaInst>(i) || ++inst_cnt >= 16) inst_ok = false;
    }
    if (!inst_ok) continue;

    dbg("Performing loop unroll");
    // 验证结束，循环展开，目前为了实现简单仅展开2次，如果实现正确的话运行n次就可以展开2^n次
    std::unordered_map<Value *, Value *> map;
    auto get = [&map](Value *v) {
      auto it = map.find(v);
      return it != map.end() ? it->second : v;
    };
    Inst *first_non_phi = nullptr;
    for (Inst *i = bb_body->insts.head;; i = i->next) {
      if (auto x = dyn_cast<PhiInst>(i)) {
        map.insert({x, x->incoming_values[idx_in_body].value});
      } else {
        first_non_phi = i;
        break;
      }
    }

    Value *old_n = (&cond->lhs)[!cond_ix].value;
    bb_body->insts.remove(br); // 后面需要往bb_body的最后insert，所以先把跳转指令去掉，等下再加回来
    Inst *orig_last = bb_body->insts.tail; // 不可能为null，因为bb_body中至少存在一条计算i1的指令
    assert(orig_last != nullptr);

    // 先做一个特判，如果可能的话完全展开这个循环
    if (auto i0c = dyn_cast<ConstValue>(i0), old_nc = dyn_cast<ConstValue>(old_n); i0c && old_nc) {
      i32 beg = i0c->imm, end = old_nc->imm, times = (end - beg) / step; // 这不是精确的次数，可能会差一次
      if (times <= 0 || times > 32) goto normal_unroll;
      beg += step;
      while (op::eval((op::Op) cond->tag, cond_ix == 0 ? beg : end, cond_ix == 0 ? end : beg)) {
        for (Inst *i = first_non_phi;; i = i->next) {
          clone_inst(i, bb_body, map);
          if (i == orig_last) break;
        }
        for (Inst *i = bb_body->insts.head;; i = i->next) {
          if (auto x = dyn_cast<PhiInst>(i)) {
            map.find(x)->second = get(x->incoming_values[idx_in_body].value);
          } else break;
        }
        beg += step;
      }
      delete br;
      bb_body->pred.erase(bb_body->pred.begin() + idx_in_body);
      new JumpInst(bb_end, bb_body);
      for (Inst *i = bb_end->insts.head;; i = i->next) {
        if (auto x = dyn_cast<PhiInst>(i)) {
          x->incoming_values[idx_in_end].set(get(x->incoming_values[idx_in_end].value));
        } else break;
      }
      for (Inst *i = bb_body->insts.head;;) {
        if (auto x = dyn_cast<PhiInst>(i)) {
          Inst *next = x->next;
          x->replaceAllUseWith(x->incoming_values[!idx_in_body].value);
          bb_body->insts.remove(x);
          delete x;
          i = next;
        } else break;
      }
      continue;
    }
    // 特判失败了，还是展开2次
    normal_unroll:
    Value *new_n = new BinaryInst(Value::Tag::Add, old_n, ConstValue::get(-step), cond0 ? static_cast<Inst *>(cond0) : static_cast<Inst *>(br0));
    if (cond0) {
      br0->cond.set(new BinaryInst(cond0->tag, cond0_i0 == 0 ? cond0->lhs.value : new_n, cond0_i0 == 0 ? new_n : cond0->rhs.value, cond0));
    }

    for (Inst *i = first_non_phi;; i = i->next) {
      clone_inst(i, bb_body, map);
      if (i == orig_last) break;
    }
    bb_body->insts.insertAtEnd(br);

    Value *new_ix = get((&cond->lhs)[cond_ix].value);
    Value *new_cond = new BinaryInst(cond->tag, cond_ix == 0 ? new_ix : new_n, cond_ix == 0 ? new_n : new_ix, br);
    br->cond.set(new_cond);

    auto bb_if = new BasicBlock;
    auto bb_last = new BasicBlock;
    f->bb.insertAfter(bb_if, bb_body);
    f->bb.insertAfter(bb_last, bb_if);
    br0->right = bb_if, bb_if->pred.push_back(bb_cond);
    br->right = bb_if, bb_if->pred.push_back(bb_body);
    bb_last->pred.push_back(bb_if);
    {
      // PhiInst的构造函数要求insts非空，所以先插入最后的指令
      auto if_cond = new BinaryInst(cond->tag, nullptr, nullptr, bb_if);
      new BranchInst(if_cond, bb_last, bb_end, bb_if);
      // 这一步是构造bb_if中的phi，它来自bb_body和bb_end的phi
      // (bb_body和bb_end的phi并不一定完全一样，有可能一个值只在循环内用到，也有可能一个循环内定义的值循环中却没有用到)
      // 循环1构造来自bb_body的phi，顺便将来自bb_body的phi的来自bb_body的值修改为新值
      // 循环2修改map，将来自bb_body的phi映射到刚刚插入的phi
      // 循环3构造来自bb_end的phi
      // 循环1和2不能合并，否则违背了phi的parallel的特性，当bb_body中的一个phi作为另一个phi的操作数时，就可能出错
      for (Inst *i = bb_body->insts.head;; i = i->next) {
        if (auto x = dyn_cast<PhiInst>(i)) {
          Value *from_cond = x->incoming_values[!idx_in_body].value, *from_body = get(x->incoming_values[idx_in_body].value);
          x->incoming_values[idx_in_body].set(from_body);
          auto p = new PhiInst(if_cond);
          p->incoming_values[0].set(from_cond);
          p->incoming_values[1].set(from_body);
        } else break;
      }
      for (Inst *i = bb_body->insts.head, *i1 = bb_if->insts.head;; i = i->next, i1 = i1->next) {
        if (isa<PhiInst>(i)) map.find(i)->second = i1; else break;
      }
      for (Inst *i = bb_end->insts.head;; i = i->next) {
        if (auto x = dyn_cast<PhiInst>(i)) {
          Value *from_cond = x->incoming_values[!idx_in_end].value, *from_body = get(x->incoming_values[idx_in_end].value);
          bool found = false;
          for (Inst *j = bb_if->insts.head; !found; j = j->next) {
            if (auto y = dyn_cast<PhiInst>(j)) {
              if (y->incoming_values[0].value == from_cond && y->incoming_values[1].value == from_body) {
                x->incoming_values[!idx_in_end].set(y);
                found = true;
              }
            } else break;
          }
          if (!found) {
            auto p = new PhiInst(if_cond);
            p->incoming_values[0].set(from_cond);
            p->incoming_values[1].set(from_body);
            x->incoming_values[!idx_in_end].set(p);
          }
        } else break;
      }
      (&if_cond->lhs)[cond_ix].set(get(phi_ix));
      (&if_cond->lhs)[!cond_ix].set(old_n);
    }
    bb_end->pred[!idx_in_end] = bb_if;
    bb_end->pred[idx_in_end] = bb_last;
    for (Inst *i = first_non_phi;; i = i->next) {
      clone_inst(i, bb_last, map);
      if (i == orig_last) break;
    }
    new JumpInst(bb_end, bb_last);
    for (Inst *i = bb_end->insts.head; i; i = i->next) {
      if (auto x = dyn_cast<PhiInst>(i)) {
        x->incoming_values[idx_in_end].set(get(x->incoming_values[idx_in_end].value));
      } else break;
    }
  }
}
#include "loop_unroll.hpp"
#include "cfg.hpp"
#include "../../op.hpp"
#include <cassert>

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
    // 这个pass不维护内存依赖，如果需要的话就重新运行一次memdep pass
    res = new LoadInst(y->lhs_sym, get(y->arr), get(y->index), bb);
  } else if (auto y = dyn_cast<StoreInst>(x)) {
    res = new StoreInst(y->lhs_sym, get(y->arr), get(y->data), get(y->index), bb);
  } else if (auto y = dyn_cast<MemOpInst>(x)) {
    return; // 同LoadInst之理
  } else {
    // 不可能是Branch, Jump, resurn, CallInst, Alloca, Phi, MemPhi
    UNREACHABLE();
  }
  map.insert_or_assign(x, res);
}

// gvn_gcm pass已经把其他形式的equiv的Value用同一个指针表示了，这里只用考虑ConstValue
static bool equiv(Value *l, Value *r) {
  if (auto l1 = dyn_cast<ConstValue>(l), r1 = dyn_cast<ConstValue>(r); l1 && r1) return l1->imm == r1->imm;
  else return l == r;
}

void loop_unroll(IrFunc *f) {
  LoopInfo info = compute_loop_info(f);
  std::vector<Loop *> deepest;
  for (Loop *l : info.top_level) get_deepest(deepest, l);
  for (Loop *l : deepest) {
    // 只考虑这样的循环，前端也只会生成这样的while循环
    // bb_cond:
    //   ...
    //   if (i(0) < n) br bb_header else br bb_end
    // bb_header: ; preds [bb_cond, bb_body]
    //   i = phi [i(0), bb_cond] [i(x), bb_body]
    //   ...
    //   jmp bb_body
    // bb_body: ; preds = [bb_header]
    //   i(1) = i + C1
    //   ...
    //   i(x) = i(x-1) + Cx
    //   ...
    //   if (i(x) < n) br bb_header else br bb_end // 注意这里是i(x)，不是i
    // bb_end:; preds = [bb_cond, bb_body]
    //   ...
    // 其中i由循环中的一个phi定值，且来源仅有两个：初值i0和i+常数，n在循环外定值
    // 根据mem2reg算法的性质，这个循环中不可能在bb_back里放置phi，所以只考虑i由bb_header中的phi定值
    // 大小关系可以是除了==, !=外的四个
    if (l->bbs.size() != 2) continue;
    BasicBlock *bb_header = l->bbs[0];
    BasicBlock *bb_body = l->bbs[1];
    if (bb_header->pred.size() != 2) continue;
    u32 idx_in_header = bb_body == bb_header->pred[1];
    BasicBlock *bb_cond = bb_header->pred[!idx_in_header];
    auto br0 = dyn_cast<BranchInst>(bb_cond->insts.tail);
    if (!br0 || br0->left != bb_header || br0->cond.value->tag < Value::Tag::Lt || br0->cond.value->tag > Value::Tag::Gt) continue;
    auto cond0 = static_cast<BinaryInst *>(br0->cond.value);
    // cond0的lhs和rhs不可能是循环中定义的，不然就不dominate它的use了
    auto jump = dyn_cast<JumpInst>(bb_header->insts.tail);
    if (!jump || jump->next != bb_body) continue;
    auto br = dyn_cast<BranchInst>(bb_body->insts.tail);
    if (!br || br->left != bb_header || br->cond.value->tag < Value::Tag::Lt || br->cond.value->tag > Value::Tag::Gt) continue;
    auto cond = static_cast<BinaryInst *>(br->cond.value);
    BasicBlock *bb_end = br0->right;
    if (bb_end->pred.size() != 2 || br->right != bb_end) continue;

    int step = 0;
    Use *cond0_i0 = nullptr, *cond0_n = nullptr, *cond_ix = nullptr, *cond_n = nullptr;
    PhiInst *phi_ix = nullptr;
    for (int pos = 0; !cond0_i0 && pos < 2; ++pos) { // 考虑i < n和n > i两种写法
      step = 0, cond_ix = &cond->lhs + pos, cond_n = &cond->lhs + !pos;
      Value *ix = cond_ix->value, *n = cond_n->value, *i = ix;
      while (true) {
        if (auto x = dyn_cast<BinaryInst>(i)) {
          // 这里只考虑Add，因为gvn_gcm pass会把减常数变成加相反数
          if (auto r = dyn_cast<ConstValue>(x->rhs.value); r && x->tag == Value::Tag::Add) {
            step += r->imm;
            i = x->lhs.value;
          } else break;
        } else if (auto x = dyn_cast<PhiInst>(i)) {
          if (x->bb != bb_header) break;
          assert(x->incoming_values.size() == 2);
          Value *i0 = x->incoming_values[!idx_in_header].value;
          phi_ix = x;
          if (x->incoming_values[idx_in_header].value != ix) break;
          // 两个cond的n必须是同一个，这保证了bb_body中的cond的n不是循环中定义的
          if (cond->tag == cond0->tag && equiv(i0, cond0->lhs.value) && equiv(n, cond0->rhs.value)) {
            cond0_i0 = &cond0->lhs, cond0_n = &cond0->rhs;
            break;
          } else if (op::isrev((op::Op) cond->tag, (op::Op) cond0->tag) && equiv(i0, cond0->rhs.value) && equiv(n, cond0->lhs.value)) {
            cond0_i0 = &cond0->rhs, cond0_n = &cond0->lhs;
            break;
          } else break;
        } else break;
      }
    }
    if (!cond0_i0) continue;

    assert(!isa<PhiInst>(bb_body->insts.head));
    // 把bb_header中的非phi/跳转指令移动到bb_body，此时还不确定是否应该展开，但是这个转移是可以做的，方便后面算指令条数
    for (Inst *i = bb_header->insts.tail->prev; !isa<PhiInst>(i);) { // 不用判断i是否为null，因为bb_header中至少存在一条定义i的phi
      Inst *prev = i->prev;
      bb_header->insts.remove(i);
      bb_body->insts.insertAtBegin(i); // 不用担心插在phi前了，因为bb_body中没有phi
      i->bb = bb_body;
      i = prev;
      assert(i != nullptr);
    }

    bool inst_ok = true;
    int inst_cnt = 0;
    for (Inst *i = bb_body->insts.head; inst_ok && i; i = i->next) {
      if (isa<BranchInst>(i) || isa<MemOpInst>(i)) continue;
      // 包含call的循环没有什么展开的必要
      // 目前不考虑有局部数组的情形，memdep应该不能处理多个局部数组对应同一个Decl
      else if (isa<CallInst>(i) || isa<AllocaInst>(i) || ++inst_cnt >= 16) inst_ok = false;
    }
    if (!inst_ok) continue;

    dbg("Performing loop unroll");
    // 验证结束，循环展开，目前为了实现简单仅展开2次，如果实现正确的话运行n次就可以展开2^n次
    Value *old_n = cond0_n->value, *new_n = new BinaryInst(Value::Tag::Add, cond0_n->value, new ConstValue(-step), cond0);
    cond0_n->set(new_n);
    cond_n->set(new_n);

    std::unordered_map<Value *, Value *> map;
    for (Inst *i = bb_header->insts.head;; i = i->next) {
      if (auto x = dyn_cast<PhiInst>(i)) {
        map.insert({x, x->incoming_values[idx_in_header].value});
      } else break;
    }
    bb_body->insts.remove(br); // 后面需要往bb_body的最后insert，所以先把跳转指令去掉，等下再加回来
    Inst *orig_last = bb_body->insts.tail; // 不可能为null，因为bb_body中至少存在一条计算i1的指令 todo: 真的吗？上面并没有判断至少有一个BinaryInst
    assert(orig_last != nullptr);
    for (Inst *i = bb_body->insts.head;; i = i->next) {
      clone_inst(i, bb_body, map);
      if (i == orig_last) break;
    }
    bb_body->insts.insertAtEnd(br);

    auto get = [&map](Value *v) {
      auto it = map.find(v);
      return it != map.end() ? it->second : v;
    };
    br->cond.set(get(cond));

    u32 idx_in_end = bb_body == bb_end->pred[1];
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
      // fixme: 这里生成的phi序列可能出现定义在使用后面，llvm不接受这样的程序，我们的后端能够处理吗？
      for (Inst *i = bb_header->insts.head;; i = i->next) {
        if (auto x = dyn_cast<PhiInst>(i)) {
          Value *from_header = x->incoming_values[!idx_in_header].value, *from_body = get(x->incoming_values[idx_in_header].value);
          x->incoming_values[idx_in_header].set(from_body);
          auto p = new PhiInst(bb_if);
          p->incoming_values[0].set(from_header);
          p->incoming_values[1].set(from_body);
          auto it = map.find(x);
          assert(it != map.end());
          it->second = p;
        } else break;
      }
      // bb_header和bb_end中的phi并不一定完全一样，有可能一个值只在循环内用到，也有可能一个循环内定义的值循环中却没有用到
      for (Inst *i = bb_end->insts.head;; i = i->next) {
        if (auto x = dyn_cast<PhiInst>(i)) {
          Value *from_header = x->incoming_values[!idx_in_end].value, *from_body = get(x->incoming_values[idx_in_end].value);
          bool found = false;
          for (Inst *j = bb_if->insts.head; !found; j = j->next) {
            if (auto y = dyn_cast<PhiInst>(j)) {
              if (equiv(y->incoming_values[0].value, from_header) && equiv(y->incoming_values[1].value, from_body)) {
                x->incoming_values[!idx_in_end].set(y);
                found = true;
              }
            } else break;
          }
          if (!found) {
            auto p = new PhiInst(bb_if);
            p->incoming_values[0].set(from_header);
            p->incoming_values[1].set(from_body);
            x->incoming_values[!idx_in_end].set(p);
          }
        } else break;
      }
      Value *phi_ix1 = get(phi_ix), *n = old_n;
      if (cond_ix == &cond->rhs) std::swap(phi_ix1, n);
      if_cond->lhs.set(phi_ix1);
      if_cond->rhs.set(n);
    }
    bb_end->pred[!idx_in_end] = bb_if;
    bb_end->pred[idx_in_end] = bb_last;
    for (Inst *i = bb_body->insts.head;; i = i->next) {
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
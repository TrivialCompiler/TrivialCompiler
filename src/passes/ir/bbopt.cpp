#include "bbopt.hpp"

static void dfs(BasicBlock *bb) {
  if (!bb->vis) {
    bb->vis = true;
    for (BasicBlock *x : bb->succ()) {
      if (x) dfs(x);
    }
  }
}

void bbopt(IrFunc *f) {
  bool changed;
  do {
    changed = false;
    // 这个循环本来只是为了消除if (常数)，没有必要放在do while里的，但是在这里消除if (x) br a else br a也比较方便
    // 后面这种情形会被下面的循环引入，所以这个循环也放在do while里
    for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
      if (auto x = dyn_cast<BranchInst>(bb->insts.tail)) {
        BasicBlock *deleted = nullptr;
        if (auto cond = dyn_cast<ConstValue>(x->cond.value)) {
          new JumpInst(cond->imm ? x->left : x->right, bb);
          deleted = cond->imm ? x->right : x->left;
        } else if (x->left == x->right) { // 可能被消除以jump结尾的空基本块引入
          new JumpInst(x->left, bb);
          deleted = x->right;
          changed = true; // 可能引入新的以jump结尾的空基本块
        }
        if (deleted) {
          bb->insts.remove(x);
          delete x;
          u32 idx = std::find(deleted->pred.begin(), deleted->pred.end(), bb) - deleted->pred.begin();
          deleted->pred.erase(deleted->pred.begin() + idx);
          for (Inst *i = deleted->insts.head;; i = i->next) {
            if (auto phi = dyn_cast<PhiInst>(i)) phi->incoming_values.erase(phi->incoming_values.begin() + idx);
            else break;
          }
        }
      }
    }
    // 这个循环消除以jump结尾的空基本块
    // 简单起见不考虑第一个bb，因为第一个bb是entry，把它删了需要把新的entry移动到第一个来
    for (BasicBlock *bb = f->bb.head->next; bb;) {
      BasicBlock *next = bb->next;
      // 要求target != bb，避免去掉空的死循环
      if (auto x = dyn_cast<JumpInst>(bb->insts.tail); x && x->next != bb && bb->insts.head == bb->insts.tail) {
        BasicBlock *target = x->next;
        // 如果存在一个pred，它以BranchInst结尾，且left或right已经为target，且target中存在phi，则不能把另一个也变成bb
        // 例如 bb1: { b = a + 1; if (x) br bb2 else br bb3 } bb2: { br bb3; } bb3: { c = phi [a, bb1] [b bb2] }
        // 这时bb2起到了一个区分phi来源的作用
        if (isa<PhiInst>(target->insts.head)) {
          for (BasicBlock *p : bb->pred) {
            if (auto br = dyn_cast<BranchInst>(p->insts.tail)) {
              if (br->left == target || br->right == target) goto end;
            }
          }
        }
        u32 idx = std::find(target->pred.begin(), target->pred.end(), bb) - target->pred.begin();
        target->pred.erase(target->pred.begin() + idx);
        for (BasicBlock *p : bb->pred) {
          auto succ = p->succ_ref();
          **std::find_if(succ.begin(), succ.end(), [bb](BasicBlock **y) { return *y == bb; }) = target;
          target->pred.push_back(p);
        }
        u32 n_pred = bb->pred.size();
        for (Inst *i = target->insts.head;; i = i->next) {
          if (auto phi = dyn_cast<PhiInst>(i)) {
            Value *v = phi->incoming_values[idx].value;
            phi->incoming_values.erase(phi->incoming_values.begin() + idx);
            for (u32 j = 0; j < n_pred; ++j) {
              phi->incoming_values.emplace_back(v, phi);
            }
          } else break;
        }
        f->bb.remove(bb);
        delete bb;
        changed = true;
      }
      end:;
      bb = next;
    }
  } while (changed);

  f->clear_all_vis();
  dfs(f->bb.head);
  // 不可达的bb仍然可能有指向可达的bb的边，需要删掉目标bb中的pred和phi中的这一项
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    if (!bb->vis) {
      for (BasicBlock *s : bb->succ()) {
        if (s && s->vis) {
          u32 idx = std::find(s->pred.begin(), s->pred.end(), bb) - s->pred.begin();
          s->pred.erase(s->pred.begin() + idx);
          for (Inst *i = s->insts.head;; i = i->next) {
            if (auto x = dyn_cast<PhiInst>(i))x->incoming_values.erase(x->incoming_values.begin() + idx);
            else break;
          }
        }
      }
    }
  }
  for (BasicBlock *bb = f->bb.head; bb;) {
    BasicBlock *next = bb->next;
    if (!bb->vis) {
      f->bb.remove(bb);
      delete bb;
    }
    bb = next;
  }

  // 合并无条件跳转，这对性能没有影响，但是可以让其他优化更好写
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    if (auto x = dyn_cast<JumpInst>(bb->insts.tail)) {
      BasicBlock *target = x->next;
      if (target->pred.size() == 1 && !isa<PhiInst>(target->insts.head)) {
        for (Inst *i = target->insts.head; i;) {
          Inst *next = i->next;
          target->insts.remove(i);
          bb->insts.insertBefore(i, x);
          i->bb = bb;
          i = next;
        }
        bb->insts.remove(x);
        delete x;
        for (BasicBlock *s : bb->succ()) {
          if (s) { *std::find(s->pred.begin(), s->pred.end(), target) = bb; }
        }
        f->bb.remove(target);
        delete target;
      }
    }
  }
}
#include "remove_identical_branch.hpp"

static bool not_singly_used(Inst *i) { return i->uses.head != i->uses.tail; }

// 目前这个pass的唯一作用是针对mv测例进行优化，它经过前面的优化之后得到的pattern类似
// bb_if:
//   ...
//   if (x == 0) br bb_then else br bb_else
// bb_then:
//   br bb_after
// bb_else:
//   t1 = a[i]
//   t2 = b[j]
//   t3 = x * t2
//   t4 = t1 + t3
//   a[j] = t4
//   br bb_after
// bb_after:
//   ...
// 目前我没有想到什么general的方法可以优化这种if
void remove_identical_branch(IrFunc *f) {
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    auto br = dyn_cast<BranchInst>(bb->insts.tail);
    if (!br) continue;
    auto cond = dyn_cast<BinaryInst>(br->cond.value);
    if (!cond || cond->tag != Value::Tag::Eq || cond->rhs.value != ConstValue::get(0)) continue;
    BasicBlock *left = br->left, *right = br->right;
    if (left->insts.head != left->insts.tail || left->pred.size() != 1 || right->pred.size() != 1) continue;
    auto j1 = dyn_cast<JumpInst>(left->insts.tail), j2 = dyn_cast<JumpInst>(right->insts.tail);
    if (!j1 || !j2 || j1->next != j2->next) continue;
    BasicBlock *after = j1->next;
    if (isa<PhiInst>(after->insts.head)) continue;
    auto i1 = dyn_cast<LoadInst>(right->insts.head);
    if (!i1 || !isa<ParamRef>(i1->arr.value) || not_singly_used(i1)) continue;
    auto i2 = dyn_cast<LoadInst>(i1->next);
    if (!i2 || !isa<ParamRef>(i2->arr.value) || i2->arr.value == i1->arr.value || not_singly_used(i2)) continue;
    auto i3 = dyn_cast<BinaryInst>(i2->next);
    if (!i3 || i3->tag != Value::Tag::Mul || i3->lhs.value != cond->lhs.value || i3->rhs.value != i2 || not_singly_used(i3)) continue;
    auto i4 = dyn_cast<BinaryInst>(i3->next);
    if (!i4 || i4->tag != Value::Tag::Add || i4->lhs.value != i1 || i4->rhs.value != i3 || not_singly_used(i4)) continue;
    auto i5 = dyn_cast<StoreInst>(i4->next);
    if (!i5 || i5->arr.value != i1->arr.value || i5->next != j2) continue;
    dbg("Performing remove identical branch");
    bb->insts.remove(br);
    delete br;
    new JumpInst(right, bb);
    f->bb.remove(left);
    after->pred.erase(std::find(after->pred.begin(), after->pred.end(), left));
    delete left;
    break;
  }
}
#include "fold_counted_div_loop.hpp"

#include <algorithm>
#include <unordered_set>

#include "../../structure/op.hpp"

namespace {

bool is_positive_power_of_two(i32 x) {
  if (x <= 0) return false;
  auto ux = static_cast<u32>(x);
  return (ux & (ux - 1)) == 0;
}

int log2_exact(i32 x) {
  int res = 0;
  for (auto ux = static_cast<u32>(x); ux > 1; ux >>= 1) ++res;
  return res;
}

BinaryInst *as_compare(Value *v) {
  auto x = dyn_cast<BinaryInst>(v);
  if (!x) return nullptr;
  if (x->tag >= Value::Tag::Lt && x->tag <= Value::Tag::Gt) return x;
  if (x->tag == Value::Tag::Ne) {
    auto r = dyn_cast<ConstValue>(x->rhs.value);
    auto cmp = dyn_cast<BinaryInst>(x->lhs.value);
    if (r && r->imm == 0 && cmp && cmp->tag >= Value::Tag::Lt && cmp->tag <= Value::Tag::Gt) return cmp;
  }
  return nullptr;
}

Value *incoming_from(PhiInst *phi, BasicBlock *pred) {
  auto it = std::find(phi->incoming_bbs().begin(), phi->incoming_bbs().end(), pred);
  assert(it != phi->incoming_bbs().end());
  return phi->incoming_values[it - phi->incoming_bbs().begin()].value;
}

bool is_add_one(Value *v, PhiInst *phi) {
  auto x = dyn_cast<BinaryInst>(v);
  auto rhs = x ? dyn_cast<ConstValue>(x->rhs.value) : nullptr;
  return x && x->tag == Value::Tag::Add && x->lhs.value == phi && rhs && rhs->imm == 1;
}

bool is_div_rec(Value *v, PhiInst *phi, int &shift_per_iter) {
  auto x = dyn_cast<BinaryInst>(v);
  auto rhs = x ? dyn_cast<ConstValue>(x->rhs.value) : nullptr;
  if (!x || x->tag != Value::Tag::Div || x->lhs.value != phi || !rhs || !is_positive_power_of_two(rhs->imm)) return false;
  shift_per_iter = log2_exact(rhs->imm);
  return true;
}

bool match_trip_count(BinaryInst *cmp, PhiInst *ind_phi, int &trip_count) {
  ConstValue *bound = nullptr;
  bool ok = false;
  if (cmp->tag == Value::Tag::Lt && cmp->lhs.value == ind_phi) {
    bound = dyn_cast<ConstValue>(cmp->rhs.value);
    ok = true;
  } else if (cmp->tag == Value::Tag::Gt && cmp->rhs.value == ind_phi) {
    bound = dyn_cast<ConstValue>(cmp->lhs.value);
    ok = true;
  }
  if (!ok || !bound || bound->imm < 0) return false;
  trip_count = bound->imm;
  return true;
}

Inst *first_non_phi(BasicBlock *bb) {
  Inst *i = bb->insts.head;
  while (i && isa<PhiInst>(i)) i = i->next;
  return i;
}

Value *make_folded_value(Value *start, int shift, BasicBlock *insert_bb) {
  if (shift == 0) return start;
  if (shift >= 32) return ConstValue::get(0);
  if (shift >= 31) return nullptr;
  if (Inst *insert_before = first_non_phi(insert_bb)) {
    return new BinaryInst(Value::Tag::Div, start, ConstValue::get(1 << shift), insert_before);
  }
  return new BinaryInst(Value::Tag::Div, start, ConstValue::get(1 << shift), insert_bb);
}

bool all_external_uses_in(PhiInst *phi, const std::unordered_set<BasicBlock *> &loop_bbs, BasicBlock *exit) {
  for (Use *u = phi->uses.head; u; u = u->next) {
    if (!loop_bbs.count(u->user->bb) && u->user->bb != exit) return false;
  }
  return true;
}

void replace_external_uses(PhiInst *phi, Value *replacement, const std::unordered_set<BasicBlock *> &loop_bbs) {
  for (Use *u = phi->uses.head; u;) {
    Use *next = u->next;
    if (!loop_bbs.count(u->user->bb)) u->set(replacement);
    u = next;
  }
}

}  // namespace

void fold_counted_div_loop(IrFunc *f) {
  for (BasicBlock *header = f->bb.head; header; header = header->next) {
    auto br = dyn_cast<BranchInst>(header->insts.tail);
    if (!br) continue;

    BasicBlock *body = br->left;
    BasicBlock *exit = br->right;
    auto jump = body ? dyn_cast<JumpInst>(body->insts.tail) : nullptr;
    if (!body || !exit || !jump || jump->next != header) continue;
    if (header->pred.size() != 2 || body->pred.size() != 1 || body->pred[0] != header) continue;

    BasicBlock *preheader = header->pred[0] == body ? header->pred[1] : header->pred[0];
    auto cmp = as_compare(br->cond.value);
    if (!cmp) continue;

    PhiInst *ind_phi = nullptr;
    int trip_count = -1;
    for (Inst *i = header->insts.head; i; i = i->next) {
      auto phi = dyn_cast<PhiInst>(i);
      if (!phi) break;
      auto init = dyn_cast<ConstValue>(incoming_from(phi, preheader));
      if (init && init->imm == 0 && is_add_one(incoming_from(phi, body), phi) && match_trip_count(cmp, phi, trip_count)) {
        ind_phi = phi;
        break;
      }
    }
    if (!ind_phi) continue;

    std::unordered_set<BasicBlock *> loop_bbs = {header, body};
    for (Inst *i = header->insts.head; i; i = i->next) {
      auto phi = dyn_cast<PhiInst>(i);
      if (!phi || phi == ind_phi) continue;

      int shift_per_iter = 0;
      if (!is_div_rec(incoming_from(phi, body), phi, shift_per_iter)) continue;
      if (!all_external_uses_in(phi, loop_bbs, exit)) continue;

      int shift = shift_per_iter * trip_count;
      Value *folded = make_folded_value(incoming_from(phi, preheader), shift, exit);
      if (!folded) continue;

      dbg("Folding counted division loop");
      replace_external_uses(phi, folded, loop_bbs);
    }
  }
}

#include "tighten_guarded_loop_bound.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace {

bool is_cmp(Value::Tag tag) {
  return tag == Value::Tag::Lt || tag == Value::Tag::Le || tag == Value::Tag::Ge || tag == Value::Tag::Gt;
}

Value::Tag invert_cmp(Value::Tag tag) {
  switch (tag) {
    case Value::Tag::Lt:
      return Value::Tag::Ge;
    case Value::Tag::Le:
      return Value::Tag::Gt;
    case Value::Tag::Ge:
      return Value::Tag::Lt;
    case Value::Tag::Gt:
      return Value::Tag::Le;
    default:
      UNREACHABLE();
  }
}

Value::Tag swap_cmp(Value::Tag tag) {
  switch (tag) {
    case Value::Tag::Lt:
      return Value::Tag::Gt;
    case Value::Tag::Le:
      return Value::Tag::Ge;
    case Value::Tag::Ge:
      return Value::Tag::Le;
    case Value::Tag::Gt:
      return Value::Tag::Lt;
    default:
      UNREACHABLE();
  }
}

int pred_index(BasicBlock *bb, BasicBlock *pred) {
  auto it = std::find(bb->pred.begin(), bb->pred.end(), pred);
  return it == bb->pred.end() ? -1 : static_cast<int>(it - bb->pred.begin());
}

bool has_phi(BasicBlock *bb) { return isa<PhiInst>(bb->insts.head); }

bool defined_in(Value *v, const std::array<BasicBlock *, 3> &blocks) {
  auto inst = dyn_cast<Inst>(v);
  return inst && std::find(blocks.begin(), blocks.end(), inst->bb) != blocks.end();
}

bool all_uses_in(Value *v, const std::array<BasicBlock *, 3> &blocks) {
  for (Use *u = v->uses.head; u; u = u->next) {
    if (std::find(blocks.begin(), blocks.end(), u->user->bb) == blocks.end()) return false;
  }
  return true;
}

bool match_positive_step(Value *v, PhiInst *iv, int &step) {
  auto add = dyn_cast<BinaryInst>(v);
  if (!add || add->tag != Value::Tag::Add) return false;
  if (add->lhs.value == iv) {
    auto c = dyn_cast<ConstValue>(add->rhs.value);
    if (!c || c->imm <= 0) return false;
    step = c->imm;
    return true;
  }
  if (add->rhs.value == iv) {
    auto c = dyn_cast<ConstValue>(add->lhs.value);
    if (!c || c->imm <= 0) return false;
    step = c->imm;
    return true;
  }
  return false;
}

bool only_header_phi_uses(Value *v, BasicBlock *header) {
  for (Use *u = v->uses.head; u; u = u->next) {
    if (u->user->bb != header || !isa<PhiInst>(u->user)) return false;
  }
  return true;
}

bool body_prefix_condition(BinaryInst *cmp, bool true_to_body, PhiInst *iv, Value::Tag &tag, Value *&bound) {
  if (!is_cmp(cmp->tag)) return false;

  tag = true_to_body ? cmp->tag : invert_cmp(cmp->tag);
  Value *lhs = cmp->lhs.value;
  Value *rhs = cmp->rhs.value;
  if (lhs == iv) {
    bound = rhs;
  } else if (rhs == iv) {
    tag = swap_cmp(tag);
    bound = lhs;
  } else {
    return false;
  }

  // With a positive IV step, only an initial true range can replace a skipped tail.
  return tag == Value::Tag::Lt || tag == Value::Tag::Le;
}

Value *exclusive_bound(Value::Tag tag, Value *bound, BasicBlock *bb) {
  if (tag == Value::Tag::Lt) return bound;
  if (tag == Value::Tag::Le) return new BinaryInst(Value::Tag::Add, bound, ConstValue::get(1), bb);
  return nullptr;
}

BasicBlock *single_preheader(BasicBlock *header, BasicBlock *guard, BasicBlock *body) {
  BasicBlock *preheader = nullptr;
  for (BasicBlock *pred : header->pred) {
    if (pred == guard || pred == body) continue;
    if (preheader) return nullptr;
    preheader = pred;
  }
  return preheader;
}

bool replace_successor(BasicBlock *from, BasicBlock *old_succ, BasicBlock *new_succ) {
  auto succ = from->succ_ref();
  for (BasicBlock **ref : succ) {
    if (ref && *ref == old_succ) {
      *ref = new_succ;
      return true;
    }
  }
  return false;
}

bool has_successor(BasicBlock *from, BasicBlock *succ_bb) {
  auto succ = from->succ_ref();
  return std::any_of(succ.begin(), succ.end(), [succ_bb](BasicBlock **ref) { return ref && *ref == succ_bb; });
}

bool split_preheader_with_min_bound(IrFunc *f, BasicBlock *header, BasicBlock *preheader, PhiInst *iv,
                                    Value::Tag lhs_tag, Value *lhs_bound, Value::Tag rhs_tag, Value *rhs_bound) {
  int preheader_idx = pred_index(header, preheader);
  if (preheader_idx < 0) return false;
  if (!has_successor(preheader, header)) return false;
  if ((lhs_tag != Value::Tag::Lt && lhs_tag != Value::Tag::Le) ||
      (rhs_tag != Value::Tag::Lt && rhs_tag != Value::Tag::Le)) {
    return false;
  }

  auto cmp_bb = new BasicBlock;
  auto lhs_bb = new BasicBlock;
  auto rhs_bb = new BasicBlock;
  f->bb.insertBefore(cmp_bb, header);
  f->bb.insertBefore(lhs_bb, header);
  f->bb.insertBefore(rhs_bb, header);

  auto lhs_exclusive = exclusive_bound(lhs_tag, lhs_bound, cmp_bb);
  auto rhs_exclusive = exclusive_bound(rhs_tag, rhs_bound, cmp_bb);
  if (!lhs_exclusive || !rhs_exclusive) return false;
  auto cmp = new BinaryInst(Value::Tag::Lt, lhs_exclusive, rhs_exclusive, cmp_bb);
  new BranchInst(cmp, lhs_bb, rhs_bb, cmp_bb);
  new JumpInst(header, lhs_bb);
  new JumpInst(header, rhs_bb);

  [[maybe_unused]] bool replaced = replace_successor(preheader, header, cmp_bb);
  assert(replaced);
  cmp_bb->pred.push_back(preheader);
  lhs_bb->pred.push_back(cmp_bb);
  rhs_bb->pred.push_back(cmp_bb);

  header->pred[preheader_idx] = lhs_bb;
  header->pred.push_back(rhs_bb);

  for (Inst *inst = header->insts.head; inst; inst = inst->next) {
    auto phi = dyn_cast<PhiInst>(inst);
    if (!phi) break;
    phi->incoming_values.emplace_back(phi->incoming_values[preheader_idx].value, phi);
  }

  auto limit = new PhiInst(header);
  for (u32 i = 0; i < limit->incoming_values.size(); ++i) {
    BasicBlock *pred = header->pred[i];
    if (pred == lhs_bb) {
      limit->incoming_values[i].set(lhs_exclusive);
    } else if (pred == rhs_bb) {
      limit->incoming_values[i].set(rhs_exclusive);
    } else {
      limit->incoming_values[i].set(limit);
    }
  }

  auto header_br = dyn_cast<BranchInst>(header->insts.tail);
  assert(header_br);
  auto new_cmp = new BinaryInst(Value::Tag::Lt, iv, limit, header_br);
  header_br->cond.set(new_cmp);
  return true;
}

bool guard_block_is_simple(BasicBlock *guard, BinaryInst *cmp, BinaryInst *step_inst) {
  for (Inst *inst = guard->insts.head; inst; inst = inst->next) {
    if (isa<BranchInst>(inst)) return inst == guard->insts.tail;
    if (inst == cmp || inst == step_inst) continue;
    return false;
  }
  return false;
}

void delete_block(BasicBlock *bb, IrFunc *f) {
  std::vector<Inst *> insts;
  for (Inst *inst = bb->insts.head; inst; inst = inst->next) {
    insts.push_back(inst);
    for (auto [it, end] = inst->operands(); it < end; ++it) it->set(nullptr);
  }
  for (Inst *inst : insts) {
    bb->insts.remove(inst);
    inst->deleteValue();
  }
  f->bb.remove(bb);
  delete bb;
}

bool try_tighten(BasicBlock *header, IrFunc *f) {
  auto header_br = dyn_cast<BranchInst>(header->insts.tail);
  if (!header_br) return false;

  BasicBlock *guard = header_br->left;
  BasicBlock *exit = header_br->right;
  if (!guard || !exit || guard->pred.size() != 1 || guard->pred[0] != header) return false;

  auto guard_br = dyn_cast<BranchInst>(guard->insts.tail);
  if (!guard_br) return false;

  BasicBlock *body = nullptr;
  bool true_to_body = false;
  if (guard_br->left == header && guard_br->right != header) {
    body = guard_br->right;
    true_to_body = false;
  } else if (guard_br->right == header && guard_br->left != header) {
    body = guard_br->left;
    true_to_body = true;
  } else {
    return false;
  }
  if (!body || body == exit || body->pred.size() != 1 || body->pred[0] != guard || has_phi(body)) return false;
  auto body_jump = dyn_cast<JumpInst>(body->insts.tail);
  if (!body_jump || body_jump->next != header) return false;

  int guard_idx = pred_index(header, guard);
  int body_idx = pred_index(header, body);
  if (guard_idx < 0 || body_idx < 0) return false;

  auto guard_cmp = dyn_cast<BinaryInst>(guard_br->cond.value);
  if (!guard_cmp) return false;

  std::array<BasicBlock *, 3> local_blocks{header, guard, body};

  PhiInst *iv = nullptr;
  BinaryInst *old_step = nullptr;
  int step = 0;
  for (Inst *inst = header->insts.head; inst; inst = inst->next) {
    auto phi = dyn_cast<PhiInst>(inst);
    if (!phi) break;
    if (!all_uses_in(phi, local_blocks)) return false;
    Value *from_guard = phi->incoming_values[guard_idx].value;
    if (from_guard != phi->incoming_values[body_idx].value) continue;
    int candidate_step = 0;
    if (match_positive_step(from_guard, phi, candidate_step)) {
      iv = phi;
      old_step = static_cast<BinaryInst *>(from_guard);
      step = candidate_step;
      break;
    }
  }
  if (!iv || old_step->bb != guard || !only_header_phi_uses(old_step, header)) return false;
  if (!guard_block_is_simple(guard, guard_cmp, old_step)) return false;

  Value::Tag body_cmp_tag;
  Value *body_cmp_bound = nullptr;
  if (!body_prefix_condition(guard_cmp, true_to_body, iv, body_cmp_tag, body_cmp_bound)) return false;
  if (defined_in(body_cmp_bound, local_blocks)) return false;

  auto header_cmp = dyn_cast<BinaryInst>(header_br->cond.value);
  Value::Tag header_cmp_tag{};
  Value *header_cmp_bound = nullptr;
  bool can_use_min_bound =
      step == 1 && header_cmp && body_prefix_condition(header_cmp, true, iv, header_cmp_tag, header_cmp_bound) &&
      !defined_in(header_cmp_bound, local_blocks);
  BasicBlock *preheader = can_use_min_bound ? single_preheader(header, guard, body) : nullptr;
  can_use_min_bound = can_use_min_bound && preheader;

  for (Inst *inst = header->insts.head; inst; inst = inst->next) {
    auto phi = dyn_cast<PhiInst>(inst);
    if (!phi) break;
    if (phi != iv && phi->incoming_values[guard_idx].value != phi->incoming_values[body_idx].value) return false;
  }

  dbg("Tightening guarded loop bound");
  if (!can_use_min_bound) {
    auto body_cmp = new BinaryInst(body_cmp_tag, iv, body_cmp_bound, header_br);
    auto combined_cmp = new BinaryInst(Value::Tag::And, header_br->cond.value, body_cmp, header_br);
    header_br->cond.set(combined_cmp);
  }
  header_br->left = body;

  auto new_step = new BinaryInst(Value::Tag::Add, iv, ConstValue::get(step), body_jump);

  body->pred[0] = header;
  for (Inst *inst = header->insts.head; inst; inst = inst->next) {
    auto phi = dyn_cast<PhiInst>(inst);
    if (!phi) break;
    phi->incoming_values.erase(phi->incoming_values.begin() + guard_idx);
  }
  header->pred.erase(header->pred.begin() + guard_idx);
  int new_body_idx = body_idx - (guard_idx < body_idx ? 1 : 0);
  iv->incoming_values[new_body_idx].set(new_step);

  delete_block(guard, f);
  if (can_use_min_bound) {
    split_preheader_with_min_bound(f, header, preheader, iv, header_cmp_tag, header_cmp_bound, body_cmp_tag,
                                   body_cmp_bound);
  }
  return true;
}

}  // namespace

void tighten_guarded_loop_bound(IrFunc *f) {
  for (BasicBlock *bb = f->bb.head; bb;) {
    BasicBlock *next = bb->next;
    bb = try_tighten(bb, f) ? f->bb.head : next;
  }
}

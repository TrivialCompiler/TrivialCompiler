#include "if_to_cond.hpp"

namespace {

bool is_simple_move(MachineInst *inst) {
  auto mv = dyn_cast<MIMove>(inst);
  return mv && mv->is_simple();
}

bool can_apply_cond(MachineInst *inst) {
  if (auto x = dyn_cast<MIBinary>(inst)) {
    return x->cond == ArmCond::Any && x->tag != MachineInst::Tag::Div;
  } else if (auto x = dyn_cast<MIAccess>(inst)) {
    return x->cond == ArmCond::Any;
  } else if (auto x = dyn_cast<MIFma>(inst)) {
    return x->cond == ArmCond::Any;
  } else if (auto x = dyn_cast<MIMove>(inst)) {
    return x->cond == ArmCond::Any;
  }
  return false;
}

void apply_cond(MachineInst *inst, ArmCond cond) {
  if (auto x = dyn_cast<MIBinary>(inst)) {
    x->cond = cond;
  } else if (auto x = dyn_cast<MIAccess>(inst)) {
    x->cond = cond;
  } else if (auto x = dyn_cast<MIFma>(inst)) {
    x->cond = cond;
  } else if (auto x = dyn_cast<MIMove>(inst)) {
    x->cond = cond;
  } else {
    UNREACHABLE();
  }
}

bool convert_block(MachineBB *bb, MachineInst *end, ArmCond cond, u32 max_count) {
  u32 count = 0;
  for (auto inst = bb->insts.head; inst != end; inst = inst->next) {
    ++count;
    if (!can_apply_cond(inst)) return false;
  }
  if (count == 0 || count > max_count) return false;

  for (auto inst = bb->insts.head; inst != end; inst = inst->next) {
    apply_cond(inst, cond);
  }
  return true;
}

bool rotate_simple_loop(MachineBB *header) {
  MICompare *cmp = nullptr;
  MIBranch *exit_branch = nullptr;
  MachineInst *after_cmp = nullptr;
  for (auto inst = header->insts.head; inst; inst = inst->next) {
    cmp = dyn_cast<MICompare>(inst);
    if (cmp) {
      after_cmp = cmp->next;
      exit_branch = dyn_cast_nullable<MIBranch>(after_cmp);
      for (u32 count = 0; !exit_branch && after_cmp && count < 2; ++count) {
        if (!is_simple_move(after_cmp)) break;
        after_cmp = after_cmp->next;
        exit_branch = dyn_cast_nullable<MIBranch>(after_cmp);
      }
    }
    if (exit_branch) break;
  }
  auto body = header->next;
  if (!cmp || !exit_branch || !body || exit_branch->cond == ArmCond::Any || exit_branch->target == body) return false;
  if (exit_branch->next) return false;

  u32 prefix_count = 0;
  for (auto inst = header->insts.head; inst != cmp; inst = inst->next) {
    if (!is_simple_move(inst)) return false;
    if (++prefix_count > 3) return false;
  }

  auto backedge = dyn_cast_nullable<MIJump>(body->insts.tail);
  if (!backedge || backedge->target != header) return false;
  u32 body_count = 0;
  u32 memory_count = 0;
  u32 store_count = 0;
  for (auto inst = body->insts.head; inst != backedge; inst = inst->next) {
    if (isa<MIAccess>(inst)) memory_count++;
    if (isa<MIStore>(inst)) store_count++;
    body_count++;
  }
  if (body_count < 4) return false;
  // Rotate only loop shapes that have shown a clear per-iteration branch cost:
  // memory update/copy loops, read-only reductions such as dot products, and
  // larger arithmetic loops.  Shorter generic loops can regress from the
  // duplicated latch compare and extra live values.
  bool memory_update_loop = memory_count >= 3 && store_count == 1;
  bool memory_copy_loop = memory_count >= 2 && store_count == 1 && body_count >= 4;
  bool read_only_compute_loop = memory_count >= 2 && store_count == 0 && body_count >= 4;
  bool arithmetic_loop = memory_count == 0 && body_count >= 6;
  if (!memory_update_loop && !memory_copy_loop && !read_only_compute_loop && !arithmetic_loop) return false;

  for (auto inst = header->insts.head; inst != cmp; inst = inst->next) {
    auto src = static_cast<MIMove *>(inst);
    auto clone = new MIMove(backedge);
    clone->dst = src->dst;
    clone->rhs = src->rhs;
    clone->shift = src->shift;
  }

  auto cmp_clone = new MICompare(body);
  body->insts.remove(cmp_clone);
  body->insts.insertBefore(cmp_clone, backedge);
  cmp_clone->bb = body;
  cmp_clone->lhs = cmp->lhs;
  cmp_clone->rhs = cmp->rhs;

  for (auto inst = cmp->next; inst != exit_branch; inst = inst->next) {
    auto src = static_cast<MIMove *>(inst);
    auto clone = new MIMove(backedge);
    clone->dst = src->dst;
    clone->rhs = src->rhs;
    clone->shift = src->shift;
  }

  body->insts.remove(backedge);
  auto loop_branch = new MIBranch(body);
  loop_branch->cond = opposite_cond(exit_branch->cond);
  loop_branch->target = body;
  if (exit_branch->target != body->next) {
    new MIJump(exit_branch->target, body);
  }
  dbg("Rotated simple while loop branch");
  return true;
}

bool clone_header_test(MachineBB *header, MICompare *cmp, MachineInst *insert_before) {
  for (auto inst = header->insts.head; inst != cmp; inst = inst->next) {
    auto src = static_cast<MIMove *>(inst);
    auto clone = new MIMove(insert_before);
    clone->dst = src->dst;
    clone->rhs = src->rhs;
    clone->shift = src->shift;
  }

  auto cmp_clone = new MICompare(insert_before->bb);
  insert_before->bb->insts.remove(cmp_clone);
  insert_before->bb->insts.insertBefore(cmp_clone, insert_before);
  cmp_clone->bb = insert_before->bb;
  cmp_clone->lhs = cmp->lhs;
  cmp_clone->rhs = cmp->rhs;
  return true;
}

bool find_header_branch(MachineBB *header, MICompare *&cmp, MIBranch *&exit_branch) {
  cmp = nullptr;
  exit_branch = nullptr;
  for (auto inst = header->insts.head; inst; inst = inst->next) {
    cmp = dyn_cast<MICompare>(inst);
    exit_branch = cmp ? dyn_cast_nullable<MIBranch>(cmp->next) : nullptr;
    if (exit_branch) break;
  }
  if (!cmp || !exit_branch || exit_branch->cond == ArmCond::Any || exit_branch->next) return false;

  u32 prefix_count = 0;
  for (auto inst = header->insts.head; inst != cmp; inst = inst->next) {
    if (!is_simple_move(inst)) return false;
    if (++prefix_count > 3) return false;
  }
  return true;
}

bool rotate_break_latch_loop(MachineBB *header) {
  MICompare *cmp = nullptr;
  MIBranch *exit_branch = nullptr;
  if (!find_header_branch(header, cmp, exit_branch)) return false;

  auto body = header->next;
  auto latch = body ? body->next : nullptr;
  if (!body || !latch || exit_branch->target == body || exit_branch->target == latch) return false;

  auto break_branch = dyn_cast_nullable<MIBranch>(body->insts.tail);
  if (!break_branch || break_branch->target != exit_branch->target || break_branch->cond == ArmCond::Any) return false;

  auto backedge = dyn_cast_nullable<MIJump>(latch->insts.tail);
  if (!backedge || backedge->target != header) return false;

  u32 latch_count = 0;
  for (auto inst = latch->insts.head; inst != backedge; inst = inst->next) {
    if (!can_apply_cond(inst) && !isa<MICompare>(inst)) return false;
    if (++latch_count > 4) return false;
  }
  if (latch_count == 0) return false;

  clone_header_test(header, cmp, backedge);
  latch->insts.remove(backedge);
  auto loop_branch = new MIBranch(latch);
  loop_branch->cond = opposite_cond(exit_branch->cond);
  loop_branch->target = body;
  if (exit_branch->target != latch->next) {
    new MIJump(exit_branch->target, latch);
  }
  dbg("Rotated break-latch while loop branch");
  return true;
}

}  // namespace

void if_to_cond(MachineFunc* f) {
  for (auto bb = f->bb.head; bb; bb = bb->next) {
    // find pattern:
    // BB1:
    // b.cond BB3
    // BB2:
    // ...some instructions
    // BB3:
    // converts to:
    // BB1:
    // BB2:
    // ...some instructions + cond
    // BB3:
    if (auto b = dyn_cast_nullable<MIBranch>(bb->insts.tail)) {
      auto bb2 = bb->next;
      auto bb3 = b->target;
      if (bb2 && bb2->next == bb3) {
        if (convert_block(bb2, nullptr, opposite_cond(b->cond), 4)) {
          dbg("Optimizing branches to conditional execution");
          bb->insts.remove(b);
        }
      }
    }
  }

  for (auto bb = f->bb.head; bb; bb = bb->next) {
    rotate_simple_loop(bb);
  }

  for (auto bb = f->bb.head; bb; bb = bb->next) {
    rotate_break_latch_loop(bb);
  }
}

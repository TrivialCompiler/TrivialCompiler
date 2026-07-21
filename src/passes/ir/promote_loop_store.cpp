#include "promote_loop_store.hpp"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "cfg.hpp"
#include "memdep.hpp"

namespace {

bool contains(const std::vector<BasicBlock *> &blocks, BasicBlock *bb) {
  return std::find(blocks.begin(), blocks.end(), bb) != blocks.end();
}

bool defined_in_loop(Value *v, const std::vector<BasicBlock *> &blocks) {
  auto inst = dyn_cast<Inst>(v);
  return inst && contains(blocks, inst->bb);
}

int pred_index(BasicBlock *bb, BasicBlock *pred) {
  auto it = std::find(bb->pred.begin(), bb->pred.end(), pred);
  return it == bb->pred.end() ? -1 : static_cast<int>(it - bb->pred.begin());
}

Inst *first_non_phi(BasicBlock *bb) {
  Inst *inst = bb->insts.head;
  while (inst && isa<PhiInst>(inst)) inst = inst->next;
  return inst;
}

LoadInst *make_load_before_terminator(Decl *sym, Value *arr, Value *index, BasicBlock *bb) {
  Inst *term = bb->insts.tail;
  auto load = new LoadInst(sym, arr, index, bb);
  bb->insts.remove(load);
  bb->insts.insertBefore(load, term);
  return load;
}

void move_before(Inst *inst, Inst *before) {
  inst->bb->insts.remove(inst);
  before->bb->insts.insertBefore(inst, before);
  inst->bb = before->bb;
}

StoreInst *make_store_before(Decl *sym, Value *arr, Value *data, Value *index, Inst *before) {
  auto store = new StoreInst(sym, arr, data, index, before->bb);
  move_before(store, before);
  return store;
}

bool same_addr(AccessInst *lhs, AccessInst *rhs) {
  return lhs->lhs_sym == rhs->lhs_sym && lhs->arr.value == rhs->arr.value && lhs->index.value == rhs->index.value;
}

struct Candidate {
  LoadInst *load = nullptr;
  StoreInst *store = nullptr;
  BinaryInst *update = nullptr;
};

Candidate match_update_store(StoreInst *store) {
  auto update = dyn_cast<BinaryInst>(store->data.value);
  if (!update || update->tag != Value::Tag::Add) return {};

  auto lhs_load = dyn_cast<LoadInst>(update->lhs.value);
  if (lhs_load && same_addr(lhs_load, store)) return {lhs_load, store, update};

  auto rhs_load = dyn_cast<LoadInst>(update->rhs.value);
  if (rhs_load && same_addr(rhs_load, store)) return {rhs_load, store, update};

  return {};
}

bool rooted_in_param(Value *addr) {
  while (auto gep = dyn_cast<GetElementPtrInst>(addr)) addr = gep->arr.value;
  return isa<ParamRef>(addr);
}

bool memory_safe(const Candidate &candidate, const std::vector<BasicBlock *> &blocks) {
  for (BasicBlock *bb : blocks) {
    for (Inst *inst = bb->insts.head; inst; inst = inst->next) {
      if (inst == candidate.store || inst == candidate.load) continue;
      if (auto load = dyn_cast<LoadInst>(inst)) {
        if (alias(candidate.store->lhs_sym, load->lhs_sym)) return false;
      } else if (auto store = dyn_cast<StoreInst>(inst)) {
        if (alias(candidate.store->lhs_sym, store->lhs_sym)) return false;
      } else if (auto call = dyn_cast<CallInst>(inst); call && call->func->has_side_effect &&
                                                 is_arr_call_alias(candidate.store->lhs_sym, call)) {
        return false;
      }
    }
  }
  return true;
}

bool collect_loop_shape(Loop *loop, BasicBlock *&preheader, BasicBlock *&latch, BasicBlock *&exit) {
  BasicBlock *header = loop->header();
  auto header_br = dyn_cast<BranchInst>(header->insts.tail);
  if (!header_br || header->pred.size() != 2) return false;

  for (BasicBlock *pred : header->pred) {
    if (contains(loop->bbs, pred)) {
      if (latch) return false;
      latch = pred;
    } else {
      if (preheader) return false;
      preheader = pred;
    }
  }
  if (!preheader || !latch) return false;
  auto latch_jump = dyn_cast<JumpInst>(latch->insts.tail);
  if (!latch_jump || latch_jump->next != header) return false;

  for (BasicBlock *bb : loop->bbs) {
    for (BasicBlock *succ : bb->succ()) {
      if (!succ || contains(loop->bbs, succ)) continue;
      if (exit && exit != succ) return false;
      exit = succ;
      if (bb != header) return false;
    }
  }
  return exit && pred_index(exit, header) >= 0;
}

bool find_candidate(Loop *loop, Candidate &candidate) {
  for (BasicBlock *bb : loop->bbs) {
    for (Inst *inst = bb->insts.head; inst; inst = inst->next) {
      auto store = dyn_cast<StoreInst>(inst);
      if (!store) continue;
      Candidate cur = match_update_store(store);
      if (!cur.store) continue;
      if (candidate.store) return false;
      candidate = cur;
    }
  }
  if (!candidate.store) return false;
  if (defined_in_loop(candidate.store->arr.value, loop->bbs) || defined_in_loop(candidate.store->index.value, loop->bbs)) {
    return false;
  }
  if (rooted_in_param(candidate.store->arr.value)) return false;
  return memory_safe(candidate, loop->bbs);
}

void remove_store(StoreInst *store) {
  for (auto [it, end] = store->operands(); it < end; ++it) it->set(nullptr);
  store->bb->insts.remove(store);
  store->deleteValue();
}

bool try_promote(Loop *loop) {
  BasicBlock *preheader = nullptr;
  BasicBlock *latch = nullptr;
  BasicBlock *exit = nullptr;
  if (!collect_loop_shape(loop, preheader, latch, exit)) return false;

  Candidate candidate;
  if (!find_candidate(loop, candidate)) return false;
  if (candidate.store->bb != latch && pred_index(latch, candidate.store->bb) < 0) return false;

  dbg("Promoting loop-carried store to scalar");

  auto init = make_load_before_terminator(candidate.store->lhs_sym, candidate.store->arr.value, candidate.store->index.value, preheader);
  auto acc = new PhiInst(loop->header());
  candidate.load->replaceAllUseWith(acc);
  Value *updated = candidate.update;

  Value *next_acc = nullptr;
  if (candidate.store->bb == latch) {
    next_acc = updated;
  } else {
    auto latch_phi = new PhiInst(latch);
    for (u32 i = 0; i < latch_phi->incoming_values.size(); ++i) {
      latch_phi->incoming_values[i].set(latch_phi->incoming_bbs()[i] == candidate.store->bb ? updated : static_cast<Value *>(acc));
    }
    next_acc = latch_phi;
  }

  int pre_idx = pred_index(loop->header(), preheader);
  int latch_idx = pred_index(loop->header(), latch);
  assert(pre_idx >= 0 && latch_idx >= 0);
  acc->incoming_values[pre_idx].set(init);
  acc->incoming_values[latch_idx].set(next_acc);

  Inst *exit_insert = first_non_phi(exit);
  assert(exit_insert);
  make_store_before(candidate.store->lhs_sym, candidate.store->arr.value, acc, candidate.store->index.value, exit_insert);
  remove_store(candidate.store);
  return true;
}

}  // namespace

void promote_loop_store(IrFunc *f) {
  auto loops = compute_loop_info(f).deepest_loops();
  for (Loop *loop : loops) {
    try_promote(loop);
  }
}

#include "remove_dead_local_init.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

#include "../../structure/ast.hpp"

namespace {

constexpr int MAX_TRACKED_ELEMS = 64;

AllocaInst *root_alloca(Value *v) {
  if (auto x = dyn_cast<AllocaInst>(v)) return x;
  if (auto x = dyn_cast<GetElementPtrInst>(v)) return root_alloca(x->arr.value);
  return nullptr;
}

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

Value *incoming_from(PhiInst *phi, BasicBlock *pred) {
  int idx = pred_index(phi->bb, pred);
  assert(idx >= 0);
  return phi->incoming_values[idx].value;
}

BinaryInst *as_compare(Value *v) {
  auto x = dyn_cast<BinaryInst>(v);
  if (!x) return nullptr;
  if (is_cmp(x->tag)) return x;
  if (x->tag == Value::Tag::Ne) {
    auto rhs = dyn_cast<ConstValue>(x->rhs.value);
    auto cmp = dyn_cast<BinaryInst>(x->lhs.value);
    if (rhs && rhs->imm == 0 && cmp && is_cmp(cmp->tag)) return cmp;
  }
  return nullptr;
}

bool match_add_one(Value *v, PhiInst *iv) {
  auto add = dyn_cast<BinaryInst>(v);
  if (!add || add->tag != Value::Tag::Add) return false;
  if (add->lhs.value == iv) {
    auto rhs = dyn_cast<ConstValue>(add->rhs.value);
    return rhs && rhs->imm == 1;
  }
  if (add->rhs.value == iv) {
    auto lhs = dyn_cast<ConstValue>(add->lhs.value);
    return lhs && lhs->imm == 1;
  }
  return false;
}

bool body_bound(BinaryInst *cmp, bool true_to_body, PhiInst *iv, int &bound) {
  Value::Tag tag = true_to_body ? cmp->tag : invert_cmp(cmp->tag);
  Value *lhs = cmp->lhs.value;
  Value *rhs = cmp->rhs.value;
  if (lhs == iv) {
    auto c = dyn_cast<ConstValue>(rhs);
    if (!c) return false;
    bound = c->imm;
  } else if (rhs == iv) {
    tag = swap_cmp(tag);
    auto c = dyn_cast<ConstValue>(lhs);
    if (!c) return false;
    bound = c->imm;
  } else {
    return false;
  }
  return tag == Value::Tag::Lt;
}

uint64_t full_mask(int elems) {
  return elems == 64 ? ~uint64_t{0} : ((uint64_t{1} << elems) - 1);
}

bool range_initialized(uint64_t state, int first, int last_exclusive) {
  for (int i = first; i < last_exclusive; ++i) {
    if (!(state & (uint64_t{1} << i))) return false;
  }
  return true;
}

bool direct_target_access(AccessInst *access, AllocaInst *target) {
  return access->arr.value == target;
}

bool target_access(AccessInst *access, AllocaInst *target) {
  return root_alloca(access->arr.value) == target;
}

bool target_call_alias(CallInst *call, AllocaInst *target) {
  if (!call->func->has_side_effect) return false;
  for (const Use &arg : call->args) {
    if (root_alloca(arg.value) == target) return true;
  }
  return false;
}

bool mark_store(StoreInst *store, AllocaInst *target, uint64_t &state, int elems) {
  if (!target_access(store, target)) return true;
  if (state == full_mask(elems)) return true;
  if (!direct_target_access(store, target)) return false;
  auto idx = dyn_cast<ConstValue>(store->index.value);
  if (!idx || idx->imm < 0 || idx->imm >= elems) return false;
  state |= uint64_t{1} << idx->imm;
  return true;
}

bool load_is_initialized(LoadInst *load, AllocaInst *target, uint64_t state, int elems) {
  if (!target_access(load, target)) return true;
  if (auto idx = dyn_cast<ConstValue>(load->index.value); idx && direct_target_access(load, target)) {
    return idx->imm >= 0 && idx->imm < elems && (state & (uint64_t{1} << idx->imm));
  }
  return state == full_mask(elems);
}

bool summarize_counted_loop(BasicBlock *header, AllocaInst *target, uint64_t state, int elems, BasicBlock *&exit,
                            uint64_t &out_state) {
  auto br = dyn_cast<BranchInst>(header->insts.tail);
  if (!br || header->pred.size() != 2) return false;

  BasicBlock *body = nullptr;
  bool true_to_body = false;
  auto left_jump = br->left ? dyn_cast<JumpInst>(br->left->insts.tail) : nullptr;
  auto right_jump = br->right ? dyn_cast<JumpInst>(br->right->insts.tail) : nullptr;
  if (left_jump && left_jump->next == header) {
    body = br->left;
    exit = br->right;
    true_to_body = true;
  } else if (right_jump && right_jump->next == header) {
    body = br->right;
    exit = br->left;
    true_to_body = false;
  } else {
    return false;
  }
  if (!body || !exit || body->pred.size() != 1 || body->pred[0] != header) return false;

  int body_idx = pred_index(header, body);
  if (body_idx < 0) return false;
  BasicBlock *preheader = header->pred[1 - body_idx];

  auto cmp = as_compare(br->cond.value);
  if (!cmp) return false;

  PhiInst *iv = nullptr;
  int start = 0;
  int bound = 0;
  for (Inst *inst = header->insts.head; inst; inst = inst->next) {
    auto phi = dyn_cast<PhiInst>(inst);
    if (!phi) break;
    auto init = dyn_cast<ConstValue>(incoming_from(phi, preheader));
    if (init && match_add_one(incoming_from(phi, body), phi) && body_bound(cmp, true_to_body, phi, bound)) {
      iv = phi;
      start = init->imm;
      break;
    }
  }
  if (!iv || start < 0 || bound < start || bound > elems) return false;

  bool saw_target_store = false;
  bool saw_step_store = false;
  bool saw_loop_carried_load = false;
  uint64_t loop_state = state;
  Value *iv_next = incoming_from(iv, body);
  for (Inst *inst = body->insts.head; inst; inst = inst->next) {
    if (inst == body->insts.tail && isa<JumpInst>(inst)) break;
    if (auto load = dyn_cast<LoadInst>(inst)) {
      if (load_is_initialized(load, target, loop_state, elems)) continue;
      if (direct_target_access(load, target) && load->index.value == iv && start < bound &&
          (state & (uint64_t{1} << start))) {
        saw_loop_carried_load = true;
        continue;
      }
      return false;
    } else if (auto store = dyn_cast<StoreInst>(inst)) {
      if (!target_access(store, target)) continue;
      if (!direct_target_access(store, target)) return false;
      if (store->index.value == iv) {
        for (int i = start; i < bound; ++i) loop_state |= uint64_t{1} << i;
      } else if (store->index.value == iv_next && bound < elems) {
        for (int i = start + 1; i <= bound; ++i) loop_state |= uint64_t{1} << i;
        saw_step_store = true;
      } else {
        return false;
      }
      saw_target_store = true;
    } else if (auto call = dyn_cast<CallInst>(inst)) {
      if (target_call_alias(call, target) && loop_state != full_mask(elems)) return false;
    }
  }
  if (saw_loop_carried_load && !saw_step_store && !range_initialized(state, start, bound)) return false;

  out_state = saw_target_store ? loop_state : state;
  return true;
}

bool can_remove(CallInst *memset_call, AllocaInst *target, int elems) {
  std::unordered_map<BasicBlock *, uint64_t> in_state;
  std::queue<BasicBlock *> work;

  auto enqueue = [&](BasicBlock *bb, uint64_t state) {
    auto it = in_state.find(bb);
    if (it == in_state.end()) {
      in_state.emplace(bb, state);
      work.push(bb);
      return;
    }
    uint64_t merged = it->second & state;
    if (merged != it->second) {
      it->second = merged;
      work.push(bb);
    }
  };

  in_state[memset_call->bb] = 0;
  work.push(memset_call->bb);

  while (!work.empty()) {
    BasicBlock *bb = work.front();
    work.pop();
    uint64_t state = in_state[bb];

    Inst *inst = bb == memset_call->bb ? memset_call->next : bb->insts.head;
    for (; inst; inst = inst->next) {
      if (auto load = dyn_cast<LoadInst>(inst)) {
        if (!load_is_initialized(load, target, state, elems)) return false;
      } else if (auto store = dyn_cast<StoreInst>(inst)) {
        if (!mark_store(store, target, state, elems)) return false;
      } else if (auto call = dyn_cast<CallInst>(inst)) {
        if (target_call_alias(call, target) && state != full_mask(elems)) return false;
      }
    }

    if (auto br = dyn_cast<BranchInst>(bb->insts.tail)) {
      BasicBlock *loop_exit = nullptr;
      uint64_t loop_state = state;
      if (summarize_counted_loop(bb, target, state, elems, loop_exit, loop_state)) {
        enqueue(loop_exit, loop_state);
      } else {
        enqueue(br->left, state);
        enqueue(br->right, state);
      }
    } else if (auto jump = dyn_cast<JumpInst>(bb->insts.tail)) {
      enqueue(jump->next, state);
    }
  }

  return true;
}

bool is_full_zero_local_memset(CallInst *call, AllocaInst *&target, int &elems) {
  if (call->func != Func::BUILTIN[8].val || call->args.size() != 3) return false;
  auto fill = dyn_cast<ConstValue>(call->args[1].value);
  auto bytes = dyn_cast<ConstValue>(call->args[2].value);
  target = root_alloca(call->args[0].value);
  if (!fill || !bytes || fill->imm != 0 || !target || target->sym->dims.empty()) return false;
  elems = target->sym->dims[0]->result;
  return elems > 0 && elems <= MAX_TRACKED_ELEMS && bytes->imm == elems * 4;
}

void remove_call(CallInst *call) {
  for (auto [it, end] = call->operands(); it < end; ++it) it->set(nullptr);
  call->bb->insts.remove(call);
  call->deleteValue();
}

}  // namespace

void remove_dead_local_init(IrFunc *f) {
  std::vector<CallInst *> candidates;
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *inst = bb->insts.head; inst; inst = inst->next) {
      if (auto call = dyn_cast<CallInst>(inst)) candidates.push_back(call);
    }
  }

  for (CallInst *call : candidates) {
    AllocaInst *target = nullptr;
    int elems = 0;
    if (is_full_zero_local_memset(call, target, elems) && can_remove(call, target, elems)) {
      dbg("Removing dead local zero initialization");
      remove_call(call);
    }
  }
}

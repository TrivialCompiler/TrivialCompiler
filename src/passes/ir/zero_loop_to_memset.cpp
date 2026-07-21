// Zeroing-loop to memset pass.
//
// Replaces a simple counted loop that stores zero to consecutive elements with a
// builtin memset call.  Example: `for i in 0..n: a[i] = 0` becomes
// `memset(a, 0, n * 4)`.
#include "zero_loop_to_memset.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include "../../structure/ast.hpp"

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

bool defined_in(Value *v, const std::array<BasicBlock *, 2> &blocks) {
  auto inst = dyn_cast<Inst>(v);
  return inst && std::find(blocks.begin(), blocks.end(), inst->bb) != blocks.end();
}

bool all_uses_in(Value *v, const std::array<BasicBlock *, 2> &blocks) {
  for (Use *u = v->uses.head; u; u = u->next) {
    if (std::find(blocks.begin(), blocks.end(), u->user->bb) == blocks.end()) return false;
  }
  return true;
}

bool match_add_one(Value *v, PhiInst *iv) {
  auto add = dyn_cast<BinaryInst>(v);
  if (!add || add->tag != Value::Tag::Add) return false;
  if (add->lhs.value == iv) {
    auto c = dyn_cast<ConstValue>(add->rhs.value);
    return c && c->imm == 1;
  }
  if (add->rhs.value == iv) {
    auto c = dyn_cast<ConstValue>(add->lhs.value);
    return c && c->imm == 1;
  }
  return false;
}

bool body_bound(BinaryInst *cmp, bool true_to_body, PhiInst *iv, Value *&bound) {
  Value::Tag tag = true_to_body ? cmp->tag : invert_cmp(cmp->tag);
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
  return tag == Value::Tag::Lt;
}

bool exit_phis_are_reusable(BasicBlock *exit, BasicBlock *header, const std::array<BasicBlock *, 2> &local_blocks) {
  int idx = pred_index(exit, header);
  if (idx < 0) return false;
  for (Inst *inst = exit->insts.head; inst; inst = inst->next) {
    auto phi = dyn_cast<PhiInst>(inst);
    if (!phi) break;
    if (defined_in(phi->incoming_values[idx].value, local_blocks)) return false;
  }
  return true;
}

// Match a two-block counted loop with one zero store at `arr[iv]` and replace
// its body with one memset call.
bool try_replace(BasicBlock *header) {
  auto header_br = dyn_cast<BranchInst>(header->insts.tail);
  if (!header_br || header->pred.size() != 2) return false;

  BasicBlock *body = nullptr;
  BasicBlock *exit = nullptr;
  bool true_to_body = false;
  if (header_br->left && header_br->right) {
    auto left_jump = dyn_cast_nullable<JumpInst>(header_br->left->insts.tail);
    auto right_jump = dyn_cast_nullable<JumpInst>(header_br->right->insts.tail);
    if (left_jump && left_jump->next == header) {
      body = header_br->left;
      exit = header_br->right;
      true_to_body = true;
    } else if (right_jump && right_jump->next == header) {
      body = header_br->right;
      exit = header_br->left;
      true_to_body = false;
    }
  }
  if (!body || !exit || body->pred.size() != 1 || body->pred[0] != header) return false;

  int body_idx = pred_index(header, body);
  if (body_idx < 0) return false;
  BasicBlock *preheader = header->pred[1 - body_idx];

  auto cmp = as_compare(header_br->cond.value);
  if (!cmp) return false;

  std::array<BasicBlock *, 2> local_blocks{header, body};

  PhiInst *iv = nullptr;
  BinaryInst *step = nullptr;
  for (Inst *inst = header->insts.head; inst; inst = inst->next) {
    auto phi = dyn_cast<PhiInst>(inst);
    if (!phi) break;
    auto init = dyn_cast<ConstValue>(incoming_from(phi, preheader));
    if (init && init->imm == 0 && match_add_one(incoming_from(phi, body), phi)) {
      iv = phi;
      step = dyn_cast<BinaryInst>(incoming_from(phi, body));
      break;
    }
  }
  if (!iv || !step || step->bb != body || !all_uses_in(step, local_blocks)) return false;

  Value *bound = nullptr;
  if (!body_bound(cmp, true_to_body, iv, bound) || defined_in(bound, local_blocks)) return false;
  if (auto c = dyn_cast<ConstValue>(bound); c && (c->imm <= 0 || c->imm > std::numeric_limits<i32>::max() / 4)) {
    return false;
  }

  StoreInst *zero_store = nullptr;
  for (Inst *inst = body->insts.head; inst; inst = inst->next) {
    if (isa<JumpInst>(inst)) {
      if (inst != body->insts.tail) return false;
    } else if (inst == step) {
      continue;
    } else if (auto store = dyn_cast<StoreInst>(inst)) {
      auto data = dyn_cast<ConstValue>(store->data.value);
      if (zero_store || !data || data->imm != 0 || store->index.value != iv || defined_in(store->arr.value, local_blocks)) {
        return false;
      }
      zero_store = store;
    } else {
      return false;
    }
  }
  if (!zero_store || !exit_phis_are_reusable(exit, header, local_blocks)) return false;

  dbg("Replacing zeroing loop with memset");
  Value *arr = zero_store->arr.value;

  for (Inst *inst = header->insts.head; inst; inst = inst->next) {
    auto phi = dyn_cast<PhiInst>(inst);
    if (!phi) break;
    phi->incoming_values.erase(phi->incoming_values.begin() + body_idx);
  }
  header->pred.erase(header->pred.begin() + body_idx);

  int header_exit_idx = pred_index(exit, header);
  assert(header_exit_idx >= 0);
  exit->pred.push_back(body);
  for (Inst *inst = exit->insts.head; inst; inst = inst->next) {
    auto phi = dyn_cast<PhiInst>(inst);
    if (!phi) break;
    phi->incoming_values.emplace_back(phi->incoming_values[header_exit_idx].value, phi);
  }

  std::vector<Inst *> old_insts;
  for (Inst *inst = body->insts.head; inst; inst = inst->next) {
    old_insts.push_back(inst);
    for (auto [it, end] = inst->operands(); it < end; ++it) it->set(nullptr);
  }
  for (Inst *inst : old_insts) {
    body->insts.remove(inst);
    inst->deleteValue();
  }

  Value *bytes = nullptr;
  if (auto c = dyn_cast<ConstValue>(bound)) {
    bytes = ConstValue::get(c->imm * 4);
  } else {
    bytes = new BinaryInst(Value::Tag::Mul, bound, ConstValue::get(4), body);
  }
  auto call = new CallInst(Func::BUILTIN[8].val, body);
  call->args.reserve(3);
  call->args.emplace_back(arr, call);
  call->args.emplace_back(ConstValue::get(0), call);
  call->args.emplace_back(bytes, call);
  new JumpInst(exit, body);
  return true;
}

}  // namespace

void zero_loop_to_memset(IrFunc *f) {
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    try_replace(bb);
  }
}

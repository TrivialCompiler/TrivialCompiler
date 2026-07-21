#include "promote_const_local_array.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../structure/ast.hpp"
#include "bbopt.hpp"

namespace {

constexpr int MAX_PROMOTED_ELEMS = 128;

AllocaInst *root_alloca(Value *v) {
  if (auto x = dyn_cast<AllocaInst>(v)) return x;
  if (auto x = dyn_cast<GetElementPtrInst>(v)) return root_alloca(x->arr.value);
  return nullptr;
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
  if (idx < 0) return nullptr;
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
    auto c = dyn_cast<ConstValue>(add->rhs.value);
    return c && c->imm == 1;
  }
  if (add->rhs.value == iv) {
    auto c = dyn_cast<ConstValue>(add->lhs.value);
    return c && c->imm == 1;
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

bool defined_in(Value *v, const std::unordered_set<BasicBlock *> &blocks) {
  auto inst = dyn_cast<Inst>(v);
  return inst && blocks.find(inst->bb) != blocks.end();
}

bool exit_phis_are_reusable(BasicBlock *exit, BasicBlock *header,
                            const std::unordered_set<BasicBlock *> &local_blocks) {
  int idx = pred_index(exit, header);
  if (idx < 0) return false;
  for (Inst *inst = exit->insts.head; inst; inst = inst->next) {
    auto phi = dyn_cast<PhiInst>(inst);
    if (!phi) break;
    if (defined_in(phi->incoming_values[idx].value, local_blocks)) return false;
  }
  return true;
}

using KnownMap = std::unordered_map<Value *, int>;

std::optional<int> eval_value(Value *v, KnownMap &known) {
  if (auto c = dyn_cast<ConstValue>(v)) return c->imm;
  if (auto it = known.find(v); it != known.end()) return it->second;
  auto bin = dyn_cast<BinaryInst>(v);
  if (!bin) return std::nullopt;
  auto lhs = eval_value(bin->lhs.value, known);
  auto rhs = eval_value(bin->rhs.value, known);
  if (!lhs || !rhs) return std::nullopt;
  switch (bin->tag) {
    case Value::Tag::Add:
      return *lhs + *rhs;
    case Value::Tag::Sub:
      return *lhs - *rhs;
    case Value::Tag::Rsb:
      return *rhs - *lhs;
    case Value::Tag::Mul:
      return *lhs * *rhs;
    case Value::Tag::Div:
      return *rhs == 0 ? std::nullopt : std::optional<int>(*lhs / *rhs);
    case Value::Tag::Mod:
      return *rhs == 0 ? std::nullopt : std::optional<int>(*lhs % *rhs);
    case Value::Tag::Lt:
      return *lhs < *rhs;
    case Value::Tag::Le:
      return *lhs <= *rhs;
    case Value::Tag::Ge:
      return *lhs >= *rhs;
    case Value::Tag::Gt:
      return *lhs > *rhs;
    case Value::Tag::Eq:
      return *lhs == *rhs;
    case Value::Tag::Ne:
      return *lhs != *rhs;
    case Value::Tag::And:
      return *lhs & *rhs;
    case Value::Tag::Or:
      return *lhs | *rhs;
    default:
      return std::nullopt;
  }
}

bool simulate_inst(Inst *inst, AllocaInst *target, std::vector<std::optional<int>> &values, KnownMap &known,
                   std::unordered_set<Inst *> &init_insts, bool allow_other_side_effects) {
  if (auto load = dyn_cast<LoadInst>(inst)) {
    if (!target_access(load, target)) return true;
    auto idx = eval_value(load->index.value, known);
    if (!idx || *idx < 0 || *idx >= static_cast<int>(values.size()) || !values[*idx]) return false;
    known[load] = *values[*idx];
    init_insts.insert(load);
    return true;
  }
  if (auto store = dyn_cast<StoreInst>(inst)) {
    if (!target_access(store, target)) return allow_other_side_effects;
    auto idx = eval_value(store->index.value, known);
    auto data = eval_value(store->data.value, known);
    if (!idx || !data || *idx < 0 || *idx >= static_cast<int>(values.size())) return false;
    values[*idx] = *data;
    init_insts.insert(store);
    return true;
  }
  if (auto call = dyn_cast<CallInst>(inst)) {
    if (target_call_alias(call, target)) return false;
    return allow_other_side_effects || !call->has_side_effect();
  }
  if (auto bin = dyn_cast<BinaryInst>(inst)) {
    auto value = eval_value(bin, known);
    if (value) known[bin] = *value;
    return true;
  }
  if (isa<GetElementPtrInst>(inst) || isa<PhiInst>(inst) || isa<JumpInst>(inst) || isa<BranchInst>(inst)) {
    return true;
  }
  return allow_other_side_effects || !inst->has_side_effect();
}

bool summarize_init_loop(BasicBlock *header, AllocaInst *target, std::vector<std::optional<int>> &values,
                         BasicBlock *&preheader, BasicBlock *&body, BasicBlock *&exit,
                         std::unordered_set<Inst *> &init_insts) {
  auto br = dyn_cast<BranchInst>(header->insts.tail);
  if (!br || header->pred.size() != 2) return false;

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
  preheader = header->pred[1 - body_idx];

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
  if (!iv || start < 0 || bound < start || bound > static_cast<int>(values.size())) return false;

  std::unordered_set<BasicBlock *> local_blocks{header, body};
  if (!exit_phis_are_reusable(exit, header, local_blocks)) return false;

  for (int iter = start; iter < bound; ++iter) {
    KnownMap known;
    known[iv] = iter;
    for (Inst *inst = body->insts.head; inst; inst = inst->next) {
      if (isa<JumpInst>(inst)) break;
      if (!simulate_inst(inst, target, values, known, init_insts, false)) return false;
    }
  }
  return true;
}

void remove_inst(Inst *inst) {
  for (auto [it, end] = inst->operands(); it < end; ++it) it->set(nullptr);
  inst->bb->insts.remove(inst);
  inst->deleteValue();
}

bool all_later_writes_are_init(IrFunc *f, AllocaInst *target, const std::unordered_set<Inst *> &init_insts) {
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *inst = bb->insts.head; inst; inst = inst->next) {
      if (auto store = dyn_cast<StoreInst>(inst)) {
        if (target_access(store, target) && init_insts.find(store) == init_insts.end()) return false;
      } else if (auto call = dyn_cast<CallInst>(inst)) {
        if (target_call_alias(call, target)) return false;
      }
    }
  }
  return true;
}

Decl *make_global_array(IrProgram *p, IrFunc *f, AllocaInst *alloc, const std::vector<std::optional<int>> &values,
                        size_t id) {
  std::vector<Expr *> init;
  init.reserve(values.size());
  for (auto value : values) {
    if (!value) return nullptr;
    init.push_back(*value == 0 ? &IntConst::ZERO : new IntConst{Expr::Tag::IntConst, *value});
  }

  auto name = new std::string("__promoted_" + std::string(f->func->name) + "_" + std::string(alloc->sym->name) + "_" +
                              std::to_string(id));
  auto promoted =
      new Decl{true, true, true, {name->c_str(), name->length()}, alloc->sym->dims, {nullptr}, init};
  promoted->value = new GlobalRef(promoted);
  p->glob_decl.push_back(promoted);
  return promoted;
}

bool try_promote(IrProgram *p, IrFunc *f, AllocaInst *alloc, size_t id) {
  if (!alloc->sym || alloc->sym->dims.empty() || !alloc->sym->dims[0]) return false;
  int size = alloc->sym->dims[0]->result;
  if (size <= 0 || size > MAX_PROMOTED_ELEMS) return false;

  std::vector<std::optional<int>> values(size);
  KnownMap known;
  std::unordered_set<Inst *> init_insts;

  BasicBlock *preheader = alloc->bb;
  BasicBlock *header = nullptr;
  BasicBlock *body = nullptr;
  BasicBlock *exit = nullptr;

  for (Inst *inst = alloc->next; inst; inst = inst->next) {
    if (auto jump = dyn_cast<JumpInst>(inst)) {
      header = jump->next;
      break;
    }
    if (isa<BranchInst>(inst) || isa<ReturnInst>(inst)) return false;
    if (!simulate_inst(inst, alloc, values, known, init_insts, true)) return false;
  }
  if (!header) return false;

  BasicBlock *loop_preheader = nullptr;
  if (!summarize_init_loop(header, alloc, values, loop_preheader, body, exit, init_insts)) return false;
  if (loop_preheader != preheader) return false;
  if (!all_later_writes_are_init(f, alloc, init_insts)) return false;

  auto old_jump = dyn_cast<JumpInst>(preheader->insts.tail);
  if (!old_jump || old_jump->next != header) return false;

  Decl *global = make_global_array(p, f, alloc, values, id);
  if (!global) return false;

  dbg("Promoting local constant array");

  for (Inst *inst : init_insts) {
    if (inst->bb == preheader) remove_inst(inst);
  }

  remove_inst(old_jump);
  new JumpInst(exit, preheader);

  int preheader_idx = pred_index(header, preheader);
  if (preheader_idx >= 0) {
    header->pred.erase(header->pred.begin() + preheader_idx);
    for (Inst *inst = header->insts.head; inst; inst = inst->next) {
      auto phi = dyn_cast<PhiInst>(inst);
      if (!phi) break;
      phi->incoming_values.erase(phi->incoming_values.begin() + preheader_idx);
    }
  }

  int header_exit_idx = pred_index(exit, header);
  if (header_exit_idx >= 0) {
    exit->pred.push_back(preheader);
    for (Inst *inst = exit->insts.head; inst; inst = inst->next) {
      auto phi = dyn_cast<PhiInst>(inst);
      if (!phi) break;
      phi->incoming_values.emplace_back(phi->incoming_values[header_exit_idx].value, phi);
    }
  }

  alloc->replaceAllUseWith(global->value);
  if (!alloc->uses.head) remove_inst(alloc);
  bbopt(f);
  return true;
}

}  // namespace

void promote_const_local_array(IrProgram *p) {
  size_t id = 0;
  for (IrFunc *f = p->func.head; f; f = f->next) {
    if (f->builtin) continue;
    std::vector<AllocaInst *> allocas;
    for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
      for (Inst *inst = bb->insts.head; inst; inst = inst->next) {
        if (auto alloc = dyn_cast<AllocaInst>(inst)) allocas.push_back(alloc);
      }
    }
    for (AllocaInst *alloc : allocas) {
      if (try_promote(p, f, alloc, id)) ++id;
    }
  }
}

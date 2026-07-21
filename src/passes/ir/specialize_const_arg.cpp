#include "specialize_const_arg.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../structure/ast.hpp"
#include "../../structure/op.hpp"
#include "cfg.hpp"

namespace {

constexpr int MAX_SPECIALIZATIONS = 12;

struct SpecializationKey {
  IrFunc *base;
  std::vector<std::pair<int, i32>> const_args;

  bool operator==(const SpecializationKey &rhs) const {
    return base == rhs.base && const_args == rhs.const_args;
  }
};

struct SpecializationKeyHash {
  size_t operator()(const SpecializationKey &key) const {
    size_t h = reinterpret_cast<size_t>(key.base) >> 4;
    for (auto [idx, imm] : key.const_args) {
      h ^= static_cast<size_t>(idx + 0x9e3779b9) + (h << 6) + (h >> 2);
      h ^= static_cast<size_t>(imm) + (h << 6) + (h >> 2);
    }
    return h;
  }
};

bool can_fold(Value::Tag tag, i32 lhs, i32 rhs) {
  (void) lhs;
  return !((tag == Value::Tag::Div || tag == Value::Tag::Mod) && rhs == 0);
}

bool try_fold_binary(BinaryInst *x, Value *lhs, Value *rhs, Value *&out) {
  auto l = dyn_cast<ConstValue>(lhs);
  auto r = dyn_cast<ConstValue>(rhs);
  if (!l || !r || !can_fold(x->tag, l->imm, r->imm)) return false;
  out = ConstValue::get(op::eval(static_cast<op::Op>(x->tag), l->imm, r->imm));
  return true;
}

bool param_index(Func *func, Decl *decl, int &idx) {
  if (func->params.empty()) return false;
  Decl *begin = func->params.data();
  Decl *end = begin + func->params.size();
  if (decl < begin || decl >= end) return false;
  idx = static_cast<int>(decl - begin);
  return true;
}

bool is_param_ref(Value *v, Func *func, int idx) {
  auto p = dyn_cast<ParamRef>(v);
  return p && p->decl == &func->params[idx];
}

bool is_positive_decrement(Value *v, Func *func, int idx) {
  auto x = dyn_cast<BinaryInst>(v);
  if (!x) return false;
  auto rhs = dyn_cast<ConstValue>(x->rhs.value);
  if (!rhs || !is_param_ref(x->lhs.value, func, idx)) return false;
  return (x->tag == Value::Tag::Sub && rhs->imm > 0) || (x->tag == Value::Tag::Add && rhs->imm < 0);
}

bool supports_const_arg_specialization(IrFunc *callee, int idx) {
  for (BasicBlock *bb = callee->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i; i = i->next) {
      auto call = dyn_cast<CallInst>(i);
      if (call && call->func == callee && is_positive_decrement(call->args[idx].value, callee->func, idx)) return true;
    }
  }
  return false;
}

IrFunc *clone_for_const_args(IrFunc *src, const SpecializationKey &key, int ordinal) {
  auto cloned_name = std::make_unique<std::string>(std::string(src->func->name) + ".const." + std::to_string(ordinal));
  auto cloned_func = new Func(*src->func);
  cloned_func->name = *cloned_name;

  auto dst = new IrFunc;
  dst->func = cloned_func;
  cloned_func->val = dst;
  dst->builtin = false;
  dst->load_global = src->load_global;
  dst->has_side_effect = src->has_side_effect;
  dst->can_inline = false;

  // Keep cloned function and local names alive for the rest of compilation.
  static std::vector<std::unique_ptr<std::string>> names;
  names.push_back(std::move(cloned_name));

  std::unordered_map<BasicBlock *, BasicBlock *> bb_map;
  std::unordered_map<Value *, Value *> val_map;
  std::unordered_map<Decl *, Decl *> sym_map;

  std::vector<ConstValue *> const_params(src->func->params.size(), nullptr);
  for (auto [idx, imm] : key.const_args) {
    const_params[idx] = ConstValue::get(imm);
  }

  auto get_sym = [&](Decl *sym) -> Decl * {
    if (auto it = sym_map.find(sym); it != sym_map.end()) return it->second;
    int idx = -1;
    if (param_index(src->func, sym, idx)) {
      Decl *mapped = &cloned_func->params[idx];
      sym_map.insert({sym, mapped});
      return mapped;
    }
    return sym;
  };

  auto get = [&](Value *v) -> Value * {
    if (auto it = val_map.find(v); it != val_map.end()) return it->second;
    if (auto x = dyn_cast<ParamRef>(v)) {
      int idx = -1;
      if (param_index(src->func, x->decl, idx)) {
        if (const_params[idx]) return const_params[idx];
        auto mapped = new ParamRef(&cloned_func->params[idx]);
        val_map.insert({v, mapped});
        return mapped;
      }
    }
    assert(!isa<Inst>(v));
    return v;
  };

  for (BasicBlock *bb = src->bb.head; bb; bb = bb->next) {
    auto cloned = new BasicBlock;
    bb_map.insert({bb, cloned});
    dst->bb.insertAtEnd(cloned);
  }

  for (BasicBlock *bb : compute_rpo(src)) {
    BasicBlock *cloned = bb_map.find(bb)->second;
    for (BasicBlock *p : bb->pred) {
      cloned->pred.push_back(bb_map.find(p)->second);
    }

    Inst *i = bb->insts.head;
    for (;; i = i->next) {
      if (auto x = dyn_cast<PhiInst>(i)) {
        val_map.insert({x, new PhiInst(cloned)});
      } else {
        break;
      }
    }

    for (; i; i = i->next) {
      Inst *res = nullptr;
      if (auto x = dyn_cast<BinaryInst>(i)) {
        Value *lhs = get(x->lhs.value);
        Value *rhs = get(x->rhs.value);
        Value *folded = nullptr;
        if (try_fold_binary(x, lhs, rhs, folded)) {
          val_map.insert({i, folded});
          continue;
        }
        res = new BinaryInst(x->tag, lhs, rhs, cloned);
      } else if (auto x = dyn_cast<BranchInst>(i)) {
        res = new BranchInst(get(x->cond.value), bb_map.find(x->left)->second, bb_map.find(x->right)->second, cloned);
      } else if (auto x = dyn_cast<JumpInst>(i)) {
        res = new JumpInst(bb_map.find(x->next)->second, cloned);
      } else if (auto x = dyn_cast<ReturnInst>(i)) {
        res = new ReturnInst(x->ret.value ? get(x->ret.value) : nullptr, cloned);
      } else if (auto x = dyn_cast<GetElementPtrInst>(i)) {
        res = new GetElementPtrInst(get_sym(x->lhs_sym), get(x->arr.value), get(x->index.value), x->multiplier, cloned);
      } else if (auto x = dyn_cast<LoadInst>(i)) {
        res = new LoadInst(get_sym(x->lhs_sym), get(x->arr.value), get(x->index.value), cloned);
      } else if (auto x = dyn_cast<StoreInst>(i)) {
        res = new StoreInst(get_sym(x->lhs_sym), get(x->arr.value), get(x->data.value), get(x->index.value), cloned);
      } else if (auto x = dyn_cast<CallInst>(i)) {
        auto call = new CallInst(x->func, cloned);
        call->args.reserve(x->args.size());
        for (const Use &u : x->args) call->args.emplace_back(get(u.value), call);
        res = call;
      } else if (auto x = dyn_cast<AllocaInst>(i)) {
        auto sym = new Decl(*x->sym);
        res = new AllocaInst(sym, cloned);
        sym->value = res;
        sym_map.insert({x->sym, sym});
      } else {
        UNREACHABLE();
      }
      val_map.insert({i, res});
    }
  }

  for (BasicBlock *bb = src->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head;; i = i->next) {
      if (auto x = dyn_cast<PhiInst>(i)) {
        auto cloned = static_cast<PhiInst *>(val_map.find(x)->second);
        for (u32 j = 0, sz = x->incoming_values.size(); j < sz; ++j) {
          cloned->incoming_values[j].set(get(x->incoming_values[j].value));
        }
      } else {
        break;
      }
    }
  }

  return dst;
}

bool build_key(CallInst *call, SpecializationKey &key) {
  IrFunc *callee = call->func;
  if (callee->builtin || callee->func->name == "main") return false;
  if (callee->func->params.size() != call->args.size()) return false;

  key.base = callee;
  key.const_args.clear();
  for (u32 i = 0, sz = call->args.size(); i < sz; ++i) {
    if (callee->func->params[i].is_param_array() || !supports_const_arg_specialization(callee, i)) continue;
    // Include the common `n - 1` recursion sentinel without specializing an unbounded negative chain.
    if (auto arg = dyn_cast<ConstValue>(call->args[i].value); arg && arg->imm >= -1) {
      key.const_args.push_back({static_cast<int>(i), arg->imm});
      break;
    }
  }
  return !key.const_args.empty();
}

std::vector<CallInst *> collect_calls(IrProgram *p) {
  std::vector<CallInst *> calls;
  for (IrFunc *f = p->func.head; f; f = f->next) {
    if (f->builtin) continue;
    for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
      for (Inst *i = bb->insts.head; i; i = i->next) {
        if (auto call = dyn_cast<CallInst>(i)) calls.push_back(call);
      }
    }
  }
  return calls;
}

}  // namespace

void specialize_const_arg(IrProgram *p) {
  std::unordered_map<SpecializationKey, IrFunc *, SpecializationKeyHash> specializations;
  std::unordered_set<IrFunc *> cloned_funcs;
  int created = 0;
  bool changed = true;

  while (changed && created < MAX_SPECIALIZATIONS) {
    changed = false;
    for (CallInst *call : collect_calls(p)) {
      if (cloned_funcs.count(call->func)) continue;
      SpecializationKey key;
      if (!build_key(call, key)) continue;

      IrFunc *target = nullptr;
      auto it = specializations.find(key);
      if (it != specializations.end()) {
        target = it->second;
      } else {
        if (created >= MAX_SPECIALIZATIONS) break;
        target = clone_for_const_args(key.base, key, created);
        p->func.insertAtEnd(target);
        specializations.insert({key, target});
        cloned_funcs.insert(target);
        ++created;
        changed = true;
      }
      call->func = target;
    }
  }
}

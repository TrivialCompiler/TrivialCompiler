// Local initialization sinking pass.
//
// Moves entry-block zero memsets for local arrays past cheap guard blocks when
// one branch returns early.  Example: `memset(t,0); if (bad) return; ...` becomes
// `if (bad) return; memset(t,0); ...`.
#include "sink_local_init.hpp"

#include <unordered_set>
#include <vector>

#include "../../structure/ast.hpp"

static AllocaInst *root_alloca(Value *v) {
  if (auto x = dyn_cast<AllocaInst>(v)) return x;
  if (auto x = dyn_cast<GetElementPtrInst>(v)) return root_alloca(x->arr.value);
  return nullptr;
}

static bool is_zero_local_memset(CallInst *call) {
  if (call->func != Func::BUILTIN[8].val || call->args.size() != 3) return false;
  auto fill = dyn_cast<ConstValue>(call->args[1].value);
  auto count = dyn_cast<ConstValue>(call->args[2].value);
  return fill && count && fill->imm == 0 && count->imm >= 0 && root_alloca(call->args[0].value);
}

static bool is_direct_return(BasicBlock *bb) {
  return bb && bb->insts.head && bb->insts.head == bb->insts.tail && isa<ReturnInst>(bb->insts.head);
}

static bool is_guard_block(BasicBlock *bb) {
  if (!bb) return false;
  for (auto inst = bb->insts.head; inst; inst = inst->next) {
    if (!(isa<PhiInst>(inst) || isa<BinaryInst>(inst) || isa<BranchInst>(inst) || isa<JumpInst>(inst))) {
      return false;
    }
  }
  return true;
}

struct EarlyReturnEdge {
  BranchInst *branch;
  BasicBlock *cont;
  bool cont_is_left;
};

static EarlyReturnEdge find_early_return_edge(BasicBlock *entry, std::unordered_set<BasicBlock *> &guards) {
  std::vector<BasicBlock *> work;
  if (auto br = dyn_cast<BranchInst>(entry->insts.tail)) {
    work.push_back(br->left);
    work.push_back(br->right);
  } else if (auto jump = dyn_cast<JumpInst>(entry->insts.tail)) {
    work.push_back(jump->next);
  } else {
    return {};
  }

  while (!work.empty()) {
    auto bb = work.back();
    work.pop_back();
    if (guards.count(bb)) continue;
    if (!is_guard_block(bb)) continue;
    guards.insert(bb);

    if (auto br = dyn_cast<BranchInst>(bb->insts.tail)) {
      bool left_returns = is_direct_return(br->left);
      bool right_returns = is_direct_return(br->right);
      if (left_returns != right_returns) {
        return {br, left_returns ? br->right : br->left, !left_returns};
      }
      if (!left_returns) work.push_back(br->left);
      if (!right_returns) work.push_back(br->right);
    } else if (auto jump = dyn_cast<JumpInst>(bb->insts.tail)) {
      work.push_back(jump->next);
    }
  }

  return {};
}

static BasicBlock *split_edge(IrFunc *f, EarlyReturnEdge edge) {
  auto split = new BasicBlock;
  f->bb.insertBefore(split, edge.cont);

  if (edge.cont_is_left) {
    edge.branch->left = split;
  } else {
    edge.branch->right = split;
  }

  split->pred.push_back(edge.branch->bb);
  for (auto &pred : edge.cont->pred) {
    if (pred == edge.branch->bb) {
      pred = split;
      break;
    }
  }
  new JumpInst(edge.cont, split);
  return split;
}

void sink_local_init(IrFunc *f) {
  auto entry = f->bb.head;
  if (!entry) return;

  std::vector<CallInst *> memsets;
  for (auto inst = entry->insts.head; inst; inst = inst->next) {
    if (auto call = dyn_cast<CallInst>(inst); call && is_zero_local_memset(call)) {
      memsets.push_back(call);
    }
  }
  if (memsets.empty()) return;

  for (auto inst = entry->insts.head; inst; inst = inst->next) {
    if (inst == entry->insts.tail) break;
    if (isa<LoadInst>(inst) || isa<StoreInst>(inst)) return;
    if (auto call = dyn_cast<CallInst>(inst); call && !is_zero_local_memset(call)) return;
  }

  std::unordered_set<BasicBlock *> guards;
  auto edge = find_early_return_edge(entry, guards);
  if (!edge.branch) return;
  auto split = split_edge(f, edge);
  auto insert_before = split->insts.tail;

  for (auto call : memsets) {
    call->bb->insts.remove(call);
    call->bb = split;
    split->insts.insertBefore(call, insert_before);
  }
}

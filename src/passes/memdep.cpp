#include "memdep.hpp"
#include "../ast.hpp"

// 如果一个是另一个的postfix，则可能alias；nullptr相当于通配符
static bool dim_alias(const std::vector<Expr *> &dim1, const std::vector<Expr *> &dim2) {
  auto pred = [](Expr *l, Expr *r) { return !l || !r || l->result == r->result; };
  return dim1.size() < dim2.size() ?
    std::equal(dim1.begin(), dim1.end(), dim2.end() - dim1.size(), pred) :
    std::equal(dim2.begin(), dim2.end(), dim1.end() - dim2.size(), pred);
}

// 目前只考虑用数组的类型/维度来排除alias，不考虑用下标来排除
// arr1, arr2只可能是ParamRef, GlobalRef, AllocaInst
// todo: 如果以后支持把a[b][c]转化成t = a[b], t[c]的话，arr还可能是LoadInst
// 这个关系是对称的
static bool alias(Value *arr1, Value *arr2) {
  if (auto x = dyn_cast<ParamRef>(arr1)) {
    if (auto y = dyn_cast<ParamRef>(arr2))
      return dim_alias(x->decl->dims, y->decl->dims);
    else if (auto y = dyn_cast<GlobalRef>(arr2))
      return dim_alias(x->decl->dims, y->decl->dims);
    else if (auto y = dyn_cast<AllocaInst>(arr2))
      return false;
    else
      UNREACHABLE();
  } else if (auto x = dyn_cast<GlobalRef>(arr1)) {
    if (auto y = dyn_cast<ParamRef>(arr2))
      return dim_alias(x->decl->dims, y->decl->dims);
    else if (auto y = dyn_cast<GlobalRef>(arr2))
      return x->decl == y->decl;
    else if (auto y = dyn_cast<AllocaInst>(arr2))
      return false;
    else
      UNREACHABLE();
  } else if (auto x = dyn_cast<AllocaInst>(arr1)) {
    if (auto y = dyn_cast<ParamRef>(arr2))
      return false;
    else if (auto y = dyn_cast<GlobalRef>(arr2))
      return false;
    else if (auto y = dyn_cast<AllocaInst>(arr2))
      return x == y; // 与x->sym == y->sym的值应该是一致的
    else
      UNREACHABLE();
  } else {
    UNREACHABLE();
  }
}

static void dfs_access(AccessInst *src, Inst *i) {
  BasicBlock *bb = i->bb;
  if (bb->vis) return;
  bb->vis = true;
  Value *arr = src->arr.value;
  bool is_load = isa<LoadInst>(src);
  bool is_glob = isa<GlobalRef>(arr);
  for (; i; i = i->next) {
    if (auto x = dyn_cast<AccessInst>(i)) {
      // load不可能依赖于load，不管是否alias
      if ((!is_load || !isa<LoadInst>(x)) && alias(arr, x->arr.value))
        x->dep.emplace_back(src, i);
    } else if (auto x = dyn_cast<CallInst>(i)) {
      // call依赖于全局的load和store，或者call的参数与load和store的地址alias
      if (is_glob || std::any_of(x->args.begin(), x->args.end(), [arr](Use &u) {
        auto a = dyn_cast<LoadInst>(u.value);
        return a && alias(arr, a->arr.value);
      }))
        x->dep.emplace_back(src, i);
    }
  }
  for (BasicBlock *x : bb->succ()) {
    if (x) dfs_access(src, x->insts.head);
  }
}

static void dfs_call(CallInst *src, Inst *i) {
  BasicBlock *bb = i->bb;
  if (bb->vis) return;
  bb->vis = true;
  for (; i; i = i->next) {
    if (auto x = dyn_cast<AccessInst>(i)) {
      Value *arr = x->arr.value;
      // 全局的load和store依赖于call，或者load和store的地址与call的参数alias
      if (isa<GlobalRef>(arr) || std::any_of(src->args.begin(), src->args.end(), [arr](Use &u) {
        auto a = dyn_cast<LoadInst>(u.value);
        return a && alias(arr, a->arr.value);
      }))
        x->dep.emplace_back(src, i);
    } else if (auto x = dyn_cast<CallInst>(i)) {
      // 保守地假设任何call之间的顺序都不能调换，所以call一定依赖于前面的call，不用看参数
      x->dep.emplace_back(src, i);
    }
  }
  for (BasicBlock *x : bb->succ()) {
    if (x) dfs_call(src, x->insts.head);
  }
}

void compute_memdep(IrFunc *f) {
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i; i = i->next) {
      if (auto x = dyn_cast<AccessInst>(i))
        x->dep.clear();
      else if (auto x = dyn_cast<CallInst>(i))
        x->dep.clear();
    }
  }
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i;) {
      Inst *next = i->next;
      if (auto x = dyn_cast<AccessInst>(i)) {
        if (x->dims.size() == x->lhs_sym->dims.size()) // 这对StoreInst是必须成立的
          f->clear_all_vis(), dfs_access(x, next);
      } else if (auto x = dyn_cast<CallInst>(i)) {
        f->clear_all_vis(), dfs_call(x, next);
      }
      i = next;
    }
  }
}
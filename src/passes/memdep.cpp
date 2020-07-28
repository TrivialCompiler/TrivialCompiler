#include "memdep.hpp"
#include "cfg.hpp"
#include "../ast.hpp"
#include <unordered_map>

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
// 这个关系是对称的，但不是传递的，例如参数中的int []和int [][5]，int [][10]都alias，但int [][5]和int [][10]不alias
// todo: 测试发现公开测例中可以直接假定不同的参数不alias，也许还可以进一步面向测例编程
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

// 如果load的数组不是本函数内定义的，一个函数调用就可能修改其内容，这包括ParamRef和GlobalRef
// 如果load的数组是本函数内定义的，即是AllocaInst，则只有当其地址被不完全load作为参数传递给一个函数时，这个函数才可能修改它
static bool is_call_load_alias(Value *arr, CallInst *y) {
  return !isa<AllocaInst>(arr) || std::any_of(y->args.begin(), y->args.end(), [arr](Use &u) {
    auto a = dyn_cast<LoadInst>(u.value);
    return a && a->lhs_sym->dims.size() < a->dims.size() && alias(arr, a->arr.value);
  });
}

struct LoadInfo {
  u32 id;
  std::vector<LoadInst *> loads;
  std::unordered_set<Inst *> stores;
};

// 构造load对store，store对load的依赖关系，分成两趟分别计算
void compute_memdep(IrFunc *f) {
  // 清空原来的结果
  // todo: 我现在的清空方式是正确的吗？真的能够运行第二次吗？
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->mem_phis.head; i; i = i->next)
      delete static_cast<MemPhiInst *>(i);
    bb->mem_phis.head = bb->mem_phis.tail = nullptr;
    for (Inst *i = bb->insts.head; i;) {
      Inst *next = i->next;
      if (auto x = dyn_cast<MemOpInst>(i)) {
        bb->insts.remove(x);
        delete x;
      }
      i = next;
    }
  }
  // 把所有数组地址相同的load一起考虑，因为相关的store集合计算出来必定是一样的
  std::unordered_map<Value *, LoadInfo> loads;
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i; i = i->next) {
      if (auto x = dyn_cast<LoadInst>(i); x && x->lhs_sym->dims.size() == x->dims.size()) {
        x->mem_token.set(nullptr);
        Value *arr = x->arr.value;
        auto[it, inserted] = loads.insert({arr, {(u32) loads.size()}});
        LoadInfo &info = it->second;
        info.loads.push_back(x);
        if (!inserted) continue; // stores已经计算过了
        for (BasicBlock *bb1 = f->bb.head; bb1; bb1 = bb1->next) {
          for (Inst *i1 = bb1->insts.head; i1; i1 = i1->next) {
            if ((isa<StoreInst>(i1) && alias(arr, static_cast<StoreInst *>(i1)->arr.value)) ||
                (isa<CallInst>(i1) && is_call_load_alias(arr, static_cast<CallInst *>(i1)))) {
              info.stores.insert(i1);
            }
          }
        }
      }
    }
  }
  auto df = compute_df(f);
  // 第一趟，构造load对store的依赖关系
  {
    std::vector<BasicBlock *> worklist;
    for (auto &[arr, info] : loads) {
      f->clear_all_vis();
      for (Inst *i : info.stores) worklist.push_back(i->bb);
      while (!worklist.empty()) {
        BasicBlock *x = worklist.back();
        worklist.pop_back();
        for (BasicBlock *y : df[x]) {
          if (!y->vis) {
            y->vis = true;
            new MemPhiInst(arr, y);
            worklist.push_back(y);
          }
        }
      }
    }
    std::vector<std::pair<BasicBlock *, std::vector<Value *>>> worklist2{{f->bb.head, std::vector<Value *>(loads.size(), &UndefValue::INSTANCE)}};
    f->clear_all_vis();
    while (!worklist2.empty()) {
      BasicBlock *bb = worklist2.back().first;
      std::vector<Value *> values = std::move(worklist2.back().second);
      worklist2.pop_back();
      if (!bb->vis) {
        bb->vis = true;
        for (Inst *i = bb->mem_phis.head; i; i = i->next) {
          auto i1 = static_cast<MemPhiInst *>(i);
          values[loads.find(i1->load_or_arr)->second.id] = i;
        }
        for (Inst *i = bb->insts.head; i; i = i->next) {
          if (auto x = dyn_cast<LoadInst>(i); x && x->lhs_sym->dims.size() == x->dims.size()) {
            x->mem_token.set(values[loads.find(x->arr.value)->second.id]);
          } else if (isa<StoreInst>(i) || isa<CallInst>(i)) {
            for (auto &[load, info] : loads) {
              if (info.stores.find(i) != info.stores.end()) {
                values[info.id] = i;
              }
            }
          }
        }
        for (BasicBlock *x : bb->succ()) {
          if (x) {
            worklist2.emplace_back(x, values);
            for (Inst *i = x->mem_phis.head; i; i = i->next) {
              auto i1 = static_cast<MemPhiInst *>(i);
              u32 id = loads.find(i1->load_or_arr)->second.id;
              u32 idx = std::find(x->pred.begin(), x->pred.end(), bb) - x->pred.begin();
              i1->incoming_values[idx].set(values[id]);
            }
          }
        }
      }
    }
  }
  // 第二趟，构造store对load的依赖关系，虽然store也依赖store，但是后面不会调整store的位置，所以没有必要考虑这个依赖关系
  // 与第一趟不同，这里不能把一个地址的load放在一起考虑，比如连续两个load，如果一起考虑的话就会认为前一个load不被任何store依赖
  std::unordered_map<LoadInst *, u32> loads2;
  for (auto &[arr, info] : loads) {
    for (LoadInst *load : info.loads) {
      loads2.insert({load, (u32) loads2.size()});
      for (Inst *store : info.stores) {
        new MemOpInst(load, store);
      }
    }
  }
  {
    std::vector<BasicBlock *> worklist;
    for (auto &[load, id] : loads2) {
      f->clear_all_vis();
      worklist.push_back(load->bb);
      while (!worklist.empty()) {
        BasicBlock *x = worklist.back();
        worklist.pop_back();
        for (BasicBlock *y : df[x]) {
          if (!y->vis) {
            y->vis = true;
            new MemPhiInst(load, y);
            worklist.push_back(y);
          }
        }
      }
    }
    std::vector<std::pair<BasicBlock *, std::vector<Value *>>> worklist2{{f->bb.head, std::vector<Value *>(loads2.size(), &UndefValue::INSTANCE)}};
    f->clear_all_vis();
    while (!worklist2.empty()) {
      BasicBlock *bb = worklist2.back().first;
      std::vector<Value *> values = std::move(worklist2.back().second);
      worklist2.pop_back();
      if (!bb->vis) {
        bb->vis = true;
        for (Inst *i = bb->mem_phis.head; i; i = i->next) {
          auto i1 = static_cast<MemPhiInst *>(i);
          // 不考虑第一趟引入的MemPhiInst
          if (auto it = loads2.find(static_cast<LoadInst *>(i1->load_or_arr)); it != loads2.end()) {
            values[it->second] = i;
          }
        }
        for (Inst *i = bb->insts.head; i; i = i->next) {
          if (auto x = dyn_cast<LoadInst>(i); x && x->lhs_sym->dims.size() == x->dims.size()) {
            values[loads2.find(x)->second] = x;
          } else if (auto x = dyn_cast<MemOpInst>(i)) {
            x->mem_token.set(values[loads2.find(x->load)->second]);
          }
        }
        for (BasicBlock *x : bb->succ()) {
          if (x) {
            worklist2.emplace_back(x, values);
            for (Inst *i = x->mem_phis.head; i; i = i->next) {
              auto i1 = static_cast<MemPhiInst *>(i);
              if (auto it = loads2.find(static_cast<LoadInst *>(i1->load_or_arr)); it != loads2.end()) {
                u32 idx = std::find(x->pred.begin(), x->pred.end(), bb) - x->pred.begin();
                i1->incoming_values[idx].set(values[it->second]);
              }
            }
          }
        }
      }
    }
  }
  // 删除无用的MemPhi，避免不必要的依赖
  while (true) {
    bool changed = false;
    for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
      for (Inst *i = bb->mem_phis.head; i;) {
        Inst *next = i->next;
        if (i->uses.head == nullptr) {
          bb->mem_phis.remove(i);
          delete static_cast<MemPhiInst *>(i);
          changed = true;
        }
        i = next;
      }
    }
    if (!changed) break;
  }
}
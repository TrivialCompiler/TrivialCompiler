#include "memdep.hpp"

#include <unordered_map>

#include "../../ast.hpp"
#include "cfg.hpp"

// 如果一个是另一个的postfix，则可能alias；nullptr相当于通配符
static bool dim_alias(const std::vector<Expr *> &dim1, const std::vector<Expr *> &dim2) {
  auto pred = [](Expr *l, Expr *r) { return !l || !r || l->result == r->result; };
  return dim1.size() < dim2.size() ?
    std::equal(dim1.begin(), dim1.end(), dim2.end() - dim1.size(), pred) :
    std::equal(dim2.begin(), dim2.end(), dim1.end() - dim2.size(), pred);
}

// 目前只考虑用数组的类型/维度来排除alias，不考虑用下标来排除
// 分三种情况：!dims.empty() && dims[0] == nullptr => 参数数组; 否则is_glob == true => 全局变量; 否则是局部数组
// 这个关系是对称的，但不是传递的，例如参数中的int []和int [][5]，int [][10]都alias，但int [][5]和int [][10]不alias
static bool alias(Decl *arr1, Decl *arr2) {
  if (arr1->is_param_array()) { // 参数
    if (arr2->is_param_array())
      // NOTE_OPT: this assumes that any two arrays in parameters do not alias
      return arr1->name == arr2->name;
    else if (arr2->is_glob)
      return dim_alias(arr1->dims, arr2->dims);
    else
      return false;
  } else if (arr1->is_glob) { // 全局变量
    if (arr2->is_param_array())
      return dim_alias(arr1->dims, arr2->dims);
    else if (arr2->is_glob)
      return arr1 == arr2;
    else
      return false;
  } else { // 局部数组
    if (arr2->is_param_array())
      return false;
    else if (arr2->is_glob)
      return false;
    else
      return arr1 == arr2;
  }
}

// 如果load的数组不是本函数内定义的，一个函数调用就可能修改其内容，这包括ParamRef和GlobalRef
// 如果load的数组是本函数内定义的，即是AllocaInst，则只有当其地址被不完全load作为参数传递给一个函数时，这个函数才可能修改它
static bool is_call_load_alias(Decl *arr, CallInst *y) {
  return arr->is_param_array() || arr->is_glob || std::any_of(y->args.begin(), y->args.end(), [arr](Use &u) {
    auto a = dyn_cast<GetElementPtrInst>(u.value);
    return a && alias(arr, a->lhs_sym);
  });
}

struct LoadInfo {
  u32 id;
  std::vector<LoadInst *> loads;
  std::unordered_set<Inst *> stores;
};

void clear_memdep(IrFunc *f) {
  // 如果在同一趟循环中把操作数.set(nullptr)，同时delete，会出现先被delete后维护它的uses链表的情况，所以分两趟循环
  // 这里也不能用.value = nullptr，因为不能保证用到的指令最终都被删掉了，例如MemOpInst的mem_token可以是LoadInst
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->mem_phis.head; i; i = i->next) {
      auto i1 = static_cast<MemPhiInst *>(i);
      for (Use &u : i1->incoming_values) u.set(nullptr);
    }
    for (Inst *i = bb->insts.head; i; i = i->next) {
      if (auto x = dyn_cast<MemOpInst>(i)) x->mem_token.set(nullptr);
      else if (auto x = dyn_cast<LoadInst>(i)) x->mem_token.set(nullptr);
    }
  }
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->mem_phis.head; i;) {
      Inst *next = i->next;
      delete static_cast<MemPhiInst *>(i);
      i = next;
    }
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
}

// 构造load对store，store对load的依赖关系，分成两趟分别计算
void compute_memdep(IrFunc *f) {
  // 清空原来的结果
  clear_memdep(f);
  // 把所有数组地址相同的load一起考虑，因为相关的store集合计算出来必定是一样的
  std::unordered_map<Decl *, LoadInfo> loads;
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    for (Inst *i = bb->insts.head; i; i = i->next) {
      if (auto x = dyn_cast<LoadInst>(i)) {
        Decl *arr = x->lhs_sym;
        auto[it, inserted] = loads.insert({arr, {(u32) loads.size()}});
        LoadInfo &info = it->second;
        info.loads.push_back(x);
        if (!inserted) continue; // stores已经计算过了
        for (BasicBlock *bb1 = f->bb.head; bb1; bb1 = bb1->next) {
          for (Inst *i1 = bb1->insts.head; i1; i1 = i1->next) {
            bool is_alias = false;
            if (auto x = dyn_cast<StoreInst>(i1); x && alias(arr, x->lhs_sym))
              is_alias = true;
              // todo: 这里可以更仔细地考虑到底是否修改了参数，现在是粗略的判断，如果没有side effect一定没有修改参数/全局变量
            else if (auto x = dyn_cast<CallInst>(i1); x && x->func->has_side_effect && is_call_load_alias(arr, static_cast<CallInst *>(i1)))
              is_alias = true;
            if (is_alias) info.stores.insert(i1);
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
          values[loads.find(static_cast<Decl *>(i1->load_or_arr))->second.id] = i;
        }
        for (Inst *i = bb->insts.head; i; i = i->next) {
          if (auto x = dyn_cast<LoadInst>(i)) {
            x->mem_token.set(values[loads.find(x->lhs_sym)->second.id]);
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
              u32 id = loads.find(static_cast<Decl *>(i1->load_or_arr))->second.id;
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
          if (auto x = dyn_cast<LoadInst>(i); x) {
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

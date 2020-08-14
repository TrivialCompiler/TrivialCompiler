#include "inline_func.hpp"
#include "cfg.hpp"
#include "../../structure/ast.hpp"

void inline_func(IrProgram *p) {
  for (IrFunc *f = p->func.head; f; f = f->next) {
    bool can_inline = !f->builtin && f->bb.head->pred.empty();
    u32 inst_cnt = 0;
    for (BasicBlock *bb = f->bb.head; can_inline && bb; bb = bb->next) {
      for (Inst *i = bb->insts.head; can_inline && i; i = i->next) {
        if (isa<AllocaInst>(i) || ++inst_cnt >= 64) can_inline = false;
        // todo: 直接递归调用自身不能被内联，这是实现的限制，理论上应该是可以的，因为现在inline时是直接操作函数，会inline的同时修改它
        if (auto x = dyn_cast<CallInst>(i); x && x->func == f) can_inline = false;
      }
    }
    f->can_inline = can_inline;
  }

  std::unordered_map<BasicBlock *, BasicBlock *> bb_map;
  std::unordered_map<Value *, Value *> val_map;
  std::unordered_map<Decl *, Decl *> sym_map;
  auto get_sym = [&](Decl *sym) {
    if (auto it = sym_map.find(sym); it != sym_map.end()) return it->second;
    return sym;
  };
  std::vector<Value *> ret_map;
  std::vector<CallInst *> calls;

  for (IrFunc *f = p->func.head; f; f = f->next) {
    if (f->builtin) continue;
    calls.clear();
    for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
      for (Inst *i = bb->insts.head; i; i = i->next) {
        if (auto x = dyn_cast<CallInst>(i); x && x->func->can_inline) calls.push_back(x);
      }
    }
    for (CallInst *x : calls) {
      IrFunc *callee = x->func;
      {
        auto inline_func = "Inlining " + std::string(callee->func->name) + " into " + std::string(f->func->name);
        dbg(inline_func);
      }
      bb_map.clear(), val_map.clear(), sym_map.clear(), ret_map.clear();
      std::vector<Use> &args = x->args;
      std::vector<Decl> &params = x->func->func->params;
      for (u32 j = 0, sz = params.size(); j < sz; ++j) {
        if (params[j].is_param_array()) {
          Value *arg = args[j].value;
          assert(isa<GetElementPtrInst>(arg));
          sym_map.insert({&params[j], static_cast<GetElementPtrInst *>(arg)->lhs_sym});
        }
      }
      auto ret = new BasicBlock;
      for (BasicBlock *s : x->bb->succ()) {
        if (s) *std::find(s->pred.begin(), s->pred.end(), x->bb) = ret;
      }
      f->bb.insertAfter(ret, x->bb);
      auto get = [&](const Use &u) {
        Value *v = u.value;
        if (auto it = val_map.find(v); it != val_map.end()) return it->second;
        else if (auto x = dyn_cast<ParamRef>(v)) {
          return args[x->decl - params.data()].value;
        } else {
          assert(!isa<Inst>(v)); // 不同的函数之间可以用相同的各种Value，但是不能用相同的Inst
          return v;
        }
      };
      for (BasicBlock *bb = callee->bb.head; bb; bb = bb->next) {
        auto cloned = new BasicBlock;
        bb_map.insert({bb, cloned});
        f->bb.insertBefore(cloned, ret);
      }
      for (BasicBlock *bb : compute_rpo(callee)) {
        BasicBlock *cloned = bb_map.find(bb)->second;
        for (BasicBlock *p : bb->pred) {
          cloned->pred.push_back(bb_map.find(p)->second);
        }
        // phi指令间可能循环引用，还可能引用在自身之后定义的值，需要先定义好，最后再填值
        Inst *i = bb->insts.head;
        for (;; i = i->next) {
          if (isa<PhiInst>(i)) {
            val_map.insert({i, new PhiInst(cloned)});
          } else break;
        }
        for (; i; i = i->next) {
          Inst *res;
          if (auto x = dyn_cast<BinaryInst>(i))
            res = new BinaryInst(x->tag, get(x->lhs), get(x->rhs), cloned);
          else if (auto x = dyn_cast<BranchInst>(i))
            res = new BranchInst(get(x->cond), bb_map.find(x->left)->second, bb_map.find(x->right)->second, cloned);
          else if (auto x = dyn_cast<JumpInst>(i))
            res = new JumpInst(bb_map.find(x->next)->second, cloned);
          else if (auto x = dyn_cast<ReturnInst>(i)) {
            res = new JumpInst(ret, cloned);
            ret->pred.push_back(cloned);
            if (x->ret.value)
              ret_map.push_back(get(x->ret));
          } else if (auto x = dyn_cast<GetElementPtrInst>(i))
            res = new GetElementPtrInst(get_sym(x->lhs_sym), get(x->arr), get(x->index), x->multiplier, cloned);
          else if (auto x = dyn_cast<LoadInst>(i))
            res = new LoadInst(get_sym(x->lhs_sym), get(x->arr), get(x->index), cloned);
          else if (auto x = dyn_cast<StoreInst>(i))
            res = new StoreInst(get_sym(x->lhs_sym), get(x->arr), get(x->data), get(x->index), cloned);
          else if (auto x = dyn_cast<CallInst>(i)) {
            auto call = new CallInst(x->func, cloned);
            call->args.reserve(x->args.size());
            for (const Use &u : x->args) call->args.emplace_back(get(u), call);
            res = call;
          } else
            UNREACHABLE();
          val_map.insert({i, res});
        }
      }
      for (BasicBlock *bb = callee->bb.head; bb; bb = bb->next) {
        for (Inst *i = bb->insts.head;; i = i->next) {
          if (auto x = dyn_cast<PhiInst>(i)) {
            auto cloned = static_cast<PhiInst *>(val_map.find(x)->second);
            for (u32 j = 0, sz = x->incoming_values.size(); j < sz; ++j) {
              cloned->incoming_values[j].set(get(x->incoming_values[j]));
            }
          } else break;
        }
      }
      BasicBlock *bb = x->bb;
      for (Inst *j = x->next; j;) {
        Inst *next = j->next;
        bb->insts.remove(j);
        ret->insts.insertAtEnd(j);
        j->bb = ret;
        j = next;
      }
      BasicBlock *entry = bb_map.find(callee->bb.head)->second;
      new JumpInst(entry, bb);
      entry->pred.push_back(bb);
      if (callee->func->is_int) {
        auto phi = new PhiInst(ret);
        assert(phi->incoming_values.size() == ret_map.size());
        for (u32 j = 0, sz = phi->incoming_values.size(); j < sz; ++j) {
          phi->incoming_values[j].set(ret_map[j]);
        }
        x->replaceAllUseWith(phi);
      } else {
        assert(x->uses.head == nullptr);
      }
      bb->insts.remove(x);
      delete x;
    }
  }
}

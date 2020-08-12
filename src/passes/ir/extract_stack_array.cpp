#include "extract_stack_array.hpp"

#include <unordered_set>
#include <vector>

#include "memdep.hpp"
#include "../../structure/ast.hpp"

void extract_stack_array(IrProgram *p) {
  for (auto f = p->func.head; f; f = f->next) {
    if (f->builtin) continue;
    auto next_inst = [](Inst *inst, BasicBlock *&bb) -> Inst * {
      if (inst && inst->next) {
        return inst->next;
      } else {
        auto next_bb = bb->succ()[0];
        if (next_bb) {
          bb = next_bb;
          return bb->insts.head;
        } else {
          return nullptr;
        }
      }
    };
    // iterate through all instructions in a BB
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        // check each AllocaInst
        if (auto alloc = dyn_cast<AllocaInst>(inst)) {
          bool can_make_global = true;
          auto size = alloc->sym->dims[0]->result;
          auto buffer = new int[size]();  // auto initialized to 0
          std::unordered_set<StoreInst *> stores, met_stores;
          CallInst *memset = nullptr;
          for (auto use = alloc->uses.head; use; use = use->next) {
            if (auto store = dyn_cast<StoreInst>(use->user)) {
              if (auto data = dyn_cast<ConstValue>(store->data.value), index = dyn_cast<ConstValue>(store->index.value);
                  data && index) {
                buffer[index->imm] = data->imm;
                stores.insert(store);
              } else {
                can_make_global = false;
                break;
              }
            } else if (isa<GetElementPtrInst>(use->user)) {
              // used for function calling
              can_make_global = false;
              break;
            }
          }
          if (can_make_global) {
            BasicBlock *bb_check = alloc->bb;
            Inst *inst_check;
            // iterate from next instruction, until the first use
            for (inst_check = next_inst(alloc, bb_check); inst_check; inst_check = next_inst(inst_check, bb_check)) {
              if (auto store = dyn_cast<StoreInst>(inst_check)) {
                if (store->arr.value == alloc) {
                  met_stores.insert(store);
                }
              } else if (auto load = dyn_cast<LoadInst>(inst_check)) {
                if (load->arr.value == alloc) break;
              } else if (auto ptr = dyn_cast<GetElementPtrInst>(inst_check)) {
                if (alias(ptr->lhs_sym, alloc->sym)) break;
              } else if (auto call = dyn_cast<CallInst>(inst_check)) {
                for (auto &arg : call->args) {
                  // can only be memset, and will only appear once
                  if (arg.value == alloc) {
                    assert(memset == nullptr && call->func == Func::BUILTIN[8].val);
                    memset = call;
                  }
                }
              } else if (isa<BranchInst>(inst_check)) {
                break;
              }
            }
            if (met_stores == stores) {
              // all stores are visited, good!
              // extract this param to global
              std::vector<Expr *> init;
              init.reserve(size);
              for (int i = 0; i < size; ++i) {
                init.push_back(buffer[i] == 0 ? &IntConst::ZERO : new IntConst{Expr::Tag::IntConst, buffer[i]});
              }
              auto name =
                  new std::string("__extracted_" + std::string(f->func->name) + "_" + std::string(alloc->sym->name));
              auto extract_array = "Extract local array " + std::string(alloc->sym->name) + " to global " + *name;
              dbg(extract_array);
              auto extracted_decl =
                  new Decl{true, true, true, {name->c_str(), name->length()}, alloc->sym->dims, {nullptr}, init};
              extracted_decl->value = new GlobalRef(extracted_decl);
              p->glob_decl.push_back(extracted_decl);
              // remove memset
              if (memset) {
                memset->bb->insts.remove(memset);
                delete memset;
              }
              // replace all use to global ref
              alloc->replaceAllUseWith(extracted_decl->value);
              // remove all stores
              for (auto &s : stores) {
                s->bb->insts.remove(s);
                delete s;
              }
            }
          }
          delete[] buffer;
        }
      }
    }
  }
}

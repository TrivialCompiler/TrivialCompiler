#include "codegen.hpp"

#include <map>

#include "casting.hpp"
#include "common.hpp"

MachineProgram *machine_code_selection(IrProgram *p) {
  MachineProgram *ret = new MachineProgram;
  ret->glob_decl = p->glob_decl;
  for (auto f = p->func.head; f; f = f->next) {
    auto mf = new MachineFunc;
    ret->func.insertAtEnd(mf);
    mf->func = f;
    std::map<BasicBlock *, MachineBB *> bb_map;
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      auto mbb = new MachineBB;
      mf->bb.insertAtEnd(mbb);
      bb_map[bb] = mbb;
    }
    // map value to MachineOperand
    std::map<Value *, MachineOperand> val_map;
    // map global decl to MachineOperand
    std::map<Decl *, MachineOperand> glob_map;
    i32 virtual_max = 0;
    auto resolve = [&](Value *value) {
      if (auto x = dyn_cast<ParamRef>(value)) {
        // TODO: more than 4 args?
        for (int i = 0; i < f->func->params.size(); i++) {
          if (&f->func->params[i] == x->decl) {
            return MachineOperand{.state = MachineOperand::PreColored, .value = i};
          }
        }
        UNREACHABLE();
      } else if (auto x = dyn_cast<GlobalRef>(value)) {
        auto it = glob_map.find(x->decl);
        if (it == glob_map.end()) {
          // load global addr in entry bb
          auto new_inst = new MIGlobal(x->decl, mf->bb.head);
          // allocate virtual reg
          i32 vreg = virtual_max++;
          MachineOperand res = {.state = MachineOperand::Virtual, .value = vreg};
          val_map[value] = res;
          glob_map[x->decl] = res;
          new_inst->dst = res;
          return res;
        } else {
          return it->second;
        }
      } else if (auto x = dyn_cast<ConstValue>(value)) {
        return MachineOperand{.state = MachineOperand::Immediate, .value = x->imm};
      } else {
        auto it = val_map.find(value);
        if (it == val_map.end()) {
          // allocate virtual reg
          i32 vreg = virtual_max++;
          MachineOperand res = {.state = MachineOperand::Virtual, .value = vreg};
          val_map[value] = res;
          return res;
        } else {
          return it->second;
        }
      }
    };

    auto resolve_no_imm = [&](Value *value, MachineBB *mbb) {
      if (auto y = dyn_cast<ConstValue>(value)) {
        // can't store an immediate directly
        i32 vreg = virtual_max++;
        MachineOperand res = {.state = MachineOperand::Virtual, .value = vreg};
        // move val to vreg
        auto mv_inst = new MIUnary(MachineInst::Mv, mbb);
        mv_inst->dst = res;
        mv_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, .value = y->imm};
        return res;
      } else {
        return resolve(value);
      }
    };

    for (auto bb = f->bb.head; bb; bb = bb->next) {
      auto mbb = bb_map[bb];
      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        if (auto x = dyn_cast<JumpInst>(inst)) {
          auto new_inst = new MIJump(bb_map[x->next], mbb);
        } else if (auto x = dyn_cast<LoadInst>(inst)) {
          // TODO: dims
          if (x->dims.size() == x->lhs_sym->dims.size()) {
            // access to element
            auto arr = resolve(x->arr.value);
            MachineOperand offset;
            i32 shift = 0;
            if (x->dims.size() == 0) {
              // zero offset
              offset = MachineOperand{.state = MachineOperand::Immediate, .value = 0};
            } else if (x->dims.size() == 1) {
              // simple offset
              offset = resolve(x->dims[0].value);
              shift = 2;
            } else {
              // TODO
              UNREACHABLE();
            }

            auto new_inst = new MILoad(mbb);
            new_inst->addr = arr;
            new_inst->offset = offset;
            new_inst->dst = resolve(inst);
            new_inst->shift = shift;
          } else {
            // TODO
            UNREACHABLE();
          }
        } else if (auto x = dyn_cast<StoreInst>(inst)) {
          // TODO: dims
          auto arr = resolve(x->arr.value);
          auto data = resolve_no_imm(x->data.value, mbb);

          auto new_inst = new MIStore(mbb);
          new_inst->addr = arr;
          new_inst->data = data;
          new_inst->offset = MachineOperand{.state = MachineOperand::Immediate, .value = 0};
        } else if (auto x = dyn_cast<ReturnInst>(inst)) {
          if (x->ret.value) {
            auto val = resolve(x->ret.value);
            // move val to a0
            auto mv_inst = new MIUnary(MachineInst::Mv, mbb);
            mv_inst->dst = MachineOperand{.state = MachineOperand::PreColored, .value = 0};
            mv_inst->rhs = val;
          }
          auto new_inst = new MIReturn(mbb);
        } else if (auto x = dyn_cast<BinaryInst>(inst)) {
          auto lhs = resolve_no_imm(x->lhs.value, mbb);
          auto rhs = resolve_no_imm(x->rhs.value, mbb);
          auto new_inst = new MIBinary((MachineInst::Tag)x->tag, mbb);
          new_inst->dst = resolve(inst);
          new_inst->lhs = lhs;
          new_inst->rhs = rhs;
        } else if (auto x = dyn_cast<BranchInst>(inst)) {
          auto cond = resolve_no_imm(x->cond.value, mbb);
          // if cond != 0
          auto cmp_inst = new MICompare(mbb);
          cmp_inst->lhs = cond;
          cmp_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, .value = 0};
          auto new_inst = new MIBranch(mbb);
          new_inst->cond = MIBranch::Ne;
          new_inst->target = bb_map[x->left];
          auto fallback_inst = new MIJump(bb_map[x->right], mbb);
        }
      }
    }
  }
  return ret;
}
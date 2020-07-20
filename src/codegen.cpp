#include <map>

#include "codegen.hpp"
#include "casting.hpp"
#include "common.hpp"

MachineProgram *run_codegen(IrProgram *p) {
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
      if (auto x = dyn_cast<GlobalRef>(value)) {
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

    for (auto bb = f->bb.head; bb; bb = bb->next) {
      auto mbb = bb_map[bb];
      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        if (auto x = dyn_cast<JumpInst>(inst)) {
          auto new_inst = new MIJump(bb_map[x->next], mbb);
        } else if (auto x = dyn_cast<LoadInst>(inst)) {
          // TODO: dims
          auto arr = resolve(x->arr.value);
          auto new_inst = new MILoad(mbb);
          new_inst->addr = arr;
          new_inst->offset = MachineOperand{.state = MachineOperand::Immediate, .value = 0};
          new_inst->dst = resolve(inst);
        } else if (auto x = dyn_cast<StoreInst>(inst)) {
          // TODO: dims
          auto arr = resolve(x->arr.value);

          MachineOperand data;
          if (auto y = dyn_cast<ConstValue>(x->data.value)) {
            // can't store an immediate directly
            i32 vreg = virtual_max++;
            MachineOperand res = {.state = MachineOperand::Virtual, .value = vreg};
            // move val to vreg
            auto mv_inst = new MIUnary(MachineInst::Mv, mbb);
            mv_inst->dst = res;
            mv_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, .value = y->imm};
            data = res;
          } else {
            data = resolve(x->data.value);
          }

          auto new_inst = new MIStore(mbb);
          new_inst->addr = arr;
          new_inst->data = data;
          new_inst->offset = MachineOperand{.state = MachineOperand::Immediate, .value = 0};
        } else if (auto x = dyn_cast<ReturnInst>(inst)) {
          auto val = resolve(x->ret.value);
          // move val to a0
          auto mv_inst = new MIUnary(MachineInst::Mv, mbb);
          mv_inst->dst = MachineOperand{.state = MachineOperand::PreColored, .value = 0};
          mv_inst->rhs = val;
          auto new_inst = new MIReturn(mbb);
        }
      }
    }
  }
  return ret;
}
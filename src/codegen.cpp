#include "codegen.hpp"

#include <map>
#include <optional>

#include "casting.hpp"
#include "common.hpp"

// list of assignments (lhs, rhs)
using ParMv = std::vector<std::pair<MachineOperand, MachineOperand>>;

void insert_parallel_mv(ParMv &movs, MachineInst *insertBefore) {
  // TODO: serialization
  for (auto &[lhs, rhs] : movs) {
    auto inst = new MIMove(insertBefore);
    inst->dst = lhs;
    inst->rhs = rhs;
  }
}

MachineProgram *machine_code_selection(IrProgram *p) {
  MachineProgram *ret = new MachineProgram;
  ret->glob_decl = p->glob_decl;
  for (auto f = p->func.head; f; f = f->next) {
    auto mf = new MachineFunc;
    ret->func.insertAtEnd(mf);
    mf->func = f;

    // 1. create machine bb 1-to-1
    std::map<BasicBlock *, MachineBB *> bb_map;
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      auto mbb = new MachineBB;
      mf->bb.insertAtEnd(mbb);
      bb_map[bb] = mbb;
    }
    // maintain pred and succ
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      auto mbb = bb_map[bb];
      mbb->pred.reserve(bb->pred.size());
      // at most two successor
      auto succ = bb->succ();
      for (int i = 0; i < 2; i++) {
        if (succ[i]) {
          mbb->succ[i] = bb_map[succ[i]];
        } else {
          mbb->succ[i] = nullptr;
        }
      }
      for (auto &pred : bb->pred) {
        mbb->pred.push_back(bb_map[pred]);
      }
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
        auto mv_inst = new MIMove(mbb);
        mv_inst->dst = res;
        mv_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, .value = y->imm};
        return res;
      } else {
        return resolve(value);
      }
    };

    // 2. translate instructions except phi
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      auto mbb = bb_map[bb];
      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        if (auto x = dyn_cast<JumpInst>(inst)) {
          auto new_inst = new MIJump(bb_map[x->next], mbb);
          mbb->control_transter_inst = new_inst;
        } else if (auto x = dyn_cast<LoadInst>(inst)) {
          // TODO: dims
          if (x->dims.size() == x->lhs_sym->dims.size()) {
            // access to element
            auto arr = resolve(x->arr.value);
            MachineOperand offset;
            if (x->dims.size() == 0) {
              // zero offset
              offset = MachineOperand{.state = MachineOperand::Immediate, .value = 0};
            } else if (x->dims.size() == 1) {
              // simple offset
              offset = resolve(x->dims[0].value);
            } else {
              // sum of index * number of elements
              // allocate two registers: multiply and add
              i32 mul_vreg = virtual_max++;
              i32 add_vreg = virtual_max++;

              // mov add_vreg, 0
              auto mv_inst = new MIMove(mbb);
              mv_inst->dst = MachineOperand{.state = MachineOperand::Virtual, .value = add_vreg};
              mv_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, .value = 0};

              for (int i = 0; i < x->dims.size(); i++) {
                // number of elements
                i32 imm = 1;
                if (i + 1 < x->dims.size()) {
                  imm = x->lhs_sym->dims[i + 1]->result;
                }

                // mov mul_vreg, imm
                auto mv_inst = new MIMove(mbb);
                mv_inst->dst = MachineOperand{.state = MachineOperand::Virtual, .value = mul_vreg};
                mv_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, .value = imm};

                // mul mul_vreg, mul_vreg, dim
                auto mul_inst = new MIBinary(MachineInst::Mul, mbb);
                mul_inst->dst = MachineOperand{.state = MachineOperand::Virtual, .value = mul_vreg};
                mul_inst->lhs = MachineOperand{.state = MachineOperand::Virtual, .value = mul_vreg};
                mul_inst->rhs = resolve(x->dims[i].value);

                // add add_vreg, add_vreg, mul_vreg
                auto add_inst = new MIBinary(MachineInst::Add, mbb);
                add_inst->dst = MachineOperand{.state = MachineOperand::Virtual, .value = add_vreg};
                add_inst->lhs = MachineOperand{.state = MachineOperand::Virtual, .value = add_vreg};
                add_inst->rhs = MachineOperand{.state = MachineOperand::Virtual, .value = mul_vreg};
              }

              offset = MachineOperand{.state = MachineOperand::Virtual, .value = add_vreg};
            }

            auto new_inst = new MILoad(mbb);
            new_inst->addr = arr;
            new_inst->offset = offset;
            new_inst->dst = resolve(inst);
            new_inst->shift = 2;
          } else {
            // get addr of sub array
            // TODO
            auto mv_inst = new MIMove(mbb);
            mv_inst->dst = resolve(inst);
            mv_inst->rhs = resolve(x->arr.value);
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
            auto mv_inst = new MIMove(mbb);
            mv_inst->dst = MachineOperand{.state = MachineOperand::PreColored, .value = 0};
            mv_inst->rhs = val;
            auto new_inst = new MIReturn(mbb);
            mbb->control_transter_inst = mv_inst;
          } else {
            auto new_inst = new MIReturn(mbb);
            mbb->control_transter_inst = new_inst;
          }
        } else if (auto x = dyn_cast<BinaryInst>(inst)) {
          auto lhs = resolve_no_imm(x->lhs.value, mbb);
          auto rhs = resolve_no_imm(x->rhs.value, mbb);
          if (BinaryInst::Lt <= x->tag && x->tag <= BinaryInst::Ne) {
            // transform compare instructions
            auto dst = resolve(inst);
            auto new_inst = new MICompare(mbb);
            new_inst->lhs = lhs;
            new_inst->rhs = rhs;

            ArmCond cond, opposite;
            if (x->tag == BinaryInst::Gt) {
              cond = Gt;
              opposite = Le;
            } else if (x->tag == BinaryInst::Ge) {
              cond = Ge;
              opposite = Lt;
            } else if (x->tag == BinaryInst::Le) {
              cond = Le;
              opposite = Gt;
            } else if (x->tag == BinaryInst::Lt) {
              cond = Lt;
              opposite = Ge;
            } else if (x->tag == BinaryInst::Eq) {
              cond = Eq;
              opposite = Ne;
            } else if (x->tag == BinaryInst::Ne) {
              cond = Ne;
              opposite = Eq;
            } else {
              UNREACHABLE();
            }

            auto mv1_inst = new MIMove(mbb);
            mv1_inst->dst = dst;
            mv1_inst->cond = cond;
            mv1_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, .value = 1};
            auto mv0_inst = new MIMove(mbb);
            mv0_inst->dst = dst;
            mv0_inst->cond = opposite;
            mv0_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, .value = 0};
          } else {
            auto new_inst = new MIBinary((MachineInst::Tag)x->tag, mbb);
            new_inst->dst = resolve(inst);
            new_inst->lhs = lhs;
            new_inst->rhs = rhs;
          }
        } else if (auto x = dyn_cast<BranchInst>(inst)) {
          auto cond = resolve_no_imm(x->cond.value, mbb);
          // if cond != 0
          auto cmp_inst = new MICompare(mbb);
          cmp_inst->lhs = cond;
          cmp_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, .value = 0};
          mbb->control_transter_inst = cmp_inst;
          auto new_inst = new MIBranch(mbb);
          new_inst->cond = ArmCond::Ne;
          new_inst->target = bb_map[x->left];
          auto fallback_inst = new MIJump(bb_map[x->right], mbb);
        }
      }
    }

    // 3. handle phi nodes
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      auto mbb = bb_map[bb];
      // collect phi node information
      // (lhs, vreg) assignments
      ParMv lhs;
      // each bb has a list of (vreg, rhs) parallel moves
      std::map<BasicBlock *, ParMv> mv;
      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        // phi insts must appear at the beginning of bb
        if (auto x = dyn_cast<PhiInst>(inst)) {
          // for each phi:
          // lhs = phi [r1 bb1], [r2 bb2] ...
          // 1. create vreg for each inst
          // 2. add parallel mv (lhs1, ...) = (vreg1, ...)
          // 3. add parallel mv in each bb: (vreg1, ...) = (r1, ...)
          i32 vreg = virtual_max++;
          MachineOperand vr = {.state = MachineOperand::Virtual, .value = vreg};
          lhs.emplace_back(resolve(inst), vr);
          for (int i = 0; i < x->incoming_bbs->size(); i++) {
            auto pred_bb = x->incoming_bbs->at(i);
            auto val = resolve(x->incoming_values[i].value);
            mv[pred_bb].emplace_back(vr, val);
          }
        } else {
          break;
        }
      }
      // insert parallel mv at the beginning of current mbb
      insert_parallel_mv(lhs, mbb->insts.head);
      // insert parallel mv before the control transfer instruction of pred mbb
      for (auto &[bb, movs] : mv) {
        auto mbb = bb_map[bb];
        insert_parallel_mv(movs, mbb->control_transter_inst);
      }
    }
  }
  return ret;
}

std::pair<MachineOperand *, std::vector<MachineOperand *>> get_def_use(MachineInst *inst) {
  MachineOperand *def = nullptr;
  std::vector<MachineOperand *> use;

  if (auto x = dyn_cast<MIBinary>(inst)) {
    def = &x->dst;
    use = {&x->lhs, &x->rhs};
  } else if (auto x = dyn_cast<MIUnary>(inst)) {
    def = &x->dst;
    use = {&x->rhs};
  } else if (auto x = dyn_cast<MIMove>(inst)) {
    def = &x->dst;
    use = {&x->rhs};
  } else if (auto x = dyn_cast<MILoad>(inst)) {
    def = &x->dst;
    use = {&x->addr, &x->offset};
  } else if (auto x = dyn_cast<MIStore>(inst)) {
    use = {&x->data, &x->addr, &x->offset};
  } else if (auto x = dyn_cast<MICompare>(inst)) {
    use = {&x->lhs, &x->rhs};
  } else if (auto x = dyn_cast<MICall>(inst)) {
    // TODO
  } else if (auto x = dyn_cast<MIGlobal>(inst)) {
    def = &x->dst;
  }
  return {def, use};
}

void liveness_analysis(MachineProgram *p) {
  for (auto f = p->func.head; f; f = f->next) {
    // calculate LiveUse and Def sets for each bb
    // each elements is a virtual register
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        auto [def, use] = get_def_use(inst);

        // liveuse
        for (auto &u : use) {
          if (u && u->is_virtual() && bb->def.find(*u) == bb->def.end()) {
            bb->liveuse.insert(*u);
          }
        }
        // def
        if (def && def->is_virtual() && bb->liveuse.find(*def) == bb->liveuse.end()) {
          bb->def.insert(*def);
        }
      }
      // initial values
      bb->livein = bb->liveuse;
    }

    // calculate LiveIn and LiveOut for each bb
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto bb = f->bb.head; bb; bb = bb->next) {
        std::set<MachineOperand> new_out;
        for (auto &succ : bb->succ) {
          if (succ) {
            new_out.insert(succ->livein.begin(), succ->livein.end());
          }
        }

        if (new_out != bb->liveout) {
          changed = true;
          bb->liveout = new_out;
          std::set<MachineOperand> new_in = bb->liveuse;
          // TODO: optimize
          for (auto &e : bb->liveout) {
            if (bb->def.find(e) == bb->def.end()) {
              new_in.insert(e);
            }
          }

          bb->livein = new_in;
        }
      }
    };
  }
}

void register_allocate(MachineProgram *p) {
  for (auto f = p->func.head; f; f = f->next) {
    // interference graph
    // adjacent list
    std::map<MachineOperand, std::set<MachineOperand>> graph;
    for (auto bb = f->bb.tail; bb; bb = bb->prev) {
      // calculate live set before each instruction
      auto live = bb->liveout;
      for (auto inst = bb->insts.tail; inst; inst = inst->prev) {
        auto [def, use] = get_def_use(inst);

        // update live set
        if (def && def->is_virtual()) {
          live.erase(*def);
        }
        for (auto &u: use) {
          if (u->is_virtual() && live.insert(*u).second) {
            // new element inserted
            // it interfere with existing elements
            for (auto &e: live) {
              if (e != *u) {
                // interfere
                std::cout << e << " interfere with " << *u << std::endl;
                graph[e].insert(*u);
                graph[*u].insert(e);
              }
            }
          }
        }
      }
    }
  }
}

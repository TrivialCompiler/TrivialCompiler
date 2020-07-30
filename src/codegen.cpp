#include "codegen.hpp"

#include <cassert>
#include <map>
#include <optional>

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
  auto ret = new MachineProgram;
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
    // map param decl to MachineOperand
    std::map<Decl *, MachineOperand> param_map;

    // virtual registers
    i32 virtual_max = 0;
    auto new_virtual_reg = [&]() { return MachineOperand{.state = MachineOperand::Virtual, .value = virtual_max++}; };

    // resolve imm as instruction operand
    // ARM has limitations, see https://stackoverflow.com/questions/10261300/invalid-constant-after-fixup
    auto get_imm_operand = [&](i32 imm, MachineBB *mbb) {
      auto operand = MachineOperand{.state = MachineOperand::Immediate, .value = imm};
      if (can_encode_imm(imm)) {
        // directly encoded in instruction as imm
        return operand;
      } else {
        auto imm_split = "Immediate number " + std::to_string(imm) + " cannot be encoded, converting to MIMove";
        dbg(imm_split);
        // use MIMove, which automatically splits if necessary
        auto vreg = new_virtual_reg();
        if (mbb->control_transfer_inst) {
          // insert before control transfer when there is control transfer instruction
          auto mv_inst = new MIMove(mbb->control_transfer_inst);
          mv_inst->dst = vreg;
          mv_inst->rhs = operand;
        } else {
          // otherwise, insert at end
          auto mv_inst = new MIMove(mbb);
          mv_inst->dst = vreg;
          mv_inst->rhs = operand;
        }
        return vreg;
      }
    };

    // resolve value reference
    auto resolve = [&](Value *value, MachineBB *mbb) {
      if (auto x = dyn_cast<ParamRef>(value)) {
        auto it = param_map.find(x->decl);
        if (it == param_map.end()) {
          // copy param to vreg in entry bb
          auto new_inst = new MIMove(mf->bb.head, 0);
          // allocate virtual reg
          auto res = new_virtual_reg();
          val_map[value] = res;
          param_map[x->decl] = res;
          new_inst->dst = res;
          // TODO: more than 4 args?
          for (int i = 0; i < f->func->params.size(); i++) {
            if (&f->func->params[i] == x->decl) {
              new_inst->rhs = MachineOperand{.state = MachineOperand::PreColored, .value = i};
            }
          }
          return res;
        } else {
          return it->second;
        }
      } else if (auto x = dyn_cast<GlobalRef>(value)) {
        auto it = glob_map.find(x->decl);
        if (it == glob_map.end()) {
          // load global addr in entry bb
          auto new_inst = new MIGlobal(x->decl, mf->bb.head);
          // allocate virtual reg
          auto res = new_virtual_reg();
          val_map[value] = res;
          glob_map[x->decl] = res;
          new_inst->dst = res;
          return res;
        } else {
          return it->second;
        }
      } else if (auto x = dyn_cast<ConstValue>(value)) {
        return get_imm_operand(x->imm, mbb);
      } else {
        auto it = val_map.find(value);
        if (it == val_map.end()) {
          // allocate virtual reg
          auto res = new_virtual_reg();
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
        auto res = new_virtual_reg();
        // move val to vreg
        auto mv_inst = new MIMove(mbb);
        mv_inst->dst = res;
        mv_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, .value = y->imm};
        return res;
      } else {
        return resolve(value, mbb);
      }
    };

    // calculate the offset in array indexing expr arr[a][b][c]
    // it supports both complete (element) and incomplete (subarray) indexing
    // e.g. for int a[2][3], it could calculate the offset of a, a[x] and a[x][y]
    auto calculate_offset = [&](MachineBB *mbb, const std::vector<Use> &access_dims,
                                const std::vector<Expr *> &var_dims) -> MachineOperand {
      // direct access
      if (access_dims.empty()) return get_imm_operand(0, mbb);

      // sum of index * number of elements
      // allocate two registers: multiply and add
      auto mul_vreg = new_virtual_reg();
      auto add_vreg = new_virtual_reg();

      // mov add_vreg, 0
      auto mv_inst = new MIMove(mbb);
      mv_inst->dst = add_vreg;
      mv_inst->rhs = get_imm_operand(0, mbb);

      // TODO: eliminate MUL when sub_arr_size == 1
      for (int i = 0; i < access_dims.size(); i++) {
        // number of elements
        i32 sub_arr_size = i + 1 < var_dims.size() ? var_dims[i + 1]->result : 1;

        // mov mul_vreg, sub_arr_size
        auto mv_inst = new MIMove(mbb);
        mv_inst->dst = mul_vreg;
        mv_inst->rhs = get_imm_operand(sub_arr_size, mbb);

        // mul mul_vreg, dim, mul_vreg
        auto current_dim_index = resolve_no_imm(access_dims[i].value, mbb);
        auto mul_inst = new MIBinary(MachineInst::Mul, mbb);
        // note: Rd and Rm should be different in mul
        mul_inst->dst = mul_vreg;
        mul_inst->lhs = current_dim_index;
        mul_inst->rhs = mul_vreg;

        // add add_vreg, add_vreg, mul_vreg
        auto add_inst = new MIBinary(MachineInst::Add, mbb);
        add_inst->dst = add_vreg;
        add_inst->lhs = add_vreg;
        add_inst->rhs = mul_vreg;
      }

      return add_vreg;
    };

    // 2. translate instructions except phi
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      auto mbb = bb_map[bb];
      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        if (auto x = dyn_cast<JumpInst>(inst)) {
          auto new_inst = new MIJump(bb_map[x->next], mbb);
          mbb->control_transfer_inst = new_inst;
        } else if (auto x = dyn_cast<AccessInst>(inst)) {
          // store or load, nearly all the same
          auto arr = resolve(x->arr.value, mbb);
          new MIComment("begin offset calculation: " + std::string(x->lhs_sym->name), mbb);
          auto offset = calculate_offset(mbb, x->dims, x->lhs_sym->dims);
          new MIComment("finish offset calculation", mbb);
          // resolve index access
          if (x->dims.size() == x->lhs_sym->dims.size()) {
            // access to element
            MIAccess *access_inst;
            if (auto x = dyn_cast<StoreInst>(inst)) {
              // store to element
              new MIComment("begin store", mbb);
              auto data = resolve_no_imm(x->data.value, mbb);  // must get data first
              access_inst = new MIStore(mbb);
              static_cast<MIStore *>(access_inst)->data = data;
              new MIComment("end store", mbb);
            } else {
              // load from element
              new MIComment("begin load", mbb);
              access_inst = new MILoad(mbb);
              static_cast<MILoad *>(access_inst)->dst = resolve(inst, mbb);
              new MIComment("end load", mbb);
            }
            access_inst->addr = arr;
            access_inst->offset = offset;
            access_inst->shift = 2;
          } else {
            // access to subarray
            assert(x->tag != Value::Store);  // you could only store to a specific element
            // addr <- &arr
            new MIComment("begin subarray load", mbb);
            auto addr = new_virtual_reg();
            auto mv_inst = new MIMove(mbb);
            mv_inst->dst = addr;
            mv_inst->rhs = resolve(x->arr.value, mbb);
            // offset <- offset * 4
            if (offset.state == MachineOperand::Immediate) {
              offset.value *= 4;
            } else {
              auto mul_inst = new MIBinary(MachineInst::Tag::Mul, mbb);
              mul_inst->dst = offset;
              mul_inst->lhs = offset;
              mul_inst->rhs = get_imm_operand(4, mbb);
            }
            // inst <- addr + offset
            auto add_inst = new MIBinary(MachineInst::Tag::Add, mbb);
            add_inst->dst = resolve(inst, mbb);
            add_inst->lhs = addr;
            add_inst->rhs = offset;
            new MIComment("end subarray load", mbb);
          }
        } else if (auto x = dyn_cast<ReturnInst>(inst)) {
          if (x->ret.value) {
            auto val = resolve(x->ret.value, mbb);
            // move val to a0
            auto mv_inst = new MIMove(mbb);
            mv_inst->dst = MachineOperand{.state = MachineOperand::PreColored, .value = 0};
            mv_inst->rhs = val;
            auto new_inst = new MIReturn(mbb);
            mbb->control_transfer_inst = mv_inst;
          } else {
            auto new_inst = new MIReturn(mbb);
            mbb->control_transfer_inst = new_inst;
          }
        } else if (auto x = dyn_cast<BinaryInst>(inst)) {
          auto lhs = resolve_no_imm(x->lhs.value, mbb);
          MachineOperand rhs{};
          if (x->canUseImmOperand() && x->rhs.value->tag == Value::Const) {
            rhs = get_imm_operand(static_cast<ConstValue *>(x->rhs.value)->imm, mbb);  // might be imm or register
          } else {
            rhs = resolve_no_imm(x->rhs.value, mbb);
          }
          if (BinaryInst::Lt <= x->tag && x->tag <= BinaryInst::Ne) {
            // transform compare instructions
            auto dst = resolve(inst, mbb);
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
            mv1_inst->rhs = get_imm_operand(1, mbb);
            auto mv0_inst = new MIMove(mbb);
            mv0_inst->dst = dst;
            mv0_inst->cond = opposite;
            mv0_inst->rhs = get_imm_operand(0, mbb);
          } else if (BinaryInst::And <= x->tag && x->tag <= BinaryInst::Or) {
            // lhs && rhs:
            // cmp lhs, #0
            // movne v1, #1
            // cmp rhs, #0
            // movne v2, #1
            // and/or dst, v1, v2

            // lhs
            auto cmp_lhs_inst = new MICompare(mbb);
            cmp_lhs_inst->lhs = lhs;
            cmp_lhs_inst->rhs = get_imm_operand(0, mbb);
            auto mv_lhs_inst = new MIMove(mbb);
            auto lhs_vreg = new_virtual_reg();
            mv_lhs_inst->dst = lhs_vreg;
            mv_lhs_inst->cond = ArmCond::Ne;
            mv_lhs_inst->rhs = get_imm_operand(1, mbb);

            // rhs
            auto cmp_rhs_inst = new MICompare(mbb);
            cmp_rhs_inst->lhs = rhs;
            cmp_rhs_inst->rhs = get_imm_operand(0, mbb);
            auto mv_rhs_inst = new MIMove(mbb);
            auto rhs_vreg = new_virtual_reg();
            mv_rhs_inst->dst = rhs_vreg;
            mv_rhs_inst->cond = ArmCond::Ne;
            mv_rhs_inst->rhs = get_imm_operand(1, mbb);

            // and
            auto new_inst = new MIBinary((MachineInst::Tag)x->tag, mbb);
            new_inst->dst = resolve(inst, mbb);
            new_inst->lhs = lhs_vreg;
            new_inst->rhs = rhs_vreg;
          } else {
            auto new_inst = new MIBinary((MachineInst::Tag)x->tag, mbb);
            new_inst->dst = resolve(inst, mbb);
            new_inst->lhs = lhs;
            new_inst->rhs = rhs;
          }
        } else if (auto x = dyn_cast<BranchInst>(inst)) {
          auto cond = resolve_no_imm(x->cond.value, mbb);
          // if cond != 0
          auto cmp_inst = new MICompare(mbb);
          cmp_inst->lhs = cond;
          cmp_inst->rhs = get_imm_operand(0, mbb);
          mbb->control_transfer_inst = cmp_inst;
          auto new_inst = new MIBranch(mbb);
          new_inst->cond = ArmCond::Ne;
          new_inst->target = bb_map[x->left];
          auto fallback_inst = new MIJump(bb_map[x->right], mbb);
        } else if (auto x = dyn_cast<CallInst>(inst)) {
          // move args to r0-r3
          assert(x->func->params.size() <= 4);
          std::vector<MachineOperand> params;
          for (int i = 0; i < x->func->params.size(); i++) {
            auto rhs = resolve(x->args[i].value, mbb);
            auto mv_inst = new MIMove(mbb);
            // r0 to r3
            mv_inst->dst = MachineOperand{.state = MachineOperand::PreColored, .value = i};
            mv_inst->rhs = rhs;
          }

          auto new_inst = new MICall(mbb);
          new_inst->func = x->func;

          // return
          if (x->func->is_int) {
            // has return
            // move r0 to dst
            auto dst = resolve(inst, mbb);
            auto mv_inst = new MIMove(mbb);
            mv_inst->dst = dst;
            mv_inst->rhs = MachineOperand{.state = MachineOperand::PreColored, .value = 0};
          }
        } else if (auto x = dyn_cast<AllocaInst>(inst)) {
          i32 size = 1;
          if (!x->sym->dims.empty()) {
            size = x->sym->dims[0]->result;
          }
          size *= 4;
          mf->sp_offset += size;
          i32 offset = mf->sp_offset;
          auto dst = resolve(inst, mbb);
          auto add_inst = new MIBinary(MachineInst::Add, mbb);
          add_inst->dst = dst;
          // fp is r11
          add_inst->lhs = MachineOperand{.state = MachineOperand::PreColored, r11};
          add_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, -offset};
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
          auto vr = new_virtual_reg();
          lhs.emplace_back(resolve(inst, mbb), vr);
          for (int i = 0; i < x->incoming_bbs->size(); i++) {
            auto pred_bb = x->incoming_bbs->at(i);
            auto val = resolve(x->incoming_values[i].value, bb_map[pred_bb]);
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
        insert_parallel_mv(movs, mbb->control_transfer_inst);
      }
    }

    mf->virtual_max = virtual_max;
  }
  return ret;
}

std::pair<std::vector<MachineOperand>, std::vector<MachineOperand>> get_def_use(MachineInst *inst) {
  std::vector<MachineOperand> def;
  std::vector<MachineOperand> use;

  if (auto x = dyn_cast<MIBinary>(inst)) {
    def = {x->dst};
    use = {x->lhs, x->rhs};
  } else if (auto x = dyn_cast<MIUnary>(inst)) {
    def = {x->dst};
    use = {x->rhs};
  } else if (auto x = dyn_cast<MIMove>(inst)) {
    def = {x->dst};
    use = {x->rhs};
  } else if (auto x = dyn_cast<MILoad>(inst)) {
    def = {x->dst};
    use = {x->addr, x->offset};
  } else if (auto x = dyn_cast<MIStore>(inst)) {
    use = {x->data, x->addr, x->offset};
  } else if (auto x = dyn_cast<MICompare>(inst)) {
    use = {x->lhs, x->rhs};
  } else if (auto x = dyn_cast<MICall>(inst)) {
    // args (also caller save)
    for (int i = r0; i <= r3; i++) {
      def.push_back(MachineOperand{.state = MachineOperand::PreColored, .value = i});
      use.push_back(MachineOperand{.state = MachineOperand::PreColored, .value = i});
    }
  } else if (auto x = dyn_cast<MIGlobal>(inst)) {
    def = {x->dst};
  } else if (auto x = dyn_cast<MIReturn>(inst)) {
    // ret
    use.push_back(MachineOperand{.state = MachineOperand::PreColored, .value = r0});
  }
  return {def, use};
}

std::pair<MachineOperand *, std::vector<MachineOperand *>> get_def_use_ptr(MachineInst *inst) {
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
    def = {&x->dst};
  }
  return {def, use};
}

void liveness_analysis(MachineFunc *f) {
  // calculate LiveUse and Def sets for each bb
  // each elements is a virtual register or precolored register
  for (auto bb = f->bb.head; bb; bb = bb->next) {
    bb->liveuse.clear();
    bb->def.clear();
    for (auto inst = bb->insts.head; inst; inst = inst->next) {
      auto [def, use] = get_def_use(inst);

      // liveuse
      for (auto &u : use) {
        if (u.needs_color() && bb->def.find(u) == bb->def.end()) {
          bb->liveuse.insert(u);
        }
      }
      // def
      for (auto &d : def) {
        if (d.needs_color() && bb->liveuse.find(d) == bb->liveuse.end()) {
          bb->def.insert(d);
        }
      }
    }
    // initial values
    bb->livein = bb->liveuse;
    bb->liveout.clear();
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

// iterated register coalescing
void register_allocate(MachineProgram *p) {
  dbg("Allocating registers");
  for (auto f = p->func.head; f; f = f->next) {
    dbg(f->func->func->name);
    bool done = false;
    while (!done) {
      liveness_analysis(f);
      // interference graph
      // each node is a MachineOperand
      // can only Precolored or Virtual
      // adjacent list
      std::map<MachineOperand, std::set<MachineOperand>> adj_list;
      // adjacent set
      std::set<std::pair<MachineOperand, MachineOperand>> adj_set;
      // other variables in the paper
      std::map<MachineOperand, u32> degree;
      std::map<MachineOperand, MachineOperand> alias;
      std::map<MachineOperand, std::set<MIMove *>> move_list;
      std::set<MachineOperand> simplify_worklist;
      std::set<MachineOperand> freeze_worklist;
      std::set<MachineOperand> spill_worklist;
      std::set<MachineOperand> spilled_nodes;
      std::set<MachineOperand> coalesced_nodes;
      std::vector<MachineOperand> colored_nodes;
      std::vector<MachineOperand> select_stack;
      std::set<MIMove *> coalesced_moves;
      std::set<MIMove *> constrained_moves;
      std::set<MIMove *> frozen_moves;
      std::set<MIMove *> worklist_moves;
      std::set<MIMove *> active_moves;

      // allocatable registers
      i32 k = r11 - r0 + 1;
      // init degree for pre colored nodes
      for (i32 i = r0; i <= r3; i++) {
        MachineOperand op = {.state = MachineOperand::PreColored, .value = i};
        // very large
        degree[op] = 0x40000000;
      }

      // procedure AddEdge(u, v)
      auto add_edge = [&](MachineOperand u, MachineOperand v) {
        if (adj_set.find({u, v}) == adj_set.end() && u != v) {
          auto interference = std::string(u) + " <-> " + std::string(v);
          dbg(interference);
          adj_set.insert({u, v});
          adj_set.insert({v, u});
          if (!u.is_precolored()) {
            adj_list[u].insert(v);
            degree[u]++;
          }
          if (!v.is_precolored()) {
            adj_list[v].insert(u);
            degree[v]++;
          }
        }
      };

      // procedure Build()
      auto build = [&]() {
        // build interference graph
        for (auto bb = f->bb.tail; bb; bb = bb->prev) {
          // calculate live set before each instruction
          auto live = bb->liveout;
          for (auto inst = bb->insts.tail; inst; inst = inst->prev) {
            auto [def, use] = get_def_use(inst);
            if (auto x = dyn_cast<MIMove>(inst)) {
              if (x->dst.needs_color() && x->rhs.needs_color()) {
                live.erase(x->rhs);
                move_list[x->rhs].insert(x);
                move_list[x->dst].insert(x);
                worklist_moves.insert(x);
              }
            }

            for (auto &d : def) {
              if (d.needs_color()) {
                live.insert(d);
              }
            }

            for (auto &d : def) {
              if (d.needs_color()) {
                for (auto &l : live) {
                  add_edge(l, d);
                }
              }
            }

            for (auto &d : def) {
              if (d.needs_color()) {
                live.erase(d);
              }
            }

            for (auto &u : use) {
              if (u.needs_color()) {
                live.insert(u);
              }
            }
          }
        }
      };

      auto adjacent = [&](MachineOperand n) {
        std::set<MachineOperand> res = adj_list[n];
        for (auto it = res.begin(); it != res.end();) {
          if (std::find(select_stack.begin(), select_stack.end(), *it) == select_stack.end() &&
              std::find(coalesced_nodes.begin(), coalesced_nodes.end(), *it) == coalesced_nodes.end()) {
            it++;
          } else {
            it = res.erase(it);
          }
        }
        return res;
      };

      auto node_moves = [&](MachineOperand n) {
        std::set<MIMove *> res = move_list[n];
        for (auto it = res.begin(); it != res.end();) {
          if (active_moves.find(*it) == active_moves.end() && worklist_moves.find(*it) == worklist_moves.end()) {
            it = res.erase(it);
          } else {
            it++;
          }
        }
        return res;
      };

      auto move_related = [&](MachineOperand n) { return node_moves(n).size() > 0; };

      auto mk_worklist = [&]() {
        for (i32 i = 0; i < f->virtual_max; i++) {
          // initial
          MachineOperand vreg = {.state = MachineOperand::Virtual, .value = i};
          if (degree[vreg] >= k) {
            spill_worklist.insert(vreg);
          } else if (move_related(vreg)) {
            freeze_worklist.insert(vreg);
          } else {
            simplify_worklist.insert(vreg);
          }
        }
      };

      // EnableMoves({m} u Adjacent(m))
      auto enable_moves = [&](MachineOperand n) {
        for (auto m : node_moves(n)) {
          if (active_moves.find(m) != active_moves.end()) {
            active_moves.erase(m);
            worklist_moves.insert(m);
          }
        }

        for (auto a : adjacent(n)) {
          for (auto m : node_moves(a)) {
            if (active_moves.find(m) != active_moves.end()) {
              active_moves.erase(m);
              worklist_moves.insert(m);
            }
          }
        }
      };

      auto decrement_degree = [&](MachineOperand m) {
        auto d = degree[m];
        degree[m] = d - 1;
        if (d == k) {
          enable_moves(m);
          spill_worklist.insert(m);
          if (move_related(m)) {
            freeze_worklist.insert(m);
          } else {
            simplify_worklist.insert(m);
          }
        }
      };

      auto simplify = [&]() {
        auto it = simplify_worklist.begin();
        auto n = *it;
        simplify_worklist.erase(it);
        select_stack.push_back(n);
        for (auto &m : adjacent(n)) {
          decrement_degree(m);
        }
      };

      // procedure GetAlias(n)
      auto get_alias = [&](MachineOperand n) -> MachineOperand {
        while (std::find(coalesced_nodes.begin(), coalesced_nodes.end(), n) != coalesced_nodes.end()) {
          n = alias[n];
        }
        return n;
      };

      // procedure AddWorkList(n)
      auto add_work_list = [&](MachineOperand u) {
        if (!u.is_precolored() && !move_related(u) && degree[u] < k) {
          freeze_worklist.erase(u);
          simplify_worklist.insert(u);
        }
      };

      auto ok = [&](MachineOperand t, MachineOperand r) {
        return degree[t] < k || t.is_precolored() || adj_set.find({t, r}) != adj_set.end();
      };

      auto adj_ok = [&](MachineOperand v, MachineOperand u) {
        for (auto t : adjacent(v)) {
          if (!ok(t, u)) {
            return false;
          }
        }
        return true;
      };

      // procedure Combine(u, v)
      auto combine = [&](MachineOperand u, MachineOperand v) {
        auto it = freeze_worklist.find(v);
        if (it != freeze_worklist.end()) {
          freeze_worklist.erase(it);
        } else {
          spill_worklist.erase(v);
        }

        coalesced_nodes.insert(v);
        alias[v] = u;
        // NOTE: nodeMoves should be moveList
        auto &m = move_list[u];
        for (auto n : move_list[v]) {
          m.insert(n);
        }
        for (auto t : adjacent(v)) {
          add_edge(t, u);
          decrement_degree(t);
        }

        if (degree[u] >= k && freeze_worklist.find(u) != freeze_worklist.end()) {
          freeze_worklist.erase(u);
          spill_worklist.insert(u);
        }
      };

      auto conservative = [&](std::set<MachineOperand> adj_u, std::set<MachineOperand> adj_v) {
        int count = 0;
        // set union
        for (auto n : adj_v) {
          adj_u.insert(n);
        }
        for (auto n : adj_u) {
          if (degree[n] >= k) {
            count++;
          }
        }

        return count < k;
      };

      // procedure Coalesce()
      auto coalesce = [&]() {
        auto m = *worklist_moves.begin();
        auto u = get_alias(m->dst);
        auto v = get_alias(m->rhs);
        // swap when needed
        if (v.is_precolored()) {
          auto temp = u;
          u = v;
          v = temp;
        }
        worklist_moves.erase(m);

        if (u == v) {
          coalesced_moves.insert(m);
          add_work_list(u);
        } else if (v.is_precolored() || adj_set.find({u, v}) != adj_set.end()) {
          constrained_moves.insert(m);
          add_work_list(u);
          add_work_list(v);
        } else if (u.is_precolored() && adj_ok(v, u) || !u.is_precolored() && conservative(adjacent(u), adjacent(v))) {
          coalesced_moves.insert(m);
          combine(u, v);
          add_work_list(u);
        } else {
          active_moves.insert(m);
        }
      };
      // procedure FreezeMoves(u)
      auto freeze_moves = [&](MachineOperand u) {
        for (auto m : node_moves(u)) {
          if (active_moves.find(m) != active_moves.end()) {
            active_moves.erase(m);
          } else {
            worklist_moves.erase(m);
          }
          frozen_moves.insert(m);

          auto v = m->dst == u ? m->rhs : m->dst;
          if (!move_related(v) && degree[v] < k) {
            freeze_worklist.erase(v);
            simplify_worklist.insert(v);
          }
        }
      };

      // procedure Freeze()
      auto freeze = [&]() {
        auto u = *freeze_worklist.begin();
        freeze_worklist.erase(u);
        simplify_worklist.insert(u);
        freeze_moves(u);
      };

      // procedure SelectSpill()
      auto select_spill = [&]() {
        // TODO: heuristic
        auto m = *spill_worklist.begin();
        spill_worklist.erase(m);
        simplify_worklist.insert(m);
        freeze_moves(m);
      };

      // procedure AssignColors()
      auto assign_colors = [&]() {
        // mapping from virtual register to its allocated register
        std::map<MachineOperand, MachineOperand> colored;
        while (!select_stack.empty()) {
          auto n = select_stack.back();
          select_stack.pop_back();
          std::set<i32> ok_colors;
          for (int i = 0; i < k; i++) {
            ok_colors.insert(i);
          }

          for (auto w : adj_list[n]) {
            auto a = get_alias(w);
            if (a.state == MachineOperand::Allocated || a.state == MachineOperand::PreColored) {
              ok_colors.erase(a.value);
            } else if (a.state == MachineOperand::Virtual) {
              auto it = colored.find(a);
              if (it != colored.end()) {
                ok_colors.erase(it->second.value);
              }
            }
          }

          if (ok_colors.empty()) {
            spilled_nodes.insert(n);
          } else {
            auto color = *ok_colors.begin();
            colored[n] = MachineOperand{.state = MachineOperand::Allocated, .value = color};
          }
        }

        for (auto n : coalesced_nodes) {
          auto a = get_alias(n);
          if (a.is_precolored()) {
            colored[n] = a;
          } else {
            colored[n] = colored[a];
          }
        }

        for (auto &[before, after] : colored) {
          auto colored = std::string(before) + " => " + std::string(after);
          dbg(colored);
        }

        // replace usage of virtual registers
        for (auto bb = f->bb.head; bb; bb = bb->next) {
          for (auto inst = bb->insts.head; inst; inst = inst->next) {
            auto [def, use] = get_def_use_ptr(inst);
            if (def && colored.find(*def) != colored.end()) {
              *def = colored[*def];
            }

            for (auto &u : use) {
              if (u && colored.find(*u) != colored.end()) {
                *u = colored[*u];
              }
            }
          }
        }
      };

      build();
      mk_worklist();
      do {
        if (!simplify_worklist.empty()) {
          simplify();
        }
        if (!worklist_moves.empty()) {
          coalesce();
        }
        if (!freeze_worklist.empty()) {
          freeze();
        }
        if (!spill_worklist.empty()) {
          select_spill();
        }
      } while (!simplify_worklist.empty() || !worklist_moves.empty() || !freeze_worklist.empty() ||
               !spill_worklist.empty());
      assign_colors();
      if (spilled_nodes.empty()) {
        done = true;
      } else {
        ERR_EXIT(CODEGEN_ERROR, "Spilling not implemented", spilled_nodes);
        done = false;
      }
    }
  }
}

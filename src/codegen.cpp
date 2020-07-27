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

    // calculate the offset in array indexing expr arr[a][b][c]
    // it supports both complete (element) and incomplete (subarray) indexing
    // e.g. for int a[2][3], it could calculate the offset of a, a[x] and a[x][y]
    auto calculate_offset = [&](MachineBB *mbb, const std::vector<Use> &access_dims,
                                const std::vector<Expr *> &var_dims) -> MachineOperand {
      // direct access
      if (access_dims.empty()) return MachineOperand{.state = MachineOperand::Immediate, .value = 0};

      // sum of index * number of elements
      // allocate two registers: multiply and add
      i32 mul_vreg = virtual_max++;
      i32 add_vreg = virtual_max++;

      // mov add_vreg, 0
      auto mv_inst = new MIMove(mbb);
      mv_inst->dst = MachineOperand{.state = MachineOperand::Virtual, .value = add_vreg};
      mv_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, .value = 0};

      // TODO: eliminate MUL when sub_arr_size == 1
      for (int i = 0; i < access_dims.size(); i++) {
        // number of elements
        i32 sub_arr_size = i + 1 < var_dims.size() ? var_dims[i + 1]->result : 1;

        // mov mul_vreg, sub_arr_size
        auto mv_inst = new MIMove(mbb);
        mv_inst->dst = MachineOperand{.state = MachineOperand::Virtual, .value = mul_vreg};
        mv_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, .value = sub_arr_size};

        // mul mul_vreg, mul_vreg, dim
        auto mul_inst = new MIBinary(MachineInst::Mul, mbb);
        mul_inst->dst = MachineOperand{.state = MachineOperand::Virtual, .value = mul_vreg};
        mul_inst->lhs = MachineOperand{.state = MachineOperand::Virtual, .value = mul_vreg};
        mul_inst->rhs = resolve(access_dims[i].value);

        // add add_vreg, add_vreg, mul_vreg
        auto add_inst = new MIBinary(MachineInst::Add, mbb);
        add_inst->dst = MachineOperand{.state = MachineOperand::Virtual, .value = add_vreg};
        add_inst->lhs = MachineOperand{.state = MachineOperand::Virtual, .value = add_vreg};
        add_inst->rhs = MachineOperand{.state = MachineOperand::Virtual, .value = mul_vreg};
      }

      return MachineOperand{.state = MachineOperand::Virtual, .value = add_vreg};
    };

    // 2. translate instructions except phi
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      auto mbb = bb_map[bb];
      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        if (auto x = dyn_cast<JumpInst>(inst)) {
          auto new_inst = new MIJump(bb_map[x->next], mbb);
          mbb->control_transter_inst = new_inst;
        } else if (auto x = dyn_cast<AccessInst>(inst)) {
          // store or load, nearly all the same
          auto arr = resolve(x->arr.value);
          auto offset = calculate_offset(mbb, x->dims, x->lhs_sym->dims);
          // resolve index access
          if (x->dims.size() == x->lhs_sym->dims.size()) {
            // access to element
            MIAccess *access_inst;
            if (auto x = dyn_cast<StoreInst>(inst)) {
              // store to element
              auto data = resolve_no_imm(x->data.value, mbb);  // must get data first
              access_inst = new MIStore(mbb);
              static_cast<MIStore *>(access_inst)->data = data;
            } else {
              // load from element
              access_inst = new MILoad(mbb);
              static_cast<MILoad *>(access_inst)->dst = resolve(inst);
            }
            access_inst->addr = arr;
            access_inst->offset = offset;
            access_inst->shift = 2;
          } else {
            // access to subarray
            assert(x->tag != Value::Store);  // you could only store to a specific element
            // addr <- &arr
            MachineOperand addr = {.state = MachineOperand::Virtual, .value = virtual_max++};
            auto mv_inst = new MIMove(mbb);
            mv_inst->dst = addr;
            mv_inst->rhs = resolve(x->arr.value);
            // offset <- offset * 4
            auto mul_inst = new MIBinary(MachineInst::Tag::Mul, mbb);
            mul_inst->dst = offset;
            mul_inst->lhs = offset;
            mul_inst->rhs = MachineOperand{.state = MachineOperand::Immediate, .value = 4};
            // inst <- addr + offset
            auto add_inst = new MIBinary(MachineInst::Tag::Add, mbb);
            add_inst->dst = resolve(inst);
            add_inst->lhs = addr;
            add_inst->rhs = offset;
          }
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

    mf->virtual_max = virtual_max;
  }
  return ret;
}

std::pair<std::optional<MachineOperand>, std::vector<MachineOperand>> get_def_use(MachineInst *inst) {
  std::optional<MachineOperand> def = std::nullopt;
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
    // TODO
  } else if (auto x = dyn_cast<MIGlobal>(inst)) {
    def = {x->dst};
  } else if (auto x = dyn_cast<MIReturn>(inst)) {
    MachineOperand ret = MachineOperand{.state = MachineOperand::PreColored, .value = ArmReg::r0};
    use = {ret};
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
      if (def && def->needs_color() && bb->liveuse.find(*def) == bb->liveuse.end()) {
        bb->def.insert(*def);
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
  for (auto f = p->func.head; f; f = f->next) {
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

    auto add_edge = [&](MachineOperand u, MachineOperand v) {
      if (adj_set.find({u, v}) == adj_set.end() && u != v) {
        dbg(std::string(u) + " interfere with " + std::string(v));
        adj_set.insert({u, v});
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
              move_list[*def].insert(x);
              move_list[x->dst].insert(x);
              worklist_moves.insert(x);
            }
          }

          if (def && def->needs_color()) {
            live.insert(*def);
            for (auto &l : live) {
              add_edge(l, *def);
            }

            live.erase(*def);
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
          it = res.erase(it);
        } else {
          it++;
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

    auto get_alias = [&](MachineOperand n) -> MachineOperand {
      while (std::find(coalesced_nodes.begin(), coalesced_nodes.end(), n) != coalesced_nodes.end()) {
        n = alias[n];
      }
      return n;
    };

    auto add_work_list = [&](MachineOperand u) {
      if (!u.is_precolored() && !move_related(u) && degree[u] < k) {
        auto it = std::find(freeze_worklist.begin(), freeze_worklist.end(), u);
        if (it != freeze_worklist.end()) {
          freeze_worklist.erase(it);
        }
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

    auto combine = [&](MachineOperand u, MachineOperand v) {
      auto it = freeze_worklist.find(v);
      if (it != freeze_worklist.end()) {
        freeze_worklist.erase(it);
      } else {
        spill_worklist.erase(v);
      }

      coalesced_nodes.insert(v);
      alias[v] = u;
      // NOTE: what's nodeMoves?
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
      for (auto n : adj_u) {
        if (degree[n] >= k) {
          count ++;
        }
      }
      for (auto n : adj_v) {
        if (degree[n] >= k) {
          count ++;
        }
      }

      return count < k;
    };

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
      } else if (v.is_precolored() && adj_set.find({u, v}) != adj_set.end()) {
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
        // freeze();
      }
      if (!spill_worklist.empty()) {
        // select_spill();
      }
    } while (!simplify_worklist.empty() || !worklist_moves.empty() || !freeze_worklist.empty() ||
             !spill_worklist.empty());
  }
}

#include "allocate_register.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>

#include "../ir/cfg.hpp"

std::pair<std::vector<MachineOperand>, std::vector<MachineOperand>> get_def_use(MachineInst *inst) {
  std::vector<MachineOperand> def;
  std::vector<MachineOperand> use;

  if (auto x = dyn_cast<MIBinary>(inst)) {
    def = {x->dst};
    use = {x->lhs, x->rhs};
  } else if (auto x = dyn_cast<MILongMul>(inst)) {
    def = {x->dst};
    use = {x->lhs, x->rhs};
  } else if (auto x = dyn_cast<MIFma>(inst)) {
    def = {x->dst};
    use = {x->dst, x->lhs, x->rhs, x->acc};
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
    for (u32 i = (u32)ArmReg::r0; i < (u32)ArmReg::r0 + std::min(x->func->params.size(), (size_t)4); ++i) {
      use.push_back(MachineOperand::R((ArmReg)i));
    }
    for (u32 i = (u32)ArmReg::r0; i <= (u32)ArmReg::r3; i++) {
      def.push_back(MachineOperand::R((ArmReg)i));
    }
    def.push_back(MachineOperand::R(ArmReg::lr));
    def.push_back(MachineOperand::R(ArmReg::ip));
  } else if (auto x = dyn_cast<MIGlobal>(inst)) {
    def = {x->dst};
  } else if (isa<MIReturn>(inst)) {
    // ret
    use.push_back(MachineOperand::R(ArmReg::r0));
  }
  return {def, use};
}
std::pair<MachineOperand *, std::vector<MachineOperand *>> get_def_use_ptr(MachineInst *inst) {
  MachineOperand *def = nullptr;
  std::vector<MachineOperand *> use;

  if (auto x = dyn_cast<MIBinary>(inst)) {
    def = &x->dst;
    use = {&x->lhs, &x->rhs};
  } else if (auto x = dyn_cast<MILongMul>(inst)) {
    def = &x->dst;
    use = {&x->lhs, &x->rhs};
  } else if (auto x = dyn_cast<MIFma>(inst)) {
    def = {&x->dst};
    use = {&x->dst, &x->lhs, &x->rhs, &x->acc};
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
  } else if (isa<MICall>(inst)) {
    // intentionally blank
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
void allocate_register(MachineProgram *p) {
  for (auto f = p->func.head; f; f = f->next) {
    auto loop_info = compute_loop_info(f->func);
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
      std::map<MachineOperand, std::set<MIMove *, MIMoveCompare>> move_list;
      std::set<MachineOperand> simplify_worklist;
      std::set<MachineOperand> freeze_worklist;
      std::set<MachineOperand> spill_worklist;
      std::set<MachineOperand> spilled_nodes;
      std::set<MachineOperand> coalesced_nodes;
      std::vector<MachineOperand> colored_nodes;
      std::vector<MachineOperand> select_stack;
      std::set<MIMove *, MIMoveCompare> coalesced_moves;
      std::set<MIMove *, MIMoveCompare> constrained_moves;
      std::set<MIMove *, MIMoveCompare> frozen_moves;
      std::set<MIMove *, MIMoveCompare> worklist_moves;
      std::set<MIMove *, MIMoveCompare> active_moves;
      // for heuristic
      std::map<MachineOperand, u32> loop_cnt;

      // allocatable registers: r0 to r11, r12(ip), lr
      constexpr u32 k = (u32)ArmReg::r12 - (u32)ArmReg::r0 + 1 + 1;
      // init degree for pre colored nodes
      for (u32 i = (u32)ArmReg::r0; i <= (u32)ArmReg::lr; i++) {
        auto op = MachineOperand::R((ArmReg)i);
        // very large
        degree[op] = 0x40000000;
      }

      // procedure AddEdge(u, v)
      auto add_edge = [&](MachineOperand u, MachineOperand v) {
        if (adj_set.find({u, v}) == adj_set.end() && u != v) {
          if (debug_mode) {
            auto interference = std::string(u) + " <-> " + std::string(v);
            dbg(interference);
          }
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
              if (x->dst.needs_color() && x->rhs.needs_color() && x->is_simple()) {
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
                loop_cnt[d] += loop_info.depth_of(bb->bb);
              }
            }

            for (auto &u : use) {
              if (u.needs_color()) {
                live.insert(u);
                loop_cnt[u] += loop_info.depth_of(bb->bb);
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
        std::set<MIMove *, MIMoveCompare> res = move_list[n];
        for (auto it = res.begin(); it != res.end();) {
          if (active_moves.find(*it) == active_moves.end() && worklist_moves.find(*it) == worklist_moves.end()) {
            it = res.erase(it);
          } else {
            it++;
          }
        }
        return res;
      };

      auto move_related = [&](MachineOperand n) { return !node_moves(n).empty(); };

      auto mk_worklist = [&]() {
        for (u32 i = 0; i < f->virtual_max; i++) {
          // initial
          auto vreg = MachineOperand::V(i);
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
        u32 count = 0;
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
        } else if ((u.is_precolored() && adj_ok(v, u)) || (!u.is_precolored() && conservative(adjacent(u), adjacent(v)))) {
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
        MachineOperand m{};
        // select node with max degree (heuristic)
        m = *std::max_element(spill_worklist.begin(), spill_worklist.end(), [&](auto a, auto b) {
          return float(degree[a]) / pow(2, loop_cnt[a]) < float(degree[b]) / pow(2, loop_cnt[b]);
        });
        simplify_worklist.insert(m);
        freeze_moves(m);
        spill_worklist.erase(m);
      };

      // procedure AssignColors()
      auto assign_colors = [&]() {
        // mapping from virtual register to its allocated register
        std::map<MachineOperand, MachineOperand> colored;
        while (!select_stack.empty()) {
          auto n = select_stack.back();
          select_stack.pop_back();
          std::set<i32> ok_colors;
          for (u32 i = 0; i < k - 1; i++) {
            ok_colors.insert(i);
          }
          ok_colors.insert((i32)ArmReg::lr);

          for (auto w : adj_list[n]) {
            auto a = get_alias(w);
            if (a.state == MachineOperand::State::Allocated || a.is_precolored()) {
              ok_colors.erase(a.value);
            } else if (a.state == MachineOperand::State::Virtual) {
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
            colored[n] = MachineOperand{MachineOperand::State::Allocated, color};
          }
        }

        // for testing, might not needed
        if (!spilled_nodes.empty()) {
          return;
        }

        for (auto n : coalesced_nodes) {
          auto a = get_alias(n);
          if (a.is_precolored()) {
            colored[n] = a;
          } else {
            colored[n] = colored[a];
          }
        }

        if (debug_mode) {
          for (auto &[before, after] : colored) {
            auto colored = std::string(before) + " => " + std::string(after);
            dbg(colored);
          }
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
        for (auto &n : spilled_nodes) {
          auto spill = "Spilling v" + std::to_string(n.value) + " with loop count of " + std::to_string(loop_cnt[n]);
          dbg(spill);
          // allocate on stack
          for (auto bb = f->bb.head; bb; bb = bb->next) {
            auto offset = f->stack_size;
            auto offset_imm = MachineOperand::I(offset);

            auto generate_access_offset = [&](MIAccess *access_inst) {
              if (offset < (1u << 12u)) {  // ldr / str has only imm12
                access_inst->offset = offset_imm;
              } else {
                auto mv_inst = new MIMove(access_inst);  // insert before access
                mv_inst->rhs = offset_imm;
                mv_inst->dst = MachineOperand::V(f->virtual_max++);
                access_inst->offset = mv_inst->dst;
              }
            };

            for (auto orig_inst = bb->insts.head; orig_inst; orig_inst = orig_inst->next) {
              auto [def, use] = get_def_use_ptr(orig_inst);
              if (def && *def == n) {
                // store
                // allocate new vreg
                i32 vreg = f->virtual_max++;
                def->value = vreg;
                auto store_inst = new MIStore();
                store_inst->bb = bb;
                store_inst->addr = MachineOperand::R(ArmReg::sp);
                store_inst->shift = 0;
                bb->insts.insertAfter(store_inst, orig_inst);
                generate_access_offset(store_inst);
                store_inst->data = MachineOperand::V(vreg);
                //new MIComment("spill store", store_inst);
              }

              for (auto &u : use) {
                if (*u == n) {
                  // ldr ip, [sp, #imm / ip]
                  // use ip as source for use
                  i32 vreg = f->virtual_max++;
                  u->value = vreg;
                  auto load_inst = new MILoad(orig_inst);
                  load_inst->bb = bb;
                  load_inst->addr = MachineOperand::R(ArmReg::sp);
                  load_inst->shift = 0;
                  generate_access_offset(load_inst);
                  load_inst->dst = MachineOperand::V(vreg);
                  //new MIComment("spill load", load_inst);
                }
              }
            }
          }
          f->stack_size += 4;  // increase stack size
        }
        done = false;
      }
    }
  }
}
#include "scheduling.hpp"

#include <queue>

// virtual operand that represents condition register
const MachineOperand COND = MachineOperand{.state = MachineOperand::State::PreColored, .value = 0x40000000};

std::pair<std::vector<MachineOperand>, std::vector<MachineOperand>> get_def_use_scheduling(MachineInst *inst) {
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
    if (x->cond != ArmCond::Any) {
      use.push_back(COND);
    }
  } else if (auto x = dyn_cast<MILoad>(inst)) {
    def = {x->dst};
    use = {x->addr, x->offset};
  } else if (auto x = dyn_cast<MIStore>(inst)) {
    use = {x->data, x->addr, x->offset};
  } else if (auto x = dyn_cast<MICompare>(inst)) {
    def = {COND};
    use = {x->lhs, x->rhs};
  } else if (auto x = dyn_cast<MIBranch>(inst)) {
    if (x->cond != ArmCond::Any) {
      use = {COND};
    }
  } else if (auto x = dyn_cast<MICall>(inst)) {
    // args (also caller save)
    for (u32 i = (u32)ArmReg::r0; i < (u32)ArmReg::r0 + std::min(x->func->params.size(), (size_t)4); ++i) {
      use.push_back(MachineOperand::R((ArmReg)i));
    }
    use.push_back(MachineOperand::R(ArmReg::sp));
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

enum class CortexA72FUKind { Branch, Integer, IntegerMultiple, Load, Store };

// reference: Cortex-A72 software optimization guide
std::pair<u32, CortexA72FUKind> get_info(MachineInst *inst) {
  // TODO: check inst->tag
  if (auto x = dyn_cast<MIBinary>(inst)) {
    if (x->tag == MachineInst::Tag::Mul) {
      return {3, CortexA72FUKind::IntegerMultiple};
    } else if (x->tag == MachineInst::Tag::Div) {
      // 4~12
      return {8, CortexA72FUKind::IntegerMultiple};
    } else {
      if (x->shift.shift == 0) {
        // no shift
        return {1, CortexA72FUKind::Integer};
      } else {
        return {2, CortexA72FUKind::IntegerMultiple};
      }
    }
  } else if (auto x = dyn_cast<MILongMul>(inst)) {
    return {3, CortexA72FUKind::IntegerMultiple};
  } else if (auto x = dyn_cast<MIFma>(inst)) {
    return {4, CortexA72FUKind::IntegerMultiple};
  } else if (auto x = dyn_cast<MIMove>(inst)) {
    // TODO: handle movw/movt case
    if (x->cond == ArmCond::Any) {
      return {1, CortexA72FUKind::Integer};
    } else {
      return {2, CortexA72FUKind::Integer};
    }
  } else if (auto x = dyn_cast<MILoad>(inst)) {
    return {4, CortexA72FUKind::Load};
  } else if (auto x = dyn_cast<MIStore>(inst)) {
    return {3, CortexA72FUKind::Store};
  } else if (auto x = dyn_cast<MICompare>(inst)) {
    return {1, CortexA72FUKind::Integer};
  } else if (auto x = dyn_cast<MICall>(inst)) {
    return {1, CortexA72FUKind::Branch};
  } else if (auto x = dyn_cast<MIGlobal>(inst)) {
    return {1, CortexA72FUKind::Integer};
  } else if (isa<MIReturn>(inst)) {
    return {1, CortexA72FUKind::Branch};
  } else if (isa<MIBranch>(inst)) {
    return {1, CortexA72FUKind::Branch};
  } else if (isa<MIJump>(inst)) {
    return {1, CortexA72FUKind::Branch};
  }
  UNREACHABLE();
}

struct Node {
  MachineInst *inst;
  u32 priority;
  u32 latency;
  CortexA72FUKind kind;
  u32 temp;
  std::set<Node *> out_edges;
  std::set<Node *> in_edges;

  Node(MachineInst *inst) : inst(inst), priority(0) {
    auto [l, k] = get_info(inst);
    latency = l;
    kind = k;
  }
};

struct CortexA72FU {
  CortexA72FUKind kind;
  Node *inflight = nullptr;
  u32 complete_cycle = 0;
};

struct NodeCompare {
  bool operator()(Node *const &lhs, const Node *const &rhs) const {
    if (lhs->priority != rhs->priority) return lhs->priority > rhs->priority;
    if (lhs->latency != rhs->latency) return lhs->latency > rhs->latency;
    return false;
  }
};

void instruction_schedule(MachineFunc *f) {
  for (auto bb = f->bb.head; bb; bb = bb->next) {
    // create data dependence graph of instructions
    // instructions that read this register
    std::map<u32, std::vector<Node *>> read_insts;
    // instruction that writes this register
    std::map<u32, Node *> write_insts;
    // instruction that might have side effect
    Node *side_effect = nullptr;
    std::vector<Node *> nodes;

    // calculate data dependence graph
    for (auto inst = bb->insts.head; inst; inst = inst->next) {
      if (isa<MIComment>(inst)) {
        continue;
      }
      auto [def, use] = get_def_use_scheduling(inst);
      auto node = new Node(inst);
      nodes.push_back(node);
      for (auto &u : use) {
        if (u.is_reg()) {
          // add edges for read-after-write
          if (auto &w = write_insts[u.value]) {
            w->out_edges.insert(node);
            node->in_edges.insert(w);
          }
        }
      }

      for (auto &d : def) {
        if (d.is_reg()) {
          // add edges for write-after-read
          for (auto &r : read_insts[d.value]) {
            r->out_edges.insert(node);
            node->in_edges.insert(r);
          }
          // add edges for write-after-write
          if (auto &w = write_insts[d.value]) {
            w->out_edges.insert(node);
            node->in_edges.insert(w);
          }
        }
      }

      for (auto &u : use) {
        if (u.is_reg()) {
          // update read_insts
          read_insts[u.value].push_back(node);
        }
      }

      for (auto &d : def) {
        if (d.is_reg()) {
          // update read_insts and write_insts
          read_insts[d.value].clear();
          write_insts[d.value] = node;
        }
      }

      // don't schedule instructions with side effect
      if (side_effect) {
        side_effect->out_edges.insert(node);
        node->in_edges.insert(side_effect);
      }
      if (isa<MILoad>(inst) || isa<MIStore>(inst) || isa<MICall>(inst)) {
        side_effect = node;
      }

      // should be put at the end of bb
      if (isa<MIBranch>(inst) || isa<MIJump>(inst) || isa<MIReturn>(inst)) {
        for (auto &n : nodes) {
          if (n != node) {
            n->out_edges.insert(node);
            node->in_edges.insert(n);
          }
        }
      }
    }

    // calculate priority
    // temp is out_degree in this part
    std::vector<Node *> vis;
    for (auto &n : nodes) {
      n->temp = n->out_edges.size();
      if (n->out_edges.empty()) {
        vis.push_back(n);
        n->priority = n->latency;
      }
    }
    while (!vis.empty()) {
      Node *n = vis.back();
      vis.pop_back();
      for (auto &t : n->in_edges) {
        t->priority = std::max(t->priority, t->latency + n->priority);
        t->temp--;
        if (t->temp == 0) {
          vis.push_back(t);
        }
      }
    }

    // functional units
    // see cortex a72 software optimisation
    CortexA72FU fu[] = {
        {.kind = CortexA72FUKind::Branch},  {.kind = CortexA72FUKind::Integer},
        {.kind = CortexA72FUKind::Integer}, {.kind = CortexA72FUKind::IntegerMultiple},
        {.kind = CortexA72FUKind::Load},    {.kind = CortexA72FUKind::Store},
    };
    u32 num_inflight = 0;

    // schedule
    // removes instructions
    bb->control_transfer_inst = nullptr;
    bb->insts.head = bb->insts.tail = nullptr;
    // ready list
    std::vector<Node *> ready;
    // temp is in_degree in this part
    for (auto &n : nodes) {
      n->temp = n->in_edges.size();
      if (n->in_edges.empty()) {
        ready.push_back(n);
      }
    }

    u32 cycle = 0;
    while (!ready.empty() || num_inflight > 0) {
      std::sort(ready.begin(), ready.end(), NodeCompare{});
      for (int i = 0; i < ready.size();) {
        auto inst = ready[i];
        auto kind = inst->kind;
        bool fired = false;
        for (auto &f : fu) {
          if (f.kind == kind && f.inflight == nullptr) {
            // fire!
            dbg(inst->inst->tag);
            bb->insts.insertAtEnd(inst->inst);
            num_inflight++;
            f.inflight = inst;
            f.complete_cycle = cycle + inst->latency;
            ready.erase(ready.begin() + i);
            fired = true;
            break;
          }
        }

        if (!fired) {
          i++;
        }
      }

      cycle++;
      for (auto &f : fu) {
        if (f.complete_cycle == cycle && f.inflight) {
          // finish
          // put nodes to ready
          for (auto &t : f.inflight->out_edges) {
            t->temp--;
            if (t->temp == 0) {
              ready.push_back(t);
            }
          }
          f.inflight = nullptr;
          num_inflight--;
        }
      }
    }
  }
}
#include "machine_code.hpp"

#include <functional>
#include <iomanip>

std::ostream &operator<<(std::ostream &os, const MachineProgram &p) {
  using std::endl;
  static const std::string BB_PREFIX = ".L_BB_";
  IndexMapper<MachineBB> bb_index;

  // count instructions to solve constant pool problem
  auto pool_count = 0;
  auto inst_count = 0;
  auto insert_pool = [&](bool insert_jump = false) {
    inst_count = 0;
    auto pool_num = pool_count++;
    auto pool_name = "_POOL_" + std::to_string(pool_num++);
    auto sec_name = ".L" + pool_name;
    auto after_sec_name = ".L_AFTER" + pool_name;
    if (insert_jump) {
      os << "\t"
         << "b"
         << "\t" << after_sec_name << " @ forcibly insert constant pool" << endl;
    }
    os << sec_name << ":" << endl;
    os << "\t"
       << ".pool" << endl;
    os << after_sec_name << ":" << endl;
    return sec_name;
  };
  auto increase_count = [&](int count = 1) {
    inst_count += count;
    if (int old_count = inst_count; inst_count > 1000) {
      // force insert a constant pool, slow but necessary
      auto sec_name = insert_pool(true);
      auto force_pool =
          "forcibly insert constant pool " + sec_name + ", instruction count: " + std::to_string(old_count);
      dbg(force_pool);
    }
  };

  // print BB name
  auto pb = [&](MachineBB *bb) {
    os << BB_PREFIX << bb_index.get(bb);
    return "";
  };

  // emit stack movement code
  auto move_stack = [&](bool enter, i32 offset, auto &&output, const std::string &prefix = "") {
    auto cmd = enter ? "sub" : "add";
    auto imm_operand = MachineOperand::I(offset);
    if (can_encode_imm(-offset)) {
      // wow, your stack can really dance
      cmd = enter ? "add" : "sub";
      imm_operand.value = -imm_operand.value;
    }
    if (can_encode_imm(offset) || can_encode_imm(-offset)) {
      os << cmd << "\t"
         << "sp, sp, " << imm_operand << endl;
    } else {
      auto mv_to_r4 = new MIMove{nullptr, 0};
      mv_to_r4->rhs = MachineOperand::I(offset);
      mv_to_r4->dst = MachineOperand::R(ArmReg::r4);
      output(mv_to_r4, nullptr, nullptr, true);
      os << prefix << cmd << "\t"
         << "sp, sp, " << MachineOperand::R(ArmReg::r4) << endl;
    }
  };

  auto print_reg_list = [](std::ostream& os, MachineFunc *f) {
    for (auto r : f->used_callee_saved_regs) {
      os << "r" << int(r) << ", ";
    }
  };

  std::function<void(MachineInst *, MachineFunc *, MachineBB *, bool)> output_instruction =
      [&](MachineInst *inst, MachineFunc *f, MachineBB *bb, bool indent) {
        if (bb && inst == bb->control_transfer_inst) {
          os << "@ control transfer" << endl;
        }
        if (indent) {
          os << "\t";
        }
        if (auto x = dyn_cast<MIJump>(inst)) {
          os << "b"
             << "\t" << pb(x->target) << endl;
          insert_pool();
          increase_count();
        } else if (auto x = dyn_cast<MIBranch>(inst)) {
          os << "b" << x->cond << "\t" << pb(x->target) << endl;
          increase_count();
        } else if (auto x = dyn_cast<MIAccess>(inst)) {
          MachineOperand data{};
          std::string inst_name;
          if (auto x = dyn_cast<MILoad>(inst)) {
            data = x->dst;
            inst_name = "ldr";
          } else if (auto x = dyn_cast<MIStore>(inst)) {
            data = x->data;
            inst_name = "str";
          }
          if (x->offset.is_imm()) {
            i32 offset = x->offset.value << x->shift;
            os << inst_name << "\t" << data << ", [" << x->addr << ", #" << offset << "]" << endl;
          } else {
            os << inst_name << "\t" << data << ", [" << x->addr << ", " << x->offset << ", LSL #" << x->shift << "]"
               << endl;
          }
          increase_count();
        } else if (auto x = dyn_cast<MIGlobal>(inst)) {
          os << "ldr"
             << "\t" << x->dst << ", =" << x->sym->name << endl;
          increase_count();
        } else if (auto x = dyn_cast<MIBinary>(inst)) {
          const char *op = "unknown";
          if (x->tag == MachineInst::Tag::Mul) {
            op = "mul";
          } else if (x->tag == MachineInst::Tag::Add) {
            op = "add";
          } else if (x->tag == MachineInst::Tag::Sub) {
            op = "sub";
          } else if (x->tag == MachineInst::Tag::Rsb) {
            op = "rsb";
          } else if (x->tag == MachineInst::Tag::Div) {
            op = "sdiv";
          } else if (x->tag == MachineInst::Tag::And) {
            op = "and";
          } else if (x->tag == MachineInst::Tag::Or) {
            op = "orr";
          } else {
            UNREACHABLE();
          }
          os << op << "\t" << x->dst << ", " << x->lhs << ", " << x->rhs;
          if (x->shift.type != ArmShift::None) {
            assert(x->tag == MachineInst::Tag::Add && x->shift.type == ArmShift::Lsl);  // currently we only use this
            os << ", " << x->shift;
          }
          os << endl;
          increase_count();
        } else if (auto x = dyn_cast<MILongMul>(inst)) {
          os << "umull"
             << "\t" << x->dst_lo << ", " << x->dst_hi << ", " << x->lhs << ", " << x->rhs << endl;
        } else if (auto x = dyn_cast<MIFma>(inst)) {
          os << "mla"
             << "\t" << x->dst << ", " << x->lhs << ", " << x->rhs << ", " << x->acc << endl;
        } else if (auto x = dyn_cast<MICompare>(inst)) {
          os << "cmp"
             << "\t" << x->lhs << ", " << x->rhs << endl;
          increase_count();
        } else if (auto x = dyn_cast<MIMove>(inst)) {
          // limit of ARM immediate number, see
          // https://stackoverflow.com/questions/10261300/invalid-constant-after-fixup
          if (x->rhs.is_imm() && !can_encode_imm(x->rhs.value)) {
            using std::to_string;
            // split into high & low 16 bits
            u32 imm = x->rhs.value;
            u32 low_bits = imm & 0xffffu;
            u32 high_bits = imm >> 16u;
            auto low_operand = MachineOperand::I((i32)low_bits);
            auto high_operand = MachineOperand::I((i32)high_bits);
            // debug output
            auto move_split = "Immediate number " + to_string((i32)imm) + " in MIMove split to " +
                              to_string(high_bits) + " and " + to_string(low_bits);
            dbg(move_split);
            // output asm
            os << "@ original imm: " << (i32)imm << endl;
            os << std::hex;
            os << "\t"
               << "movw"
               << "\t" << x->dst << ", " << low_operand << " @ 0x" << low_bits << endl;
            increase_count();
            if (high_bits != 0) {
              os << "\t"
                 << "movt"
                 << "\t" << x->dst << ", " << high_operand << " @ 0x" << high_bits << endl;
              increase_count();
            }
            os << std::dec;
          } else {
            os << "mov" << x->cond << "\t" << x->dst << ", " << x->rhs;
            if (x->shift.type != ArmShift::None) {
              const char *op = "unknown";
              if (x->shift.type == ArmShift::Lsl) {
                op = "lsl";
              } else if (x->shift.type == ArmShift::Lsr) {
                op = "lsr";
              } else {
                UNREACHABLE();
              }
              os << ", " << op << " #" << x->shift.shift;
            }
            os << endl;
            increase_count();
          }
        } else if (auto x = dyn_cast<MIReturn>(inst)) {
          // function epilogue
          // restore registers and pc from stack
          // increase sp
          if (f->stack_size) {
            move_stack(false, f->stack_size, output_instruction, "\t");
          }
          os << "ldmfd\tsp!, {";
          print_reg_list(os, f);
          os << "pc}" << endl;
          insert_pool();
          increase_count(2);
        } else if (auto x = dyn_cast<MICall>(inst)) {
          os << "blx\t" << x->func->name << endl;
          increase_count();
        } else if (auto x = dyn_cast<MIComment>(inst)) {
          os << "@ " << x->content << endl;
        } else {
          UNREACHABLE();
        }
      };

  // code section
  os << ".arch armv7ve" << endl;
  os << ".section .text" << endl;
  for (auto f = p.func.head; f; f = f->next) {
    // generate symbol for function
    os << endl << ".global " << f->func->func->name << endl;
    os << "\t"
       << ".type"
       << "\t" << f->func->func->name << ", %function" << endl;
    os << f->func->func->name << ":" << endl;

    // function prologue
    os << "\tstmfd\tsp!, {";
    print_reg_list(os, f);
    os << "lr}" << endl;
    // move sp down
    if (f->stack_size) {
      move_stack(true, f->stack_size, output_instruction, "\t");
    }
    increase_count(3);

    // generate code for each BB
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      os << pb(bb) << ":" << endl;
      os << "@ pred:";
      for (auto &pred : bb->pred) {
        os << " " << pb(pred);
      }
      os << ", succ:";
      for (auto &succ : bb->succ) {
        if (succ) {
          os << " " << pb(succ);
        }
      }
      os << ", livein:";
      for (auto &op : bb->livein) {
        os << " " << op;
      }
      os << ", liveout:";
      for (auto &op : bb->liveout) {
        os << " " << op;
      }
      os << ", liveuse:";
      for (auto &use : bb->liveuse) {
        os << " " << use;
      }
      os << ", def:";
      for (auto &def : bb->def) {
        os << " " << def;
      }
      os << endl;

      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        output_instruction(inst, f, bb, true);
      }
    }
  }
  // reference to libsysy to avoid optimization
  os << "\tblx getint" << endl;

  // data section
  os << endl << endl << ".section .data" << endl;
  os << ".align 4" << endl;
  for (auto &decl : p.glob_decl) {
    os << endl << ".global " << decl->name << endl;
    os << "\t"
       << ".type"
       << "\t" << decl->name << ", %object" << endl;
    os << decl->name << ":" << endl;

    // output values
    int count = 0;
    bool initialized = false;
    i32 last = 0;

    auto print_values = [&]() {
      using std::hex;
      using std::dec;
      if (count > 1) {
        os << "\t"
           << ".fill"
           << "\t" << count << ", 4, " << last << endl;
      } else {
        os << "\t"
           << ".long"
           << "\t" << last << endl;
      }
    };

    for (auto expr : decl->flatten_init) {
      if (!initialized) {
        initialized = true;
        last = expr->result;
      }
      if (expr->result == last) {
        ++count;
      } else {
        print_values();
        last = expr->result;
        count = 1;
      }
    }
    print_values();
  }
  return os;
}

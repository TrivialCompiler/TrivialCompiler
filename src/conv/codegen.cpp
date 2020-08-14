#include "codegen.hpp"

#include <algorithm>
#include <cassert>
#include <map>
#include <optional>

// list of assignments (lhs, rhs)
using ParMv = std::vector<std::pair<MachineOperand, MachineOperand>>;
using u64 = uint64_t;

static inline void insert_parallel_mv(ParMv &movs, MachineInst *insertBefore) {
  // serialization in any order is okay
  for (auto &[lhs, rhs] : movs) {
    auto inst = new MIMove(insertBefore);
    inst->dst = lhs;
    inst->rhs = rhs;
  }
}

// resolve imm as instruction operand
// ARM has limitations, see https://stackoverflow.com/questions/10261300/invalid-constant-after-fixup
static MachineOperand generate_imm_operand(i32 imm, MachineBB *mbb, bool force_reg, int &current_virtual_max) {
  auto operand = MachineOperand::I(imm);
  if (!force_reg && can_encode_imm(imm)) {
    // directly encoded in instruction as imm
    return operand;
  } else {
    auto imm_split = "Immediate number " + std::to_string(imm) + " cannot be encoded, converting to MIMove";
    dbg(imm_split);
    // use MIMove, which automatically splits if necessary
    auto vreg = MachineOperand::V(current_virtual_max++);
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
}

MachineProgram *machine_code_generation(IrProgram *p) {
  auto ret = new MachineProgram;
  ret->glob_decl = p->glob_decl;
  for (auto f = p->func.head; f; f = f->next) {
    if (f->builtin) continue;
    auto mf = new MachineFunc;
    ret->func.insertAtEnd(mf);
    mf->func = f;

    // 1. create machine bb 1-to-1
    std::map<BasicBlock *, MachineBB *> bb_map;
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      auto mbb = new MachineBB;
      mbb->bb = bb;
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
    auto new_virtual_reg = [&]() { return MachineOperand::V(virtual_max++); };

    auto get_imm_operand = [&](i32 imm, MachineBB *mbb) { return generate_imm_operand(imm, mbb, false, virtual_max); };

    // resolve value reference
    auto resolve = [&](Value *value, MachineBB *mbb) {
      if (auto x = dyn_cast<ParamRef>(value)) {
        auto it = param_map.find(x->decl);
        if (it == param_map.end()) {
          // allocate virtual reg
          auto res = new_virtual_reg();
          val_map[value] = res;
          param_map[x->decl] = res;
          for (u32 i = 0; i < f->func->params.size(); i++) {
            if (&f->func->params[i] == x->decl) {
              if (i < 4) {
                // r0-r3
                // copy param to vreg in entry bb
                auto new_inst = new MIMove(mf->bb.head, 0);
                new_inst->rhs = MachineOperand::R((ArmReg)i);
                new_inst->dst = res;
              } else {
                // read from sp + (i-4)*4 in entry bb
                // will be fixed up in later pass
                auto vreg = new_virtual_reg();
                auto new_inst = new MILoad(mf->bb.head, 0);
                new_inst->addr = MachineOperand::R(ArmReg::sp);
                new_inst->offset = vreg;
                new_inst->dst = res;
                new_inst->shift = 0;
                auto mv_inst = new MIMove(mf->bb.head, 0);
                mv_inst->dst = vreg;
                mv_inst->rhs = MachineOperand::I((i - 4) * 4);
                mf->sp_arg_fixup.push_back(mv_inst);
              }
              break;
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
        mv_inst->rhs = MachineOperand::I(y->imm);
        return res;
      } else {
        return resolve(value, mbb);
      }
    };

    std::map<Value *, std::pair<MachineInst *, ArmCond>> cond_map;

    // 2. translate instructions except phi
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      auto mbb = bb_map[bb];
      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        if (auto x = dyn_cast<JumpInst>(inst)) {
          auto new_inst = new MIJump(bb_map[x->next], mbb);
          mbb->control_transfer_inst = new_inst;
        } else if (auto x = dyn_cast<LoadInst>(inst)) {
          auto arr = resolve(x->arr.value, mbb);
          auto index = resolve(x->index.value, mbb);
          auto load_inst = new MILoad(mbb);
          load_inst->dst = resolve(inst, mbb);
          load_inst->addr = arr;
          load_inst->offset = index;
          load_inst->shift = 2;
        } else if (auto x = dyn_cast<StoreInst>(inst)) {
          auto arr = resolve(x->arr.value, mbb);
          auto data = resolve_no_imm(x->data.value, mbb);
          auto index = resolve(x->index.value, mbb);
          auto store_inst = new MIStore(mbb);
          store_inst->addr = arr;
          store_inst->offset = index;
          store_inst->data = data;
          store_inst->shift = 2;
        } else if (auto x = dyn_cast<GetElementPtrInst>(inst)) {
          // dst = getelementptr arr, index, multiplier
          auto dst = resolve(inst, mbb);
          auto arr = resolve(x->arr.value, mbb);
          auto mult = x->multiplier * 4;
          auto y = dyn_cast<ConstValue>(x->index.value);

          new MIComment("begin getelementptr " + std::string(x->lhs_sym->name), mbb);
          if (mult == 0 || (y && y->imm == 0)) {
            // dst <- arr
            auto move_inst = new MIMove(mbb);
            move_inst->dst = dst;
            move_inst->rhs = arr;
            dbg("offset 0 eliminated in getelementptr");
          } else if (y) {
            // dst <- arr + result
            auto off = mult * y->imm;
            auto imm_operand = get_imm_operand(off, mbb);
            auto add_inst = new MIBinary(MachineInst::Tag::Add, mbb);
            add_inst->dst = dst;
            add_inst->lhs = arr;
            add_inst->rhs = imm_operand;
            auto offset_const = "offset calculated to constant " + std::to_string(off) + " in getelementptr";
            dbg(offset_const);
          } else if ((mult & (mult - 1)) == 0) {
            // dst <- arr + index << log(mult)
            auto index = resolve(x->index.value, mbb);
            auto add_inst = new MIBinary(MachineInst::Tag::Add, mbb);
            add_inst->dst = dst;
            add_inst->lhs = arr;
            add_inst->rhs = index;
            add_inst->shift.type = ArmShift::Lsl;
            add_inst->shift.shift = __builtin_ctz(mult);
            auto fuse_mul_add = "MUL " + std::to_string(mult) + " and ADD fused into shifted ADD in getelementptr";
            dbg(fuse_mul_add);
          } else {
            // dst <- arr
            auto index = resolve_no_imm(x->index.value, mbb);
            auto move_inst = new MIMove(mbb);
            move_inst->dst = dst;
            move_inst->rhs = arr;
            // mult <- mult (imm)
            auto move_mult = new MIMove(mbb);
            move_mult->dst = new_virtual_reg();
            move_mult->rhs = MachineOperand::I(mult);
            // dst <- index * mult + dst
            auto fma_inst = new MIFma(true, false, mbb);
            fma_inst->dst = dst;
            fma_inst->lhs = index;
            fma_inst->rhs = move_mult->dst;
            fma_inst->acc = dst;
            dbg("MUL and ADD fused into SMLAL");
          }
          new MIComment("end getelementptr", mbb);
        } else if (auto x = dyn_cast<ReturnInst>(inst)) {
          if (x->ret.value) {
            auto val = resolve(x->ret.value, mbb);
            // move val to a0
            auto mv_inst = new MIMove(mbb);
            mv_inst->dst = MachineOperand::R(ArmReg::r0);
            mv_inst->rhs = val;
            new MIReturn(mbb);
            mbb->control_transfer_inst = mv_inst;
          } else {
            auto new_inst = new MIReturn(mbb);
            mbb->control_transfer_inst = new_inst;
          }
        } else if (auto x = dyn_cast<BinaryInst>(inst)) {
          MachineOperand rhs{};
          auto rhs_const = x->rhs.value->tag == Value::Tag::Const;
          auto imm = static_cast<ConstValue *>(x->rhs.value)->imm;
          assert(!(x->lhs.value->tag == Value::Tag::Const && rhs_const));  // should be optimized out

          auto lhs = resolve_no_imm(x->lhs.value, mbb);
          // Optimization 2:
          // 提前检查两个特殊情况：除常数和乘2^n，里面用continue来跳过后续的操作
          if (rhs_const) {
            if (x->tag == Value::Tag::Div && imm > 0) {
              auto dst = resolve(inst, mbb);
              u32 d = static_cast<ConstValue *>(x->rhs.value)->imm;
              u32 s = __builtin_ctz(d);
              if (d == (u32(1) << s)) {  // d是2的幂次，转化成移位
                if (false) {
                  // handle negative dividend
                  auto i1 = new MIMove(mbb);
                  i1->dst = new_virtual_reg();
                  i1->rhs = lhs;
                  i1->shift.shift = 31;
                  i1->shift.type = ArmShift::Asr;
                  auto i2 = new MIBinary(MachineInst::Tag::Add, mbb);
                  i2->dst = new_virtual_reg();
                  i2->lhs = lhs;
                  i2->rhs = i1->dst;
                  i2->shift.shift = 32 - s;
                  i2->shift.type = ArmShift::Lsr;
                  auto i3 = new MIMove(mbb);
                  i3->dst = dst;
                  i3->rhs = i2->dst;
                  i3->shift.shift = s;
                  i3->shift.type = ArmShift::Asr;
                } else {
                  auto new_inst = new MIMove(mbb);
                  new_inst->dst = dst;
                  new_inst->rhs = lhs;
                  if (s > 0) {
                    new_inst->shift.shift = s;
                    new_inst->shift.type = ArmShift::Lsr;
                  }
                }
              } else if (d == 1000000007) {
                dbg("gg");
                // magic constant used in bitset, try disable optimization
                auto move = new MIMove(mbb);
                move->rhs = MachineOperand::I(d);
                move->dst = new_virtual_reg();
                auto div = new MIBinary(MachineInst::Tag::Div, mbb);
                div->lhs = lhs;
                div->rhs = move->dst;
                div->dst = dst;
              } else {
                const u32 W = 32;
                u64 n_c = (1 << (W - 1)) - ((1 << (W - 1)) % d) - 1;
                u64 p = W;
                while (((u64)1 << p) <= n_c * (d - ((u64)1 << p) % d)) {
                  p++;
                }
                u32 m = (((u64)1 << p) + (u64)d - ((u64)1 << p) % d) / (u64)d;
                u32 shift = p - W;
                auto i0 = new MIMove(mbb);
                i0->dst = new_virtual_reg();
                i0->rhs = MachineOperand::I(m);
                auto temp_dst = new_virtual_reg();
                if (m >= 0x80000000) {
                  auto i1 = new MIFma(true, true, mbb);
                  i1->dst = temp_dst;
                  i1->lhs = lhs;
                  i1->rhs = i0->dst;
                  i1->acc = lhs;
                } else {
                  auto i1 = new MILongMul(mbb);
                  i1->dst = temp_dst;
                  i1->lhs = lhs;
                  i1->rhs = i0->dst;
                }
                auto i2 = new MIMove(mbb);
                i2->dst = new_virtual_reg();
                i2->rhs = temp_dst;
                i2->shift.shift = shift;
                i2->shift.type = ArmShift::Asr;
                auto i3 = new MIBinary(MachineInst::Tag::Add, mbb);
                i3->dst = dst;
                i3->lhs = i2->dst;
                i3->rhs = lhs;
                i3->shift.shift = 31;
                i3->shift.type = ArmShift::Lsr;
              }
              continue;
            }
            if (x->tag == Value::Tag::Mul) {
              u32 log = 31 - __builtin_clz(imm);
              if (imm == (1 << log)) {
                auto dst = resolve(inst, mbb);
                auto new_inst = new MIMove(mbb);
                new_inst->dst = dst;
                new_inst->rhs = lhs;
                if (log > 0) {
                  new_inst->shift.shift = log;
                  new_inst->shift.type = ArmShift::Lsl;
                }
                continue;
              }
            }
          }
          // try to use imm
          if (x->rhsCanBeImm() && rhs_const) {
            // Optimization 1:
            // try to use negative imm to reduce instructions
            if (x->tag == Value::Tag::Add || x->tag == Value::Tag::Sub) {
              // add r0, #-40 == sub r0, #40
              // the former will be splitted into instructions (movw, movt, add), the latter is only one
              if (!can_encode_imm(imm) && can_encode_imm(-imm)) {
                auto negative_imm = "Imm " + std::to_string(imm) + " can be encoded in negative form";
                dbg(negative_imm);
                imm = -imm;
                x->tag = x->tag == Value::Tag::Add ? Value::Tag::Sub : Value::Tag::Add;
              }
            }
            rhs = get_imm_operand(imm, mbb);  // might be imm or register
          } else {
            rhs = resolve_no_imm(x->rhs.value, mbb);
          }
          // Optimization 3:
          // Fused Multiply-Add / Sub
          // NOTE_OPT: this is not correct if the final result is negative and wider than 32 bits (64 -> 32 truncation)
          if (x->tag == Value::Tag::Mul && x->uses.head == x->uses.tail) {
            // only one user, lhs and rhs are not consts
            // match pattern:
            // %x2 = mul %x1, %x0
            // %x4 = add / sub %x3, %x2
            // becomes
            // v4 = v3
            // mla / mls v4, v1, v0
            auto y = dyn_cast<BinaryInst>(x->next);
            if (y && (y->tag == Value::Tag::Add || y->tag == Value::Tag::Sub) && y->rhs.value == x) {
              dbg("Multiply-Add/Sub fused to MLA/MLS");
              auto x3 = resolve(y->lhs.value, mbb);
              auto x4 = resolve(y, mbb);
              // x4 <- x3
              auto move_inst = new MIMove(mbb);
              move_inst->dst = x4;
              move_inst->rhs = x3;
              // x4 <- x4 +/- x1 * x0
              auto fma_inst = new MIFma(y->tag == Value::Tag::Add, false, mbb);
              fma_inst->dst = x4;
              fma_inst->acc = x4;
              fma_inst->lhs = lhs;
              fma_inst->rhs = rhs;
              // skip y
              inst = inst->next;
              continue;
            }
          }

          if (x->tag == Value::Tag::Mod) {
            UNREACHABLE();  // should be replaced
          } else if (Value::Tag::Lt <= x->tag && x->tag <= Value::Tag::Ne) {
            // transform compare instructions
            auto dst = resolve(inst, mbb);
            auto new_inst = new MICompare(mbb);
            new_inst->lhs = lhs;
            new_inst->rhs = rhs;

            ArmCond cond, opposite;
            if (x->tag == Value::Tag::Gt) {
              cond = ArmCond::Gt;
            } else if (x->tag == Value::Tag::Ge) {
              cond = ArmCond::Ge;
            } else if (x->tag == Value::Tag::Le) {
              cond = ArmCond::Le;
            } else if (x->tag == Value::Tag::Lt) {
              cond = ArmCond::Lt;
            } else if (x->tag == Value::Tag::Eq) {
              cond = ArmCond::Eq;
            } else if (x->tag == Value::Tag::Ne) {
              cond = ArmCond::Ne;
            } else {
              UNREACHABLE();
            }
            opposite = opposite_cond(cond);

            // 一条BinaryInst后紧接着BranchInst，而且前者的结果仅被后者使用，那么就可以不用计算结果，而是直接用bxx的指令
            if (x->uses.head == x->uses.tail && x->uses.head && isa<BranchInst>(x->uses.head->user) &&
                x->next == x->uses.head->user) {
              dbg("Binary comparison inst not computing result");
              cond_map.insert({x, {new_inst, cond}});
            } else {
              auto mv1_inst = new MIMove(mbb);
              mv1_inst->dst = dst;
              mv1_inst->cond = cond;
              mv1_inst->rhs = get_imm_operand(1, mbb);
              auto mv0_inst = new MIMove(mbb);
              mv0_inst->dst = dst;
              mv0_inst->cond = opposite;
              mv0_inst->rhs = get_imm_operand(0, mbb);
            }
          } else if (Value::Tag::And <= x->tag && x->tag <= Value::Tag::Or) {
            // lhs && rhs:
            // cmp lhs, #0
            // movne v1, #1
            // moveq v1, #0
            // cmp rhs, #0
            // movne v2, #1
            // moveq v2, #0
            // and/or dst, v1, v2

            // lhs
            auto cmp_lhs_inst = new MICompare(mbb);
            cmp_lhs_inst->lhs = lhs;
            cmp_lhs_inst->rhs = get_imm_operand(0, mbb);
            auto lhs_vreg = new_virtual_reg();
            auto mv1_lhs_inst = new MIMove(mbb);
            mv1_lhs_inst->dst = lhs_vreg;
            mv1_lhs_inst->cond = ArmCond::Ne;
            mv1_lhs_inst->rhs = get_imm_operand(1, mbb);
            auto mv0_lhs_inst = new MIMove(mbb);
            mv0_lhs_inst->dst = lhs_vreg;
            mv0_lhs_inst->cond = ArmCond::Eq;
            mv0_lhs_inst->rhs = get_imm_operand(0, mbb);

            // rhs
            auto cmp_rhs_inst = new MICompare(mbb);
            cmp_rhs_inst->lhs = rhs;
            cmp_rhs_inst->rhs = get_imm_operand(0, mbb);
            auto rhs_vreg = new_virtual_reg();
            auto mv1_rhs_inst = new MIMove(mbb);
            mv1_rhs_inst->dst = rhs_vreg;
            mv1_rhs_inst->cond = ArmCond::Ne;
            mv1_rhs_inst->rhs = get_imm_operand(1, mbb);
            auto mv0_rhs_inst = new MIMove(mbb);
            mv0_rhs_inst->dst = rhs_vreg;
            mv0_rhs_inst->cond = ArmCond::Eq;
            mv0_rhs_inst->rhs = get_imm_operand(0, mbb);

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
          ArmCond c;
          if (auto it = cond_map.find(x->cond.value); it != cond_map.end()) {
            dbg("Branch uses flags registers instead of using comparison");
            mbb->control_transfer_inst = it->second.first;
            c = it->second.second;
          } else {
            auto cond = resolve_no_imm(x->cond.value, mbb);
            auto cmp_inst = new MICompare(mbb);
            cmp_inst->lhs = cond;
            cmp_inst->rhs = get_imm_operand(0, mbb);
            mbb->control_transfer_inst = cmp_inst;
            c = ArmCond::Ne;
          }
          auto new_inst = new MIBranch(mbb);
          if (x->left == bb->next) {
            new_inst->cond = opposite_cond(c);
            new_inst->target = bb_map[x->right];
            new MIJump(bb_map[x->left], mbb);
          } else {
            new_inst->cond = c;
            new_inst->target = bb_map[x->left];
            new MIJump(bb_map[x->right], mbb);
          }
        } else if (auto x = dyn_cast<CallInst>(inst)) {
          std::vector<MachineOperand> params;
          int n = x->func->func->params.size();
          for (int i = 0; i < n; i++) {
            if (i < 4) {
              // move args to r0-r3
              auto rhs = resolve(x->args[i].value, mbb);
              auto mv_inst = new MIMove(mbb);
              mv_inst->dst = MachineOperand::R((ArmReg)i);
              mv_inst->rhs = rhs;
            } else {
              // store to sp-(n-i)*4
              auto rhs = resolve_no_imm(x->args[i].value, mbb);
              auto st_inst = new MIStore(mbb);
              st_inst->addr = MachineOperand::R(ArmReg::sp);
              st_inst->offset = MachineOperand::I(-(n - i));
              st_inst->shift = 2;
              st_inst->data = rhs;
            }
          }

          if (n > 4) {
            // sub sp, sp, (n-4)*4
            auto add_inst = new MIBinary(MachineInst::Tag::Sub, mbb);
            add_inst->dst = MachineOperand::R(ArmReg::sp);
            add_inst->lhs = MachineOperand::R(ArmReg::sp);
            add_inst->rhs = MachineOperand::I(4 * (n - 4));
          }

          auto new_inst = new MICall(mbb);
          new_inst->func = x->func->func;

          if (n > 4) {
            // add sp, sp, (n-4)*4
            auto add_inst = new MIBinary(MachineInst::Tag::Add, mbb);
            add_inst->dst = MachineOperand::R(ArmReg::sp);
            add_inst->lhs = MachineOperand::R(ArmReg::sp);
            add_inst->rhs = MachineOperand::I(4 * (n - 4));
          }

          // return
          if (x->func->func->is_int) {
            // has return
            // move r0 to dst
            auto dst = resolve(inst, mbb);
            auto mv_inst = new MIMove(mbb);
            mv_inst->dst = dst;
            mv_inst->rhs = MachineOperand::R(ArmReg::r0);
          }
        } else if (auto x = dyn_cast<AllocaInst>(inst)) {
          i32 size = 1;
          if (!x->sym->dims.empty()) {
            size = x->sym->dims[0]->result;
          }
          size *= 4;
          auto dst = resolve(inst, mbb);
          auto offset = get_imm_operand(mf->stack_size, mbb);
          auto add_inst = new MIBinary(MachineInst::Tag::Add, mbb);
          add_inst->dst = dst;
          add_inst->lhs = MachineOperand::R(ArmReg::sp);
          add_inst->rhs = offset;
          // allocate size on sp
          mf->stack_size += size;
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
          for (u32 i = 0; i < x->incoming_values.size(); i++) {
            auto pred_bb = x->incoming_bbs()[i];
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

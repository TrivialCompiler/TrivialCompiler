#include "codegen.hpp"

#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <algorithm>

#include "machine_code.hpp"

// list of assignments (lhs, rhs)
using ParMv = std::vector<std::pair<MachineOperand, MachineOperand>>;
using u64 = uint64_t;

void insert_parallel_mv(ParMv &movs, MachineInst *insertBefore) {
  // serialization in any order is okay
  for (auto &[lhs, rhs] : movs) {
    auto inst = new MIMove(insertBefore);
    inst->dst = lhs;
    inst->rhs = rhs;
  }
}

std::pair<u64, u32> choose_multiplier(u32 d, u32 p) {
  const u32 N = 32;
  u32 l = N - __builtin_clz(d - 1);
  u64 lo = (u64(1) << (N + l)) / d;
  u64 hi = ((u64(1) << (N + l)) + (u64(1) << (N + l - p))) / d;
  while ((lo >> 1) < (hi >> 1) && l > 0) {
    lo >>= 1;
    hi >>= 1;
    --l;
  }
  return {hi, l};
}

// resolve imm as instruction operand
// ARM has limitations, see https://stackoverflow.com/questions/10261300/invalid-constant-after-fixup
MachineOperand generate_imm_operand(i32 imm, MachineBB *mbb, bool force_reg, int &current_virtual_max) {
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
};

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
    auto new_virtual_reg = [&]() { return MachineOperand::V(virtual_max++); };

    auto get_imm_operand = [&](i32 imm, MachineBB *mbb) {
      return generate_imm_operand(imm, mbb, false, virtual_max);
    };

    // resolve value reference
    auto resolve = [&](Value *value, MachineBB *mbb) {
      if (auto x = dyn_cast<ParamRef>(value)) {
        auto it = param_map.find(x->decl);
        if (it == param_map.end()) {
          // allocate virtual reg
          auto res = new_virtual_reg();
          val_map[value] = res;
          param_map[x->decl] = res;
          for (int i = 0; i < f->func->params.size(); i++) {
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
            auto fma_inst = new MIFma(mbb);
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
            auto new_inst = new MIReturn(mbb);
            mbb->control_transfer_inst = mv_inst;
          } else {
            auto new_inst = new MIReturn(mbb);
            mbb->control_transfer_inst = new_inst;
          }
        } else if (auto x = dyn_cast<BinaryInst>(inst)) {
          MachineOperand rhs{};
          auto lhs_const = x->lhs.value->tag == Value::Tag::Const;
          auto rhs_const = x->rhs.value->tag == Value::Tag::Const;
          auto imm = static_cast<ConstValue *>(x->rhs.value)->imm;
          assert(!(lhs_const && rhs_const));  // should be optimized out

          auto lhs = resolve_no_imm(x->lhs.value, mbb);
          // Optimization 2:
          // 提前检查两个特殊情况：除常数和乘2^n，里面用continue来跳过后续的操作
          if (rhs_const) {
            if (x->tag == Value::Tag::Div && imm > 0) {
              // fixme: this is an unsafe optimization, because it assumes lhs > 0 for correctness
              const u32 N = 32;
              auto dst = resolve(inst, mbb);
              u32 d = static_cast<ConstValue *>(x->rhs.value)->imm;
              if (d >= (u32(1) << (N - 1))) {  // >= 2^31，转化成l >= d
                auto new_inst = new MICompare(mbb);
                new_inst->lhs = lhs;
                new_inst->rhs = rhs;
                auto mv1_inst = new MIMove(mbb);
                mv1_inst->dst = dst;
                mv1_inst->cond = ArmCond::Ge;
                mv1_inst->rhs = get_imm_operand(1, mbb);
                auto mv0_inst = new MIMove(mbb);
                mv0_inst->dst = dst;
                mv0_inst->cond = ArmCond::Lt;
                mv0_inst->rhs = get_imm_operand(0, mbb);
              } else {
                u32 s = __builtin_ctz(d);
                if (d == (u32(1) << s)) {  // d是2的幂次，转化成移位
                  auto new_inst = new MIMove(mbb);
                  new_inst->dst = dst;
                  new_inst->rhs = lhs;
                  if (s > 0) {
                    new_inst->shift.shift = s;
                    new_inst->shift.type = ArmShift::Lsr;
                  }
                } else {
                  auto mul_shift = choose_multiplier(d, N);
                  if (mul_shift.first < (u64(1) << N))
                    s = 0;
                  else
                    mul_shift = choose_multiplier(d >> s, N - s);
                  if (mul_shift.first < (u64(1) << N)) {
                    MachineOperand mul_lhs = lhs;
                    if (s > 0) {
                      auto i = new MIMove(mbb);
                      mul_lhs = i->dst = new_virtual_reg();
                      i->rhs = lhs;
                      i->shift.shift = s;
                      i->shift.type = ArmShift::Lsr;
                    }
                    auto i0 = new MIMove(mbb);
                    i0->dst = new_virtual_reg();
                    i0->rhs = MachineOperand::I(mul_shift.first);
                    auto i1 = new MILongMul(mbb);
                    i1->dst_hi = mul_shift.second > 0 ? new_virtual_reg() : dst;
                    i1->lhs = mul_lhs;
                    i1->rhs = i0->dst;
                    if (mul_shift.second > 0) {
                      auto i = new MIMove(mbb);
                      i->dst = dst;
                      i->rhs = i1->dst_hi;
                      i->shift.shift = mul_shift.second;
                      i->shift.type = ArmShift::Lsr;
                    }
                  } else {
                    auto i0 = new MIMove(mbb);
                    i0->dst = new_virtual_reg();
                    i0->rhs = MachineOperand::I(mul_shift.first - (u64(1) << N));
                    auto i1 = new MILongMul(mbb);
                    i1->dst_hi = new_virtual_reg();
                    i1->lhs = lhs;
                    i1->rhs = i0->dst;
                    auto i2 = new MIBinary(MachineInst::Tag::Sub, mbb);
                    i2->dst = new_virtual_reg();
                    i2->lhs = lhs;
                    i2->rhs = i1->dst_hi;
                    auto i3 = new MIMove(mbb);
                    i3->dst = new_virtual_reg();
                    i3->rhs = i2->dst;
                    i3->shift.shift = 1;
                    i3->shift.type = ArmShift::Lsr;
                    auto i4 = new MIBinary(MachineInst::Tag::Add, mbb);
                    i4->dst = new_virtual_reg();
                    i4->lhs = i3->dst;
                    i4->rhs = i1->dst_hi;
                    auto i5 = new MIMove(mbb);
                    i5->dst = dst;
                    i5->rhs = i4->dst;
                    i5->shift.shift = mul_shift.second - 1;
                    i5->shift.type = ArmShift::Lsr;
                  }
                }
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
          // Fused Multiply-Add
          if (x->tag == Value::Tag::Mul && x->uses.head == x->uses.tail) {
            // only one user, lhs and rhs are not consts
            // match pattern:
            // %x2 = mul %x1, %x0
            // %x4 = add %x3, %x2
            // becomes
            // v4 = v3
            // fma v4, v1, v0
            auto y = dyn_cast<BinaryInst>(x->next);
            if (y && y->tag == Value::Tag::Add && y->rhs.value == x) {
              dbg("Found FMA location");
              auto x0 = resolve(x->lhs.value, mbb);
              auto x1 = resolve(x->rhs.value, mbb);
              auto x3 = resolve(y->lhs.value, mbb);
              auto x4 = resolve(y, mbb);
              // x4 <- x3
              auto move_inst = new MIMove(mbb);
              move_inst->dst = x4;
              move_inst->rhs = x3;
              // x4 <- x4 + x1 * x0
              auto fma_inst = new MIFma(mbb);
              fma_inst->dst = x4;
              fma_inst->acc = x4;
              fma_inst->lhs = x1;
              fma_inst->rhs = x0;
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
              opposite = ArmCond::Le;
            } else if (x->tag == Value::Tag::Ge) {
              cond = ArmCond::Ge;
              opposite = ArmCond::Lt;
            } else if (x->tag == Value::Tag::Le) {
              cond = ArmCond::Le;
              opposite = ArmCond::Gt;
            } else if (x->tag == Value::Tag::Lt) {
              cond = ArmCond::Lt;
              opposite = ArmCond::Ge;
            } else if (x->tag == Value::Tag::Eq) {
              cond = ArmCond::Eq;
              opposite = ArmCond::Ne;
            } else if (x->tag == Value::Tag::Ne) {
              cond = ArmCond::Ne;
              opposite = ArmCond::Eq;
            } else {
              UNREACHABLE();
            }

            // 一条BinaryInst后紧接着BranchInst，而且前者的结果仅被后者使用，那么就可以不用计算结果，而是直接用bxx的指令
            if (x->uses.head == x->uses.tail && x->uses.head && isa<BranchInst>(x->uses.head->user) &&
                x->next == x->uses.head->user) {
              dbg("Binary comparison inst not computing result");
              cond_map.insert({x, {new_inst, opposite}});
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
          if (auto it = cond_map.find(x->cond.value); it != cond_map.end()) {
            dbg("Branch uses flags registers instead of using comparison");
            mbb->control_transfer_inst = it->second.first;
            auto new_inst = new MIBranch(mbb);
            new_inst->cond = it->second.second;
            new_inst->target = bb_map[x->right];
            new MIJump(bb_map[x->left], mbb);
          } else {
            auto cond = resolve_no_imm(x->cond.value, mbb);
            // if cond == 0
            auto cmp_inst = new MICompare(mbb);
            cmp_inst->lhs = cond;
            cmp_inst->rhs = get_imm_operand(0, mbb);
            mbb->control_transfer_inst = cmp_inst;
            auto new_inst = new MIBranch(mbb);
            new_inst->cond = ArmCond::Eq;
            new_inst->target = bb_map[x->right];
            new MIJump(bb_map[x->left], mbb);
          }
        } else if (auto x = dyn_cast<CallInst>(inst)) {
          std::vector<MachineOperand> params;
          int n = x->func->params.size();
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
          new_inst->func = x->func;

          if (n > 4) {
            // add sp, sp, (n-4)*4
            auto add_inst = new MIBinary(MachineInst::Tag::Add, mbb);
            add_inst->dst = MachineOperand::R(ArmReg::sp);
            add_inst->lhs = MachineOperand::R(ArmReg::sp);
            add_inst->rhs = MachineOperand::I(4 * (n - 4));
          }

          // return
          if (x->func->is_int) {
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
  } else if (auto x = dyn_cast<MILongMul>(inst)) {
    def = {x->dst_hi, x->dst_lo};
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
    dbg(x->func->params.size());
    for (int i = (int)ArmReg::r0; i < (int)ArmReg::r0 + std::min(x->func->params.size(), (size_t) 4); ++i) {
      use.push_back(MachineOperand::R((ArmReg)i));
    }
    for (int i = (int)ArmReg::r0; i <= (int)ArmReg::r3; i++) {
      def.push_back(MachineOperand::R((ArmReg)i));
    }
    def.push_back(MachineOperand::R(ArmReg::lr));
    def.push_back(MachineOperand::R(ArmReg::ip));
  } else if (auto x = dyn_cast<MIGlobal>(inst)) {
    def = {x->dst};
  } else if (auto x = dyn_cast<MIReturn>(inst)) {
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
    def = &x->dst_hi;
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
  } else if (auto x = dyn_cast<MICall>(inst)) {
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
      std::unordered_map<MachineOperand, std::unordered_set<MachineOperand>> adj_list;
      // adjacent set
      std::unordered_set<std::pair<MachineOperand, MachineOperand>> adj_set;
      // other variables in the paper
      std::unordered_map<MachineOperand, u32> degree;
      std::unordered_map<MachineOperand, MachineOperand> alias;
      std::unordered_map<MachineOperand, std::set<MIMove *>> move_list;
      std::unordered_set<MachineOperand> simplify_worklist;
      std::unordered_set<MachineOperand> freeze_worklist;
      std::unordered_set<MachineOperand> spill_worklist;
      std::unordered_set<MachineOperand> spilled_nodes;
      std::unordered_set<MachineOperand> coalesced_nodes;
      std::vector<MachineOperand> colored_nodes;
      std::vector<MachineOperand> select_stack;
      std::unordered_set<MIMove *> coalesced_moves;
      std::unordered_set<MIMove *> constrained_moves;
      std::unordered_set<MIMove *> frozen_moves;
      std::unordered_set<MIMove *> worklist_moves;
      std::unordered_set<MIMove *> active_moves;

      // allocatable registers: r0 to r11, r12(ip), lr
      i32 k = (int)ArmReg::r12 - (int)ArmReg::r0 + 1 + 1;
      // init degree for pre colored nodes
      for (i32 i = (int)ArmReg::r0; i <= (int)ArmReg::r3; i++) {
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
        std::unordered_set<MachineOperand> res = adj_list[n];
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

      auto move_related = [&](MachineOperand n) { return !node_moves(n).empty(); };

      auto mk_worklist = [&]() {
        for (i32 i = 0; i < f->virtual_max; i++) {
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

      auto conservative = [&](std::unordered_set<MachineOperand> adj_u, std::unordered_set<MachineOperand> adj_v) {
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
        MachineOperand m{};
        // select node with max degree (heuristic)
        if (spill_worklist.size() > 10) {
          m = *spill_worklist.begin();
        } else {
          m = *std::max_element(spill_worklist.begin(), spill_worklist.end(), [&](auto a, auto b){
            return degree[a] < degree[b];
          });
        }
        simplify_worklist.insert(m);
        freeze_moves(m);
        spill_worklist.erase(m);
      };

      // procedure AssignColors()
      auto assign_colors = [&]() {
        // mapping from virtual register to its allocated register
        std::unordered_map<MachineOperand, MachineOperand> colored;
        while (!select_stack.empty()) {
          auto n = select_stack.back();
          select_stack.pop_back();
          std::set<i32> ok_colors;
          for (int i = 0; i < k - 1; i++) {
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
            colored[n] = MachineOperand{.state = MachineOperand::State::Allocated, .value = color};
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
          // allocate on stack
          for (auto bb = f->bb.head; bb; bb = bb->next) {
            auto offset = f->stack_size;
            auto offset_imm = MachineOperand::I(offset);

            auto generate_access_offset = [&](MIAccess *access_inst){
              if (offset < (1u << 12u)) { // ldr / str has only imm12
                access_inst->offset = offset_imm;
              } else {
                auto mv_inst = new MIMove(access_inst); // insert before access
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
                new MIComment("spill store", store_inst);
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
                  new MIComment("spill load", load_inst);
                }
              }
            }
          }
          f->stack_size += 4; // increase stack size
        }
        done = false;
      }
    }
  }
}

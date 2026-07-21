#include "codegen.hpp"

#include <algorithm>
#include <cassert>
#include <map>
#include <optional>
#include <set>

// list of assignments (lhs, rhs)
using ParMv = std::vector<std::pair<MachineOperand, MachineOperand>>;
using u64 = uint64_t;

static inline void insert_parallel_mv(ParMv &movs, MachineInst *insertBefore) {
  // serialization in any order is okay
  for (auto &[lhs, rhs] : movs) {
    if (lhs == rhs) continue;
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
                auto new_inst = new MILoad(mbb, 0);
                new_inst->addr = MachineOperand::R(ArmReg::sp);
                new_inst->offset = vreg;
                new_inst->dst = res;
                new_inst->shift = 0;
                auto mv_inst = new MIMove(new_inst);
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
//          if (glo)
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

    // Peephole: expand small zero-filling memset calls into straight-line word
    // stores.  Example: `memset(a, 0, 16)` becomes four `str zero, [a, #i]`
    // stores and avoids a runtime call.
    auto try_inline_zero_memset = [&](CallInst *call, MachineBB *mbb) {
      constexpr i32 INLINE_ZERO_MEMSET_MAX_BYTES = 128;
      if (call->func != Func::BUILTIN[8].val || call->args.size() != 3) {
        return false;
      }

      auto fill = dyn_cast<ConstValue>(call->args[1].value);
      auto count = dyn_cast<ConstValue>(call->args[2].value);
      if (!fill || !count || fill->imm != 0 || count->imm < 0 || count->imm % 4 != 0 ||
          count->imm > INLINE_ZERO_MEMSET_MAX_BYTES) {
        return false;
      }

      auto words = count->imm / 4;
      if (words == 0) {
        return true;
      }

      auto addr = resolve(call->args[0].value, mbb);
      auto zero = new_virtual_reg();
      auto zero_inst = new MIMove(mbb);
      zero_inst->dst = zero;
      zero_inst->rhs = MachineOperand::I(0);
      for (i32 i = 0; i < words; ++i) {
        auto store_inst = new MIStore(mbb);
        store_inst->addr = addr;
        store_inst->offset = MachineOperand::I(i);
        store_inst->shift = 2;
        store_inst->data = zero;
      }
      return true;
    };

    // Peephole: lower unsigned-style `x % 2^k`, or the canonical
    // `x - (x / 2^k) * 2^k`, to `x & (2^k - 1)`.  This mirrors the current
    // Div-by-pow2 lowering; signed-correct remainders need a different sequence.
    auto try_emit_pow2_remainder = [&](BinaryInst *inst, MachineBB *mbb) {
      Value *lhs = nullptr;
      ConstValue *factor = nullptr;

      if (inst->tag == Value::Tag::Mod) {
        lhs = inst->lhs.value;
        factor = dyn_cast<ConstValue>(inst->rhs.value);
      } else if (inst->tag == Value::Tag::Sub) {
        auto mul = dyn_cast<BinaryInst>(inst->rhs.value);
        if (!mul || mul->tag != Value::Tag::Mul) {
          return false;
        }

        auto div = dyn_cast<BinaryInst>(mul->lhs.value);
        factor = dyn_cast<ConstValue>(mul->rhs.value);
        if (!div || div->tag != Value::Tag::Div || !factor) {
          return false;
        }

        auto divisor = dyn_cast<ConstValue>(div->rhs.value);
        if (!divisor || divisor->imm != factor->imm || div->lhs.value != inst->lhs.value) {
          return false;
        }
        lhs = inst->lhs.value;
      } else {
        return false;
      }

      if (!factor || factor->imm <= 0) {
        return false;
      }

      auto value = static_cast<u32>(factor->imm);
      if ((value & (value - 1)) != 0) {
        return false;
      }

      auto dst = resolve(inst, mbb);
      auto src = resolve_no_imm(lhs, mbb);
      auto rhs = get_imm_operand(factor->imm - 1, mbb);
      auto and_inst = new MIBinary(MachineInst::Tag::And, mbb);
      and_inst->dst = dst;
      and_inst->lhs = src;
      and_inst->rhs = rhs;
      return true;
    };

    // Compute the multiplier/shift pair used to lower division by a positive
    // non-power-of-two constant with a multiply-high sequence.
    auto div_magic_info = [](u32 d) {
      const u32 W = 32;
      u64 sign_bit = u64{1} << (W - 1);
      u64 n_c = sign_bit - (sign_bit % d) - 1;
      u64 p = W;
      while (((u64)1 << p) <= n_c * (d - ((u64)1 << p) % d)) {
        p++;
      }
      u32 magic = static_cast<u32>((((u64)1 << p) + (u64)d - ((u64)1 << p) % d) / (u64)d);
      return std::pair{magic, static_cast<u32>(p - W)};
    };

    std::map<u32, int> div_magic_use_count;
    for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
      for (Inst *inst = bb->insts.head; inst; inst = inst->next) {
        auto bin = dyn_cast<BinaryInst>(inst);
        auto rhs = bin ? dyn_cast<ConstValue>(bin->rhs.value) : nullptr;
        if (!bin || bin->tag != Value::Tag::Div || !rhs || rhs->imm <= 0) continue;

        u32 d = rhs->imm;
        u32 s = __builtin_ctz(d);
        if (d != (u32(1) << s)) {
          div_magic_use_count[div_magic_info(d).first]++;
        }
      }
    }

    std::map<u32, MachineOperand> shared_div_magic;
    // Materialize a division magic constant locally, or hoist and share it from
    // the entry block when the same constant is used often enough.
    auto get_div_magic_operand = [&](u32 magic, MachineBB *mbb) {
      if (div_magic_use_count[magic] < 3 || mbb == mf->bb.head) {
        auto local = new MIMove(mbb);
        local->dst = new_virtual_reg();
        local->rhs = MachineOperand::I(static_cast<i32>(magic));
        return local->dst;
      }

      auto [it, inserted] = shared_div_magic.insert({magic, MachineOperand{}});
      if (inserted) {
        auto insert_before = mf->bb.head->insts.tail;
        auto materialize =
            insert_before && (isa<MIJump>(insert_before) || isa<MIBranch>(insert_before) || isa<MIReturn>(insert_before))
                ? new MIMove(insert_before)
                : new MIMove(mf->bb.head);
        materialize->dst = new_virtual_reg();
        materialize->rhs = MachineOperand::I(static_cast<i32>(magic));
        it->second = materialize->dst;
      }
      return it->second;
    };

    // Peephole: rebuild `x - (x / C) * C` when `C - 1` is a power of two as
    // `x - (q + (q << k))`, avoiding a general multiply for divisors like 3, 5,
    // 9, and 17.
    auto try_emit_shiftadd_remainder = [&](BinaryInst *mul, MachineBB *mbb, Inst *&cursor) {
      if (mul->tag != Value::Tag::Mul || mul->uses.head != mul->uses.tail) return false;

      auto sub = dyn_cast<BinaryInst>(mul->next);
      if (!sub || sub->tag != Value::Tag::Sub || sub->rhs.value != mul) return false;

      BinaryInst *div = nullptr;
      ConstValue *factor = nullptr;
      if ((div = dyn_cast<BinaryInst>(mul->lhs.value))) {
        factor = dyn_cast<ConstValue>(mul->rhs.value);
      }
      if (!factor) {
        div = dyn_cast<BinaryInst>(mul->rhs.value);
        factor = dyn_cast<ConstValue>(mul->lhs.value);
      }
      if (!div || div->tag != Value::Tag::Div || !factor || factor->imm <= 1 || div->lhs.value != sub->lhs.value) {
        return false;
      }

      auto divisor = dyn_cast<ConstValue>(div->rhs.value);
      if (!divisor || divisor->imm != factor->imm) return false;

      u32 d = static_cast<u32>(factor->imm);
      u32 shifted = d - 1;
      if ((shifted & (shifted - 1)) != 0) return false;

      auto quotient = resolve(div, mbb);
      auto dividend = resolve_no_imm(sub->lhs.value, mbb);
      auto product = new_virtual_reg();
      auto product_inst = new MIBinary(MachineInst::Tag::Add, mbb);
      product_inst->dst = product;
      product_inst->lhs = quotient;
      product_inst->rhs = quotient;
      product_inst->shift.type = ArmShift::Lsl;
      product_inst->shift.shift = __builtin_ctz(shifted);

      auto result = resolve(sub, mbb);
      auto rem_inst = new MIBinary(MachineInst::Tag::Rsb, mbb);
      rem_inst->dst = result;
      rem_inst->lhs = product;
      rem_inst->rhs = dividend;
      cursor = cursor->next;
      dbg("Rebuilt constant remainder with shifted add");
      return true;
    };

    std::map<Value *, std::pair<MachineInst *, ArmCond>> cond_map;
    std::set<Inst *> skipped_tail_returns;
    bool converted_self_tail_call = false;
    // The compare/branch peepholes below require a single use so flags can be
    // consumed directly by the nearby branch without materializing a 0/1 value.
    auto single_user = [](Value *v) -> Inst * {
      return v->uses.head && v->uses.head == v->uses.tail ? v->uses.head->user : nullptr;
    };
    auto cmp_cond = [](Value::Tag tag) {
      if (tag == Value::Tag::Gt) return ArmCond::Gt;
      if (tag == Value::Tag::Ge) return ArmCond::Ge;
      if (tag == Value::Tag::Le) return ArmCond::Le;
      if (tag == Value::Tag::Lt) return ArmCond::Lt;
      if (tag == Value::Tag::Eq) return ArmCond::Eq;
      if (tag == Value::Tag::Ne) return ArmCond::Ne;
      UNREACHABLE();
    };
    auto is_cmp = [](Value *v) {
      return v->tag >= Value::Tag::Lt && v->tag <= Value::Tag::Ne;
    };
    // Detect `(cmp1 && cmp2)` feeding one branch; codegen lowers it as two
    // conditional branches instead of building boolean temporaries.
    auto used_by_branch_and = [&](BinaryInst *cmp) {
      auto logic = dyn_cast_nullable<BinaryInst>(single_user(cmp));
      if (!logic || logic->tag != Value::Tag::And) return false;
      auto br = dyn_cast_nullable<BranchInst>(single_user(logic));
      return br && logic->next == br && is_cmp(logic->lhs.value) && is_cmp(logic->rhs.value);
    };

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
          auto arr = resolve(x->arr.value, mbb);
          auto mult = x->multiplier * 4;
          auto y = dyn_cast<ConstValue>(x->index.value);

          new MIComment("begin getelementptr " + std::string(x->lhs_sym->name), mbb);
          if (mult == 0 || (y && y->imm == 0)) {
            val_map[inst] = arr;
            dbg("offset 0 eliminated in getelementptr");
          } else if (y) {
            auto dst = resolve(inst, mbb);
            // dst <- arr + result
            auto off = mult * y->imm;
            auto op = off < 0 ? MachineInst::Tag::Sub : MachineInst::Tag::Add;
            auto imm_operand = get_imm_operand(off < 0 ? -off : off, mbb);
            auto addr_inst = new MIBinary(op, mbb);
            addr_inst->dst = dst;
            addr_inst->lhs = arr;
            addr_inst->rhs = imm_operand;
            auto offset_const = "offset calculated to constant " + std::to_string(off) + " in getelementptr";
            dbg(offset_const);
          } else if ((mult & (mult - 1)) == 0) {
            auto dst = resolve(inst, mbb);
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
            auto dst = resolve(inst, mbb);
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
          if (skipped_tail_returns.find(inst) != skipped_tail_returns.end()) continue;
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
          if (try_emit_pow2_remainder(x, mbb)) {
            continue;
          }

          MachineOperand rhs{};
          auto rhs_const = x->rhs.value->tag == Value::Tag::Const;
          auto imm = rhs_const ? static_cast<ConstValue *>(x->rhs.value)->imm : 0;
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
              } else {
                auto [m, shift] = div_magic_info(d);
                auto magic = get_div_magic_operand(m, mbb);
                auto temp_dst = new_virtual_reg();
                if (m >= 0x80000000) {
                  auto i1 = new MIFma(true, true, mbb);
                  i1->dst = temp_dst;
                  i1->lhs = lhs;
                  i1->rhs = magic;
                  i1->acc = lhs;
                } else {
                  auto i1 = new MILongMul(mbb);
                  i1->dst = temp_dst;
                  i1->lhs = lhs;
                  i1->rhs = magic;
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
          if (x->tag == Value::Tag::Mul && try_emit_shiftadd_remainder(x, mbb, inst)) {
            continue;
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
            // %x4 = add %x2, %x3 / add %x3, %x2 / sub %x3, %x2
            // becomes:
            // mla / mls v4, v1, v0, v3
            auto y = dyn_cast<BinaryInst>(x->next);
            if (y && ((y->tag == Value::Tag::Add && (y->lhs.value == x || y->rhs.value == x)) ||
                      (y->tag == Value::Tag::Sub && y->rhs.value == x))) {
              dbg("Multiply-Add/Sub fused to MLA/MLS");
              auto acc = resolve(y->lhs.value == x ? y->rhs.value : y->lhs.value, mbb);
              auto x4 = resolve(y, mbb);
              auto fma_inst = new MIFma(y->tag == Value::Tag::Add, false, mbb);
              fma_inst->dst = x4;
              fma_inst->acc = acc;
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
            if (used_by_branch_and(x)) {
              continue;
            }
            // transform compare instructions
            auto dst = resolve(inst, mbb);
            auto new_inst = new MICompare(mbb);
            new_inst->lhs = lhs;
            new_inst->rhs = rhs;

            ArmCond cond = cmp_cond(x->tag);
            ArmCond opposite = opposite_cond(cond);

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
            if (x->tag == Value::Tag::And && is_cmp(x->lhs.value) && is_cmp(x->rhs.value)) {
              auto br = dyn_cast_nullable<BranchInst>(single_user(x));
              if (br && x->next == br) {
                continue;
              }
            }
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
          // Emit one ARM compare and return the condition code that represents
          // the IR comparison result.
          auto emit_cmp = [&](BinaryInst *cmp) {
            auto cmp_inst = new MICompare(mbb);
            cmp_inst->lhs = resolve_no_imm(cmp->lhs.value, mbb);
            if (cmp->rhs.value->tag == Value::Tag::Const) {
              cmp_inst->rhs = get_imm_operand(static_cast<ConstValue *>(cmp->rhs.value)->imm, mbb);
            } else {
              cmp_inst->rhs = resolve_no_imm(cmp->rhs.value, mbb);
            }
            return std::pair(cmp_inst, cmp_cond(cmp->tag));
          };
          auto cond_and = dyn_cast<BinaryInst>(x->cond.value);
          if (cond_and && cond_and->tag == Value::Tag::And && is_cmp(cond_and->lhs.value) &&
              is_cmp(cond_and->rhs.value)) {
            auto lhs_cmp = static_cast<BinaryInst *>(cond_and->lhs.value);
            auto rhs_cmp = static_cast<BinaryInst *>(cond_and->rhs.value);
            auto [lhs_cmp_inst, lhs_cond] = emit_cmp(lhs_cmp);
            mbb->control_transfer_inst = lhs_cmp_inst;

            auto lhs_fail = new MIBranch(mbb);
            lhs_fail->cond = opposite_cond(lhs_cond);
            lhs_fail->target = bb_map[x->right];

            auto [rhs_cmp_inst, rhs_cond] = emit_cmp(rhs_cmp);
            (void)rhs_cmp_inst;
            auto rhs_branch = new MIBranch(mbb);
            if (x->left == bb->next) {
              rhs_branch->cond = opposite_cond(rhs_cond);
              rhs_branch->target = bb_map[x->right];
              new MIJump(bb_map[x->left], mbb);
            } else {
              rhs_branch->cond = rhs_cond;
              rhs_branch->target = bb_map[x->left];
              new MIJump(bb_map[x->right], mbb);
            }
            continue;
          }

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
          if (try_inline_zero_memset(x, mbb)) {
            continue;
          }

          std::vector<MachineOperand> params;
          int n = x->func->func->params.size();
          auto tail_return = dyn_cast_nullable<ReturnInst>(x->next);
          bool self_tail_call =
              x->func == f && n <= 4 && tail_return && tail_return->ret.value == x && x->uses.head == x->uses.tail;
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

          if (self_tail_call) {
            auto jump = new MIJump(mf->bb.head, mbb);
            mbb->control_transfer_inst = jump;
            mbb->succ[0] = mf->bb.head;
            mbb->succ[1] = nullptr;
            mf->bb.head->pred.push_back(mbb);
            skipped_tail_returns.insert(tail_return);
            converted_self_tail_call = true;
            dbg("Converted self tail call to jump");
            continue;
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
      // A phi move can be placed directly on a predecessor edge when that edge
      // has only this block as a real successor.
      auto single_succ_edge = [&](BasicBlock *pred) {
        auto pred_mbb = bb_map[pred];
        if (!pred_mbb->control_transfer_inst) return false;
        int succ_count = 0;
        MachineBB *succ = nullptr;
        for (auto s : pred_mbb->succ) {
          if (!s) continue;
          succ_count++;
          succ = s;
        }
        return succ_count == 1 && succ == mbb;
      };
      // Direct phi moves are safe only when no RHS still needs a vreg overwritten
      // by another move in the same edge bundle; otherwise the parallel-move
      // resolver must break cycles/copies.
      auto direct_phi_moves_are_safe = [&](const ParMv &movs) {
        std::set<MachineOperand> defs;
        for (auto [lhs, rhs] : movs) {
          if (lhs != rhs) defs.insert(lhs);
        }
        for (auto [lhs, rhs] : movs) {
          if (lhs != rhs && defs.find(rhs) != defs.end()) return false;
        }
        return true;
      };

      // collect phi node information
      // (lhs, vreg) assignments
      ParMv lhs;
      std::map<BasicBlock *, ParMv> direct_mv;
      // each bb has a list of (vreg, rhs) parallel moves
      std::map<BasicBlock *, ParMv> mv;
      bool has_phi = false;
      bool can_direct_phi = converted_self_tail_call && std::all_of(bb->pred.begin(), bb->pred.end(), single_succ_edge);
      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        // phi insts must appear at the beginning of bb
        if (auto x = dyn_cast<PhiInst>(inst)) {
          has_phi = true;
          // for each phi:
          // lhs = phi [r1 bb1], [r2 bb2] ...
          // 1. create vreg for each inst
          // 2. add parallel mv (lhs1, ...) = (vreg1, ...)
          // 3. add parallel mv in each bb: (vreg1, ...) = (r1, ...)
          auto dst = resolve(inst, mbb);
          auto vr = new_virtual_reg();
          lhs.emplace_back(dst, vr);
          for (u32 i = 0; i < x->incoming_values.size(); i++) {
            auto pred_bb = x->incoming_bbs()[i];
            auto val = resolve(x->incoming_values[i].value, bb_map[pred_bb]);
            if (can_direct_phi) {
              direct_mv[pred_bb].emplace_back(dst, val);
            }
            mv[pred_bb].emplace_back(vr, val);
          }
        } else {
          break;
        }
      }
      if (can_direct_phi) {
        for (auto &[_, movs] : direct_mv) {
          if (!direct_phi_moves_are_safe(movs)) {
            can_direct_phi = false;
            break;
          }
        }
      }
      if (has_phi && can_direct_phi) {
        for (auto &[bb, movs] : direct_mv) {
          auto mbb = bb_map[bb];
          insert_parallel_mv(movs, mbb->control_transfer_inst);
        }
        continue;
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

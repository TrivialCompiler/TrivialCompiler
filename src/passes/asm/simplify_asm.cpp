#include "simplify_asm.hpp"

#include "allocate_register.hpp"

namespace {

bool has_zero_offset(MIAccess *access) {
  return access->mode == MIAccess::Mode::Offset && access->offset == MachineOperand::I(0) && access->cond == ArmCond::Any;
}

bool can_write_back_access_base(MIAccess *access) {
  if (auto load = dyn_cast<MILoad>(access)) return !load->dst.is_equiv(access->addr);
  if (auto store = dyn_cast<MIStore>(access)) return !store->data.is_equiv(access->addr);
  return false;
}

bool contains_operand(const std::vector<MachineOperand> &operands, const MachineOperand &operand) {
  return std::find(operands.begin(), operands.end(), operand) != operands.end();
}

bool touches_operand(MachineInst *inst, const MachineOperand &operand) {
  auto [def, use] = get_def_use(inst);
  return contains_operand(def, operand) || contains_operand(use, operand);
}

bool blocks_local_scan(MachineInst *inst) {
  return isa<MIJump>(inst) || isa<MIBranch>(inst) || isa<MIReturn>(inst) || isa<MICall>(inst);
}

bool match_base_update(MIBinary *update, const MachineOperand &base, i32 &offset) {
  if (!update || update->cond != ArmCond::Any || !update->shift.is_none() || !update->lhs.is_equiv(base) ||
      !update->rhs.is_imm()) {
    return false;
  }
  offset = update->rhs.value;
  if (update->tag == MachineInst::Tag::Sub) {
    offset = -offset;
  } else if (update->tag != MachineInst::Tag::Add) {
    return false;
  }
  return offset != 0;
}

bool is_valid_access_imm_offset(i32 offset) {
  return -4095 <= offset && offset <= 4095;
}

bool try_fold_post_index(MachineBB *bb, MIAccess *access) {
  if (!has_zero_offset(access) || !can_write_back_access_base(access)) return false;

  for (MachineInst *inst = access->next; inst; inst = inst->next) {
    if (blocks_local_scan(inst)) return false;

    auto update = dyn_cast<MIBinary>(inst);
    i32 offset = 0;
    if (!match_base_update(update, access->addr, offset)) {
      if (touches_operand(inst, access->addr)) return false;
      continue;
    }
    if (!is_valid_access_imm_offset(offset)) return false;

    if (update->dst.is_equiv(access->addr)) {
      access->mode = MIAccess::Mode::Postfix;
      access->offset = MachineOperand::I(offset);
      access->shift = 0;
      bb->insts.remove(update);
      dbg("Folded address update into post-indexed access");
      return true;
    }

    for (MachineInst *move_inst = update->next; move_inst; move_inst = move_inst->next) {
      if (blocks_local_scan(move_inst)) return false;
      if (auto move = dyn_cast<MIMove>(move_inst); move && move->is_simple() && move->dst.is_equiv(access->addr) &&
                                               move->rhs.is_equiv(update->dst)) {
        access->mode = MIAccess::Mode::Postfix;
        access->offset = MachineOperand::I(offset);
        access->shift = 0;
        bb->insts.remove(update);
        bb->insts.remove(move);
        dbg("Folded address update into post-indexed access");
        return true;
      }
      if (touches_operand(move_inst, access->addr) || touches_operand(move_inst, update->dst)) return false;
    }
    return false;
  }
  return false;
}

}  // namespace

void simplify_asm(MachineFunc* f) {
  for (auto bb = f->bb.head; bb; bb = bb->next) {
    for (auto inst = bb->insts.head; inst; inst = inst->next) {
      if (auto x = dyn_cast<MIMove>(inst)) {
        if (x->shift.type != ArmShift::None && x->shift.shift == 0) {
          x->shift.type = ArmShift::None;
        }
        if (x->dst.is_equiv(x->rhs) && x->is_simple()) {
          dbg("Removed identity move");
          bb->insts.remove(inst);
        } else if (auto y = dyn_cast_nullable<MIMove>(inst->next)) {
          if (y->dst.is_equiv(x->dst) && !y->rhs.is_equiv(x->dst) && y->is_simple()) {
            dbg("Removed useless move");
            bb->insts.remove(inst);
          }
        }
      } else if (auto x = dyn_cast<MIBinary>(inst)) {
        if (x->isIdentity()) {
          dbg("Removed identity binary operation");
          bb->insts.remove(inst);
        }
      } else if (auto x = dyn_cast<MIJump>(inst)) {
        if (x->target == bb->next) {
          dbg("Removed unconditional jump to next bb");
          bb->insts.remove(inst);
        }
      } else if (auto x = dyn_cast<MILoad>(inst)) {
        try_fold_post_index(bb, x);
        if (auto y = dyn_cast_nullable<MIStore>(x->prev)) {
          if (x->addr.is_equiv(y->addr) && x->offset == y->offset && x->shift == y->shift && x->mode == y->mode) {
            // match:
            // str r0, [r1, #0]
            // ldr r2, [r1, #0]
            // ldr can be optimized to:
            // mov r2, r0
            dbg("Removed unneeded load");
            auto i = new MIMove(x->next);
            i->dst = x->dst;
            i->rhs = y->data;
            bb->insts.remove(inst);
          }
        }
      } else if (auto x = dyn_cast<MIStore>(inst)) {
        try_fold_post_index(bb, x);
      } else if (auto x = dyn_cast<MICompare>(inst)) {
        if (auto y = dyn_cast_nullable<MIMove>(x->next)) {
          if (auto z = dyn_cast_nullable<MIMove>(y->next)) {
            if (x->rhs == MachineOperand::I(0) && y->rhs == MachineOperand::I(1) && z->rhs == MachineOperand::I(0) &&
                x->lhs.is_equiv(y->dst) && x->lhs.is_equiv(z->dst) && y->cond == ArmCond::Ne &&
                z->cond == ArmCond::Eq && y->shift.is_none() && z->shift.is_none()) {
              // match:
              // cmp	r1, #0
              // movne	r1, #1
              // moveq	r1, #0
              // the last `moveq` can be removed
              dbg("Simplify vreg != 0");
              bb->insts.remove(z);
            }
          }
        }
      }
    }
  }
}

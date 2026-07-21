// Loop access strength-reduction pass.
//
// Replaces affine `base[index + c]` addressing inside simple loops with pointer
// induction values, reducing repeated index arithmetic.  Example: `a[i + 1]`
// can advance a derived pointer once per iteration.
#include "strength_reduce_loop_access.hpp"

#include <algorithm>
#include <vector>

namespace {

int pred_index(BasicBlock *bb, BasicBlock *pred) {
  auto it = std::find(bb->pred.begin(), bb->pred.end(), pred);
  return it == bb->pred.end() ? -1 : static_cast<int>(it - bb->pred.begin());
}

bool defined_in(Value *v, BasicBlock *header, BasicBlock *body) {
  auto inst = dyn_cast<Inst>(v);
  return inst && (inst->bb == header || inst->bb == body);
}

Value *incoming_from(PhiInst *phi, BasicBlock *pred) {
  int idx = pred_index(phi->bb, pred);
  assert(idx >= 0);
  return phi->incoming_values[idx].value;
}

bool match_add_one(Value *v, PhiInst *iv) {
  auto add = dyn_cast<BinaryInst>(v);
  if (!add || add->tag != Value::Tag::Add) return false;
  if (add->lhs.value == iv) {
    auto c = dyn_cast<ConstValue>(add->rhs.value);
    return c && c->imm == 1;
  }
  if (add->rhs.value == iv) {
    auto c = dyn_cast<ConstValue>(add->lhs.value);
    return c && c->imm == 1;
  }
  return false;
}

bool match_mul_iv(Value *v, PhiInst *iv, Value *&stride) {
  auto mul = dyn_cast<BinaryInst>(v);
  if (!mul || mul->tag != Value::Tag::Mul) return false;
  if (mul->lhs.value == iv) {
    stride = mul->rhs.value;
    return true;
  }
  if (mul->rhs.value == iv) {
    stride = mul->lhs.value;
    return true;
  }
  return false;
}

struct AffineIndex {
  Value *base = nullptr;
  Value *stride = nullptr;
};

bool match_affine_index(Value *v, PhiInst *iv, BasicBlock *header, BasicBlock *body, AffineIndex &index) {
  if (v == iv) {
    index = {ConstValue::get(0), ConstValue::get(1)};
    return true;
  }

  auto bin = dyn_cast<BinaryInst>(v);
  if (!bin) return false;

  Value *stride = nullptr;
  if (match_mul_iv(v, iv, stride)) {
    index = {ConstValue::get(0), stride};
    return !defined_in(index.stride, header, body);
  }

  if (bin->tag == Value::Tag::Sub && bin->rhs.value == iv) {
    index = {bin->lhs.value, ConstValue::get(-1)};
    return !defined_in(index.base, header, body);
  }

  if (bin->tag != Value::Tag::Add) return false;

  if (bin->lhs.value == iv) {
    index = {bin->rhs.value, ConstValue::get(1)};
  } else if (bin->rhs.value == iv) {
    index = {bin->lhs.value, ConstValue::get(1)};
  } else if (match_mul_iv(bin->lhs.value, iv, stride)) {
    index = {bin->rhs.value, stride};
  } else if (match_mul_iv(bin->rhs.value, iv, stride)) {
    index = {bin->lhs.value, stride};
  } else {
    return false;
  }

  return !defined_in(index.base, header, body) && !defined_in(index.stride, header, body);
}

bool is_const_int(Value *v, int imm) {
  auto c = dyn_cast<ConstValue>(v);
  return c && c->imm == imm;
}

Inst *terminator(BasicBlock *bb) {
  assert(bb->insts.tail);
  return bb->insts.tail;
}

GetElementPtrInst *make_gep_before(Decl *sym, Value *arr, Value *index, Inst *before) {
  auto gep = new GetElementPtrInst(sym, arr, index, 1, before->bb);
  gep->bb->insts.remove(gep);
  before->bb->insts.insertBefore(gep, before);
  gep->bb = before->bb;
  return gep;
}

struct Candidate {
  AccessInst *access = nullptr;
  Value *arr = nullptr;
  AffineIndex index;
};

bool collect_candidate(AccessInst *access, PhiInst *iv, BasicBlock *header, BasicBlock *body, Candidate &candidate) {
  if (!access || isa<GetElementPtrInst>(access)) return false;

  if (!defined_in(access->arr.value, header, body)) {
    AffineIndex index;
    if (!match_affine_index(access->index.value, iv, header, body, index)) return false;
    candidate = {access, access->arr.value, index};
    return true;
  }

  auto base_gep = dyn_cast<GetElementPtrInst>(access->arr.value);
  if (!base_gep || base_gep->bb != body) return false;
  if (defined_in(base_gep->arr.value, header, body) || defined_in(access->index.value, header, body)) return false;

  // Handle nested GEPs from row/column style accesses, e.g.
  // load gep(gep(base, iv * row_stride), invariant_col).  The inner GEP's
  // multiplier is part of the pointer induction stride.
  AffineIndex index;
  if (base_gep->index.value == iv) {
    index = {ConstValue::get(0), ConstValue::get(base_gep->multiplier)};
  } else {
    if (!match_affine_index(base_gep->index.value, iv, header, body, index)) return false;
    auto stride = dyn_cast<ConstValue>(index.stride);
    if (!stride) return false;
    index.stride = ConstValue::get(stride->imm * base_gep->multiplier);
  }
  if (!is_const_int(index.base, 0)) return false;

  candidate = {access, base_gep->arr.value, {access->index.value, index.stride}};
  return true;
}

bool collect_loop_shape(BasicBlock *header, std::vector<BasicBlock *> &preheaders, BasicBlock *&body, BasicBlock *&exit) {
  auto br = dyn_cast<BranchInst>(header->insts.tail);
  if (!br || header->pred.size() < 2) return false;

  auto left_jump = dyn_cast_nullable<JumpInst>(br->left->insts.tail);
  auto right_jump = dyn_cast_nullable<JumpInst>(br->right->insts.tail);
  if (left_jump && left_jump->next == header && (!right_jump || right_jump->next != header)) {
    body = br->left;
    exit = br->right;
  } else if (right_jump && right_jump->next == header && (!left_jump || left_jump->next != header)) {
    body = br->right;
    exit = br->left;
  } else {
    return false;
  }

  if (!body || !exit || body->pred.size() != 1 || body->pred[0] != header) return false;
  int body_idx = pred_index(header, body);
  if (body_idx < 0) return false;
  preheaders.clear();
  for (BasicBlock *pred : header->pred) {
    if (pred != body) preheaders.push_back(pred);
  }
  if (preheaders.empty()) return false;
  return true;
}

PhiInst *find_unit_iv(BasicBlock *header, const std::vector<BasicBlock *> &preheaders, BasicBlock *body) {
  for (Inst *inst = header->insts.head; inst; inst = inst->next) {
    auto phi = dyn_cast<PhiInst>(inst);
    if (!phi) break;
    if (!match_add_one(incoming_from(phi, body), phi)) continue;

    bool all_zero_init = true;
    for (BasicBlock *preheader : preheaders) {
      auto init = dyn_cast<ConstValue>(incoming_from(phi, preheader));
      if (!init || init->imm != 0) {
        all_zero_init = false;
        break;
      }
    }
    if (all_zero_init) return phi;
  }
  return nullptr;
}

bool transform(BasicBlock *header) {
  std::vector<BasicBlock *> preheaders;
  BasicBlock *body = nullptr;
  BasicBlock *exit = nullptr;
  if (!collect_loop_shape(header, preheaders, body, exit)) return false;

  PhiInst *iv = find_unit_iv(header, preheaders, body);
  if (!iv) return false;

  std::vector<Candidate> candidates;
  for (Inst *inst = body->insts.head; inst; inst = inst->next) {
    Candidate candidate;
    if (collect_candidate(dyn_cast<AccessInst>(inst), iv, header, body, candidate)) {
      candidates.push_back(candidate);
    }
  }
  if (candidates.empty()) return false;
  if (std::all_of(candidates.begin(), candidates.end(), [](const Candidate &candidate) {
        return is_const_int(candidate.index.stride, 1);
      })) {
    return false;
  }

  dbg("Strength reducing loop array access");
  int body_idx = pred_index(header, body);
  assert(body_idx >= 0);

  for (const Candidate &candidate : candidates) {
    auto ptr = new PhiInst(header);
    auto next = make_gep_before(candidate.access->lhs_sym, ptr, candidate.index.stride, terminator(body));
    for (BasicBlock *preheader : preheaders) {
      int pre_idx = pred_index(header, preheader);
      assert(pre_idx >= 0);
      auto init = make_gep_before(candidate.access->lhs_sym, candidate.arr, candidate.index.base, terminator(preheader));
      ptr->incoming_values[pre_idx].set(init);
    }
    ptr->incoming_values[body_idx].set(next);

    candidate.access->arr.set(ptr);
    candidate.access->index.set(ConstValue::get(0));
  }
  return true;
}

}  // namespace

void strength_reduce_loop_access(IrFunc *f) {
  for (BasicBlock *bb = f->bb.head; bb; bb = bb->next) {
    transform(bb);
  }
}

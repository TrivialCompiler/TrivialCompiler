#include "machine_code.hpp"

#include "casting.hpp"

std::ostream &operator<<(std::ostream &os, const MachineProgram &p) {
  using namespace std;
  // code section
  os << ".section .text" << endl;
  for (auto f = p.func.head; f; f = f->next) {
    os << ".global " << f->func->func->name << endl;
    os << f->func->func->name << ":" << endl;
    IndexMapper<MachineBB> bb_index;
    for (auto bb = f->bb.head; bb; bb = bb->next) {
      os << "_" << bb_index.get(bb) << ":" << endl;
      for (auto inst = bb->insts.head; inst; inst = inst->next) {
        os << "\t";
        if (auto x = dyn_cast<MIJump>(inst)) {
          os << "b _" << bb_index.get(x->target) << endl;
        } else if (auto x = dyn_cast<MILoad>(inst)) {
          os << "ldr " << x->dst << ", [" << x->addr << "]" << endl;
        } else if (auto x = dyn_cast<MIStore>(inst)) {
          os << "str " << x->data << ", [" << x->addr << "]" << endl;
        } else if (auto x = dyn_cast<MIGlobal>(inst)) {
          os << "ldr " << x->dst << ", =" << x->sym->name << endl;
        } else if (auto x = dyn_cast<MIBinary>(inst)) {
          const char *op = "unknown";
          if (x->tag == MachineInst::Mul) {
            op = "mul";
          } else if (x->tag == MachineInst::Add) {
            op = "add";
          } else if (x->tag == MachineInst::Sub) {
            op = "sub";
          } else {
            UNREACHABLE();
          }
          os << op << " " << x->dst << ", " << x->lhs << ", " << x->rhs << endl;
        } else if (auto x = dyn_cast<MIUnary>(inst)) {
          if (x->tag == MachineInst::Mv) {
            os << "mov " << x->dst << ", " << x->rhs << endl;
          } else {
            UNREACHABLE();
          }
        } else if (auto x = dyn_cast<MIReturn>(inst)) {
          os << "bx lr" << endl;
        }
      }
    }
  }
  // data section
  os << ".section .data" << endl;
  for (auto &decl : p.glob_decl) {
    os << ".global " << decl->name << endl;
    os << decl->name << ":" << endl;
    for (auto expr : decl->flatten_init) {
      os << "\t"
         << ".long " << expr->result << endl;
    }
  }
  return os;
}
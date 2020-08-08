#include "pass_manager.hpp"

#include <utility>
#include <variant>

#include "asm/allocate_register.hpp"
#include "asm/compute_stack_info.hpp"
#include "asm/simplify_asm.hpp"
#include "ir/bbopt.hpp"
#include "ir/callgraph.hpp"
#include "ir/cfg.hpp"
#include "ir/dce.hpp"
#include "ir/gvn_gcm.hpp"
#include "ir/loop_unroll.hpp"
#include "ir/mark_global_const.hpp"
#include "ir/mem2reg.hpp"

using IrFuncPass = void (*)(IrFunc *);
using MachineFuncPass = void (*)(MachineFunc *);
// for future use (such as inlining functions)
using IrProgramPass = void (*)(IrProgram *);
using MachineProgramPass = void (*)(MachineProgram *);
using CompilePass = std::variant<IrFuncPass, IrProgramPass, MachineFuncPass, MachineProgramPass>;
using PassDesc = std::pair<CompilePass, const std::string>;

#define DEFINE_PASS(p) \
  { p, #p }

static PassDesc ir_passes[] = {
    DEFINE_PASS(compute_callgraph),
    DEFINE_PASS(mark_global_const),
    DEFINE_PASS(bbopt),
    DEFINE_PASS(compute_dom_info),
    DEFINE_PASS(mem2reg),
    DEFINE_PASS(gvn_gcm),
    DEFINE_PASS(loop_unroll),
    DEFINE_PASS(compute_dom_info),
    DEFINE_PASS(gvn_gcm),
    DEFINE_PASS(dce),
    DEFINE_PASS(bbopt),
    DEFINE_PASS(compute_dom_info),
};

static PassDesc asm_passes[] = {
    DEFINE_PASS(allocate_register),
    DEFINE_PASS(simplify_asm),
    DEFINE_PASS(compute_stack_info)
};

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

template <typename T>
inline void apply_pass(T, const CompilePass &) {}

template <>
inline void apply_pass(MachineProgram *p, const CompilePass &pass) {
  std::visit(overloaded{[&](MachineFuncPass pass) {
                          for (auto *f = p->func.head; f != nullptr; f = f->next) {
                            pass(f);
                          }
                        },
                        [&](MachineProgramPass pass) { pass(p); }, [](auto arg) { UNREACHABLE(); }},
             pass);
}

template <>
inline void apply_pass(IrProgram *p, const CompilePass &pass) {
  std::visit(overloaded{[&](IrFuncPass pass) {
                          for (auto *f = p->func.head; f != nullptr; f = f->next) {
                            if (!f->builtin) pass(f);
                          }
                        },
                        [&](IrProgramPass pass) { pass(p); }, [](auto arg) { UNREACHABLE(); }},
             pass);
}

template <typename T>
static inline void run_pass(T *p, const PassDesc &desc) {
  auto &[pass, name] = desc;
  auto run_pass = "Running pass " + name;
  dbg(run_pass);
  apply_pass(p, pass);
}

void run_ir_passes(IrProgram *p, bool opt) {
  for (auto &desc : ir_passes) {
    run_pass(p, desc);
  }
}

void run_asm_passes(MachineProgram *p, bool opt) {
  for (auto &desc : asm_passes) {
    run_pass(p, desc);
  }
}

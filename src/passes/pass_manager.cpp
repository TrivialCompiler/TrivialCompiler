#include "pass_manager.hpp"

#include <utility>
#include <variant>

#include "asm/allocate_register.hpp"
#include "asm/compute_stack_info.hpp"
#include "asm/simplify_asm.hpp"
#include "asm/scheduling.hpp"
#include "ir/bbopt.hpp"
#include "ir/callgraph.hpp"
#include "ir/cfg.hpp"
#include "ir/dce.hpp"
#include "ir/dead_store_elim.hpp"
#include "ir/gvn_gcm.hpp"
#include "ir/loop_unroll.hpp"
#include "ir/mark_global_const.hpp"
#include "ir/mem2reg.hpp"
#include "ir/extract_stack_array.hpp"

using IrFuncPass = void (*)(IrFunc *);
using IrProgramPass = void (*)(IrProgram *);
using MachineFuncPass = void (*)(MachineFunc *);
using MachineProgramPass = void (*)(MachineProgram *);
using CompilePass = std::variant<IrFuncPass, IrProgramPass, MachineFuncPass, MachineProgramPass>;
using PassDesc = std::pair<CompilePass, const char*>;


#define DEFINE_PASS(p) \
  { p, #p }

static PassDesc ir_passes[] = {
    DEFINE_PASS(compute_callgraph),
    DEFINE_PASS(mark_global_const),
    DEFINE_PASS(bbopt),
    DEFINE_PASS(compute_dom_info),
    DEFINE_PASS(mem2reg),
    DEFINE_PASS(gvn_gcm),
    DEFINE_PASS(compute_callgraph),
    DEFINE_PASS(gvn_gcm),
    DEFINE_PASS(bbopt),
    DEFINE_PASS(compute_dom_info),
    DEFINE_PASS(loop_unroll),
    DEFINE_PASS(compute_dom_info),
    DEFINE_PASS(gvn_gcm),
    DEFINE_PASS(dce),
    DEFINE_PASS(dead_store_elim),
    DEFINE_PASS(bbopt),
    DEFINE_PASS(compute_dom_info),
    DEFINE_PASS(extract_stack_array)
};

static PassDesc asm_passes[] = {DEFINE_PASS(allocate_register), DEFINE_PASS(simplify_asm),
                                DEFINE_PASS(compute_stack_info), DEFINE_PASS(instruction_schedule), DEFINE_PASS(simplify_asm)};

#undef DEFINE_PASS

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

static inline void run_pass(IntermediateProgram p, const PassDesc &desc) {
  auto &pass = std::get<0>(desc);
  auto run_pass = std::string("Running pass ") + std::get<1>(desc);
  dbg(run_pass);
  std::visit(overloaded{[&](IrProgram *p) {
                          std::visit(overloaded{[&](IrFuncPass pass) {
                                                  for (auto *f = p->func.head; f != nullptr; f = f->next) {
                                                    if (!f->builtin) pass(f);
                                                  }
                                                },
                                                [&](IrProgramPass pass) { pass(p); }, [](auto arg) { UNREACHABLE(); }},
                                     pass);
                        },
                        [&](MachineProgram *p) {
                          std::visit(
                              overloaded{[&](MachineFuncPass pass) {
                                           for (auto *f = p->func.head; f != nullptr; f = f->next) {
                                             pass(f);
                                           }
                                         },
                                         [&](MachineProgramPass pass) { pass(p); }, [](auto arg) { UNREACHABLE(); }},
                              pass);
                        }},
             p);
}


void run_passes(IntermediateProgram p, bool opt) {
  if (std::get_if<MachineProgram *>(&p)) {
    for (auto &desc : asm_passes) {
      run_pass(p, desc);
    }
  } else if (std::get_if<IrProgram *>(&p)) {
    for (auto &desc : ir_passes) {
      run_pass(p, desc);
    }
  }
}

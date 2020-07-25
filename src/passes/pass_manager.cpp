#include "pass_manager.hpp"

#include <variant>
#include <utility>

#include "cfg.hpp"
#include "fill_pred.hpp"
#include "gvn_gcm.hpp"
#include "mem2reg.hpp"
#include "dce.hpp"

using IrFuncPass = void (*)(IrFunc *);
using IrProgramPass = void (*)(IrProgram *); // for future use (such as inlining functions)
using IrPass = std::variant<IrFuncPass, IrProgramPass>;
using PassDesc = std::pair<IrPass, const std::string>;

#define DEFINE_PASS(p) {p, #p}

static PassDesc mandatory_passes[] = {
    DEFINE_PASS(fill_pred),
    DEFINE_PASS(compute_dom_info),
    DEFINE_PASS(mem2reg),
    DEFINE_PASS(gvn_gcm),
    DEFINE_PASS(dce),
};
static PassDesc opt_passes[] = {};

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;


static inline void run_pass(IrProgram *p, const PassDesc &desc) {

  auto &[pass, name] = desc;
  dbg("Running IR pass " + name);

  std::visit(overloaded {
        [&](IrFuncPass pass) {
          for (auto *f = p->func.head; f != nullptr; f = f->next) {
            pass(f);
          }
        },
        [&](IrProgramPass pass) { pass(p); }
      }, pass);
}


void run_ir_passes(IrProgram *p, bool opt) {

  for (auto &desc : mandatory_passes) {
    run_pass(p, desc);
  }

  if (opt) {
    for (auto &desc : opt_passes) {
      run_pass(p, desc);
    }
  }
}

#include "../ir.hpp"
#include <unordered_map>

struct Loop {
  Loop *parent;
  std::vector<Loop *> sub_loops;
  // bbs[0]是loop header
  std::vector<BasicBlock *> bbs;

  explicit Loop(BasicBlock *header) : parent(nullptr), bbs{header} {}

  BasicBlock *header() { return bbs[0]; }

  u32 depth() {
    u32 ret = 0;
    for (Loop *x = parent; x; x = x->parent) ++ret;
    return ret;
  }
};

struct LoopInfo {
  // 返回bb所处的最深的循环
  std::unordered_map<BasicBlock *, Loop *> loop_of_bb;
  std::vector<Loop *> top_level;
};

void compute_dom_info(IrFunc *f);

// 这里假定dom树已经造好了
LoopInfo compute_loop_info(IrFunc *f);
#include "../ir.hpp"
#include <unordered_map>

struct Loop {
  Loop *parent;
  std::vector<Loop *> sub_loops;
  // bbs[0]是loop header
  std::vector<BasicBlock *> bbs;

  explicit Loop(BasicBlock *header) : parent(nullptr), bbs{header} {}

  BasicBlock *header() { return bbs[0]; }

  // 对于顶层的循环返回1
  u32 depth() {
    u32 ret = 0;
    for (Loop *x = this; x; x = x->parent) ++ret;
    return ret;
  }
};

struct LoopInfo {
  // 返回bb所处的最深的循环
  std::unordered_map<BasicBlock *, Loop *> loop_of_bb;
  std::vector<Loop *> top_level;

  // 若bb不在任何循环中，返回0
  u32 depth_of(BasicBlock *bb) {
    auto it = loop_of_bb.find(bb);
    return it == loop_of_bb.end() ? 0 : it->second->depth();
  }
};

void compute_dom_info(IrFunc *f);

// 这里假定dom树已经造好了
LoopInfo compute_loop_info(IrFunc *f);

// 计算bb的rpo序
std::vector<BasicBlock *> compute_rpo(IrFunc *f);

// 计算支配边界DF，这里用一个map来存每个bb的df，其实是很随意的选择，把它放在BasicBlock里面也不是不行
std::unordered_map<BasicBlock *, std::unordered_set<BasicBlock *>> compute_df(IrFunc *f);
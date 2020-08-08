#pragma once

#include "../common.hpp"

namespace op {
// 我希望Op和整数能够互相转化，所以不用enum class
// 同时我希望Op的成员不要暴露在全局作用域中，所以用一个namespace包起来
enum Op {
#include "op.inc"
};

inline i32 eval(Op op, i32 l, i32 r) {
  switch (op) {
    case Add:
      return l + r;
    case Sub:
      return l - r;
    case Rsb:
      return r - l;
    case Mul:
      return l * r;
    // 除0就随它去吧，反正我们对于错误都是直接退出的
    case Div:
      return l / r;
    case Mod:
      return l % r;
    case Lt:
      return l < r;
    case Le:
      return l <= r;
    case Ge:
      return l >= r;
    case Gt:
      return l > r;
    case Eq:
      return l == r;
    case Ne:
      return l != r;
    case And:
      return l && r;
    case Or:
      return l || r;
    default:
      UNREACHABLE();
  }
}

// 是否是两个相反的比较运算符，即 l <a> r 是否等于 r <b> l
inline bool isrev(Op a, Op b) {
  return (a == Lt && b == Gt) || (a == Gt && b == Lt) || (a == Le && b == Ge) || (a == Ge && b == Le);
}
}  // namespace op
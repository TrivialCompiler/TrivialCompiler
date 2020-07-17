#pragma once

#include <cstdint>

// 让string_view可以传递给C接口，这个结果不能保存在变量中，只能立即使用，因为临时的string在语句结束后就析构了
#define CSTR(sv) std::string(sv).c_str()
#define ERR(msg, ...)                         \
  do {                                        \
    fprintf(stderr, msg "\n", ##__VA_ARGS__); \
    exit(1);                                  \
  } while (false)
#define UNREACHABLE() __builtin_unreachable()

using i32 = int32_t;
using u32 = uint32_t;

#define DEFINE_CLASSOF(cls, cond) \
  static bool classof(const cls *p) { return cond; }

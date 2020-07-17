#pragma once

#include <cstdint>
#include "thirdparty/dbg.h"

enum {
  SYSTEM_ERROR = 1,
  PARSING_ERROR,
  TYPE_CHECK_ERROR
};

// 让string_view可以传递给C接口，这个结果不能保存在变量中，只能立即使用，因为临时的string在语句结束后就析构了
#define STR(sv) std::string(sv)
#define CSTR(sv) std::string(sv).c_str()
#define ERR_EXIT(code, ...)                         \
  do {                                        \
    dbg(__VA_ARGS__); \
    exit(code);                                  \
  } while (false)
#define UNREACHABLE() __builtin_unreachable()

using i32 = int32_t;
using u32 = uint32_t;

#define DEFINE_CLASSOF(cls, cond) \
  static bool classof(const cls *p) { return cond; }

#define DEFINE_ILIST(cls) \
  cls *prev; \
  cls *next;

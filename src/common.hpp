#pragma once

#include <cstdint>

#define DBG_MACRO_NO_WARNING
#include "thirdparty/dbg.h"

enum { SYSTEM_ERROR = 1, PARSING_ERROR, TYPE_CHECK_ERROR };

#define ERR_EXIT(code, ...) \
  do {                      \
    dbg(__VA_ARGS__);       \
    exit(code);             \
  } while (false)
#define UNREACHABLE() __builtin_unreachable()

using i32 = int32_t;
using u32 = uint32_t;

#define DEFINE_CLASSOF(cls, cond) \
  static bool classof(const cls *p) { return cond; }

#define DEFINE_ILIST(cls) \
  cls *prev;              \
  cls *next;

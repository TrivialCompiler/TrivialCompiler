#pragma once

#include <cstdint>
#include <map>

#define DBG_MACRO_NO_WARNING
#include "thirdparty/dbg.h"

enum { SYSTEM_ERROR = 1, PARSING_ERROR, TYPE_CHECK_ERROR, CODEGEN_ERROR };

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

template <class T>
struct IndexMapper {
  std::map<T *, u32> mapping;
  u32 index_max = 0;

  u32 alloc() { return index_max++; }

  u32 get(T *t) {
    auto [it, inserted] = mapping.insert({t, index_max});
    index_max += inserted;
    return it->second;
  }
};

// see https://alisdair.mcdiarmid.org/arm-immediate-value-encoding/
inline bool can_encode_imm(i32 imm) {
  u32 encoding = imm;
  for (int ror = 0; ror < 32; ror += 2) {
    if (!(encoding & ~0xFFu)) {
      return true;
    }
    encoding = (encoding << 2u) | (encoding >> 30u);
  }
  return false;
}

extern bool debug_mode;

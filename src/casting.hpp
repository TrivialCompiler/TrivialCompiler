#pragma once

#include <type_traits>

template <typename D, typename B>
bool isa(const B *b) {
  if constexpr (std::is_base_of_v<D, B>) {
    return true;
  } else {
    static_assert(std::is_base_of_v<B, D>);
    return D::classof(b);
  }
}

template <typename D, typename B>
const D *dyn_cast(const B *b) {
  return isa<D>(b) ? static_cast<const D *>(b) : nullptr;
}

template <typename D, typename B>
D *dyn_cast(B *b) {
  return isa<D>(b) ? static_cast<D *>(b) : nullptr;
}

template <typename D, typename B>
const D *dyn_cast_nullable(const B *b) {
  return b && isa<D>(b) ? static_cast<const D *>(b) : nullptr;
}

template <typename D, typename B>
D *dyn_cast_nullable(B *b) {
  return b && isa<D>(b) ? static_cast<D *>(b) : nullptr;
}
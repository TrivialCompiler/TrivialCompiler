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
#define UNREACHABLE() ERR_EXIT(SYSTEM_ERROR, "control flow should never reach here")

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

template <class Node>
struct ilist {
  Node *head;
  Node *tail;

  ilist() { head = tail = nullptr; }

  // insert newNode before insertBefore
  void insertBefore(Node *newNode, Node *insertBefore) {
    newNode->prev = insertBefore->prev;
    newNode->next = insertBefore;
    if (insertBefore->prev) {
      insertBefore->prev->next = newNode;
    }
    insertBefore->prev = newNode;

    if (head == insertBefore) {
      head = newNode;
    }
  }

  // insert newNode after insertAfter
  void insertAfter(Node *newNode, Node *insertAfter) {
    newNode->prev = insertAfter;
    newNode->next = insertAfter->next;
    if (insertAfter->next) {
      insertAfter->next->prev = newNode;
    }
    insertAfter->next = newNode;

    if (tail == insertAfter) {
      tail = newNode;
    }
  }

  // insert newNode at the end of ilist
  void insertAtEnd(Node *newNode) {
    newNode->prev = tail;
    newNode->next = nullptr;

    if (tail == nullptr) {
      head = tail = newNode;
    } else {
      tail->next = newNode;
      tail = newNode;
    }
  }

  // insert newNode at the begin of ilist
  void insertAtBegin(Node *newNode) {
    newNode->prev = nullptr;
    newNode->next = head;

    if (head == nullptr) {
      head = tail = newNode;
    } else {
      head->prev = newNode;
      head = newNode;
    }
  }

  // remove node from ilist
  void remove(Node *node) {
    if (node->prev != nullptr) {
      node->prev->next = node->next;
    } else {
      head = node->next;
    }

    if (node->next != nullptr) {
      node->next->prev = node->prev;
    } else {
      tail = node->prev;
    }
  }
};

extern bool debug_mode;

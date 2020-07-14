#pragma once

#include <cstdint>
#include <string_view>
#include <vector>
#include <variant>
#include <utility>

#include <cstdlib>
#include "ast.hpp"

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;

struct Token {
  enum Kind : u8 { _Eps, _Eof, _Err, Or, And, Eq, Ne, Lt, Le, Ge, Gt, Add, Sub, Mul, Div, Mod, Unary, RPar, Empty, Else, Void, Int, Const, While, If, Return, Break, Continue, Assign, Comma, Semi, Not, LPar, LBrk, RBrk, LBrc, RBrc, IntConst, Ident,  } kind;
  std::string_view piece;
  u32 line, col;
};

struct Lexer {
  std::string_view string;
  u32 line, col;

  explicit Lexer(std::string_view string) : string(string), line(1), col(1) {}

  Token next();
};

struct Parser {
  std::variant<Program, Token> parse(Lexer &lexer);
};
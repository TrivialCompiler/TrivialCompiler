#include "ast.hpp"

IntConst IntConst::ZERO{Expr::IntConst, 0, 0};
Break Break::INSTANCE{Stmt::Break};
Continue Continue::INSTANCE{Stmt::Continue};

Func Func::BUILTIN[8] = {
    Func{true, "getint"},
    Func{true, "getch"},
    Func{true, "getarray", {Decl{false, false, false, "a", {nullptr}}}},
    Func{false, "putint", {Decl{false, false, false, "a"}}},
    Func{false, "putch", {Decl{false, false, false, "a"}}},
    Func{false, "putarray", {Decl{false, false, false, "n"}, Decl{false, false, false, "a", {nullptr}}}},
    Func{false, "starttime"},
    Func{false, "stoptime"},
};
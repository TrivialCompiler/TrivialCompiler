#include "ast.hpp"

IntConst IntConst::ZERO{Expr::IntConst, 0, 0};
Break Break::INSTANCE{Stmt::Break};
Continue Continue::INSTANCE{Stmt::Continue};

Func Func::BUILTIN[8] = {
    Func{true, "getint"},
    Func{true, "getch"},
    Func{true, "getarray", {Decl{false, false, false, "a", {nullptr}}}},
    Func{false, "putint", {Decl{false, false, false, "n"}}},
    Func{false, "putch", {Decl{false, false, false, "c"}}},
    Func{false, "putarray", {Decl{false, false, false, "n"}, Decl{false, false, false, "a", {nullptr}}}},
    Func{false, "_sysy_starttime", {Decl{false, false, false, "l"}}},
    Func{false, "_sysy_stoptime", {Decl{false, false, false, "l"}}},
};

Func Func::MEMSET = {false,
                     "memset",
                     {Decl{false, false, false, "arr", {nullptr}}, Decl{false, false, false, "num"},
                      Decl{false, false, false, "count"}}};

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

using i32 = int32_t;

#define DEFINE_CLASSOF(cls, cond) static bool classof(const cls *p) { return cond; }

struct Expr {
  enum {
    Add, Sub, Mul, Div, Mod, Lt, Le, Ge, Gt, Eq, Ne, And, Or, // Binary
    Pos, Neg, Not, // Unary
    Call, Index, IntConst
  } tag;
};

// 操作符保存在Expr::tag中
struct Binary : Expr {
  DEFINE_CLASSOF(Expr, Add <= p->tag && p->tag <= Or);
  Expr *lhs;
  Expr *rhs;
};

// 操作符保存在Expr::tag中
struct Unary : Expr {
  DEFINE_CLASSOF(Expr, Pos <= p->tag && p->tag <= Not);
  Expr *rhs;
};

struct Call : Expr {
  DEFINE_CLASSOF(Expr, p->tag == Expr::Call);
  std::string_view func;
  std::vector<Expr *> args;
};

struct Index : Expr {
  DEFINE_CLASSOF(Expr, p->tag == Expr::Index);
  std::string_view name;
  // array_dims为空时即是直接访问普通变量
  std::vector<Expr *> array_dims;
};

struct IntConst : Expr {
  DEFINE_CLASSOF(Expr, p->tag == Expr::IntConst);
  i32 val;
};

struct InitList {
  // val1为nullptr时val2有效，逻辑上相当于std::variant<Expr *, std::vector<InitList>>，但是stl这一套实在是不好用
  Expr *val1; // nullable
  std::vector<InitList> val2;
};

struct Decl {
  bool is_const;
  bool has_init; // 配合init使用
  std::string_view name;
  // 基本类型总是int，所以不记录，只记录数组维度
  // array_dims[0]可能是nullptr，当且仅当Decl用于Func::params，且参数形如int a[][10]时；其他情况下每个元素都非空
  std::vector<Expr *> array_dims;
  InitList init; // 配合has_init，逻辑上相当于std::optional<InitList>，但是stl这一套实在是不好用
};

struct Stmt {
  enum {
    Assign, ExprStmt, DeclStmt, Block, If, While, Break, Continue, Return
  } tag;
};

struct Assign : Stmt {
  DEFINE_CLASSOF(Stmt, p->tag == Stmt::Assign);
  std::string_view ident;
  std::vector<Expr *> array_dims;
  Expr *rhs;
};

struct ExprStmt : Stmt {
  DEFINE_CLASSOF(Stmt, p->tag == Stmt::ExprStmt);
  Expr *val; // nullable，为空时就是一条分号
};

struct DeclStmt : Stmt {
  DEFINE_CLASSOF(Stmt, p->tag == Stmt::DeclStmt);
  std::vector<Decl> decls; // 一条语句可以定义多个变量
};

struct Block : Stmt {
  DEFINE_CLASSOF(Stmt, p->tag == Stmt::Block);
  std::vector<Stmt *> stmts;
};

struct If : Stmt {
  DEFINE_CLASSOF(Stmt, p->tag == Stmt::If);
  Expr *cond;
  Stmt *on_true;
  Stmt *on_false; // nullable
};

struct While : Stmt {
  DEFINE_CLASSOF(Stmt, p->tag == Stmt::While);
  Expr *cond;
  Stmt *body;
};

struct Break : Stmt {
  DEFINE_CLASSOF(Stmt, p->tag == Stmt::Break);
  // 因为Break和Continue不保存任何信息，用单例来节省一点内存
  static Break instance;
};

struct Continue : Stmt {
  DEFINE_CLASSOF(Stmt, p->tag == Stmt::Continue);
  static Continue instance;
};

struct Return : Stmt {
  DEFINE_CLASSOF(Stmt, p->tag == Stmt::Return);
  Expr *val; // nullable
};

struct Func {
  // 返回类型只能是int/void，因此只记录是否是int
  bool is_int;
  std::string_view name;
  // 只是用Decl来复用一下代码，其实不能算是Decl，is_const / has_init总是false
  std::vector<Decl> params;
  Block body;
};

struct Program {
  std::vector<Func> funcs;
  // 直接按照定义来变量定义应该是一个二维数组，因为一条语句可以定义多个变量，不过把它展开成一维更方便
  std::vector<Decl> decls;
};
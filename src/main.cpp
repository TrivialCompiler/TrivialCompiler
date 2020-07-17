#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <cstdio>
#include <string>

#include "ast.hpp"
#include "generated/parser.hpp"
#include "typeck.hpp"

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <path>\n", argv[0]);
    return 1;
  }
  int fd = open(argv[1], O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "failed to open %s\n", argv[1]);
    return 1;
  }
  struct stat st {};
  fstat(fd, &st);
  char *input = (char *)mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

  Lexer l(std::string_view(input, st.st_size));
  auto result = Parser{}.parse(l);

  if (Program *p = std::get_if<0>(&result)) {
    dbg("parsing success");
    type_check(*p);  // 失败时直接就exit(1)了
    dbg("type_check success");
  } else if (Token *t = std::get_if<1>(&result)) {
    ERR("parsing error", t->kind, t->line, t->col, STR(t->piece));
  }

  munmap(input, st.st_size);

  return 0;
}

extern "C" const char *__asan_default_options() { return "detect_leaks=0"; }

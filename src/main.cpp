#include <cstdio>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string>
#include <fcntl.h>
#include "ast.hpp"
#include "parser.hpp"

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
  struct stat st;
  fstat(fd, &st);
  char *input = (char *) mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

  Lexer l(std::string_view(input, st.st_size));
  auto result = Parser{}.parse(l);
  int res = 0;
  if (Program *p = std::get_if<0>(&result)) {
    puts("parsing success");
  } else if (Token *t = std::get_if<1>(&result)) {
    fprintf(stderr, "parsing error at token id %d, line %d, col %d, string piece = %s\n",
            t->kind, t->line, t->col, std::string(t->piece).c_str()); // string_view不能直接喂给C接口
    res = 1;
  }
  munmap(input, st.st_size);

  return res;
}

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>

#include "conv/codegen.hpp"
#include "conv/parser.hpp"
#include "conv/ssa.hpp"
#include "conv/typeck.hpp"
#include "passes/pass_manager.hpp"


int main(int argc, char *argv[]) {
  bool opt = false, print_usage = false;
  char *src = nullptr, *output = nullptr, *ir_file = nullptr;

  // parse command line options and check
  for (int ch; (ch = getopt(argc, argv, "Sdl:o:O:h")) != -1;) {
    switch (ch) {
      case 'S':
        // do nothing
        break;
      case 'd':
        debug_mode = true;
        break;
      case 'l':
        ir_file = strdup(optarg);
        break;
      case 'o':
        output = strdup(optarg);
        break;
      case 'O':
        opt = atoi(optarg) > 0;
        break;
      case 'h':
        print_usage = true;
        break;
      default:
        break;
    }
  }

  if (optind <= argc) {
    src = argv[optind];
  }

  dbg(src, output, ir_file, opt, print_usage, debug_mode);

  if (src == nullptr || print_usage) {
    fprintf(stderr, "Usage: %s [-l ir_file] [-S] [-d (debug mode)] [-o output_file] [-O level] input_file\n", argv[0]);
    return !print_usage && SYSTEM_ERROR;
  }

  // open input file
  int fd = open(src, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "failed to open %s\n", argv[1]);
    return SYSTEM_ERROR;
  }
  struct stat st {};
  fstat(fd, &st);
  char *input = (char *)mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

  // run lexer
  Lexer l(std::string_view(input, st.st_size));
  auto result = Parser{}.parse(l);

  // run parser
  if (Program *p = std::get_if<0>(&result)) {
    dbg("parsing success");
    type_check(*p);  // 失败时直接就exit(1)了
    dbg("type_check success");
    auto *ir = convert_ssa(*p);
    run_ir_passes(ir, opt);
    if (ir_file != nullptr) {
      std::ofstream(ir_file) << *ir;
    }
    if (output != nullptr) {
      auto *code = machine_code_generation(ir);
      run_asm_passes(code, opt);
      std::ofstream(output) << *code;
    }
  } else if (Token *t = std::get_if<1>(&result)) {
    ERR_EXIT(PARSING_ERROR, "parsing error", t->kind, t->line, t->col, t->piece);
  }

  // post-precess
  munmap(input, st.st_size);
  free(output);
  free(ir_file);

  return 0;
}

// ASan config
extern "C" [[maybe_unused]] const char *__asan_default_options() { return "detect_leaks=0"; }

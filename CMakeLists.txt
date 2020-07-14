cmake_minimum_required(VERSION 3.15)
project(TrivialCompiler)
enable_testing()

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address")
add_executable(TrivialCompiler src/main.cpp src/ast.hpp src/ast.cpp src/parser.hpp src/parser.cpp src/casting.hpp)

file(GLOB files "thirdparty/sysyruntimelibrary/*_test/*.sy")
foreach(file ${files})
    add_test(NAME check_parse_${file}
            COMMAND TrivialCompiler ${file})
endforeach()
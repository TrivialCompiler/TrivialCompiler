cmake_minimum_required(VERSION 3.15)
project(TrivialCompiler)

set(CMAKE_CXX_STANDARD 17)

add_executable(TrivialCompiler src/main.cpp src/ast.hpp src/ast.cpp src/parser.hpp src/parser.cpp src/casting.hpp)
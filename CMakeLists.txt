cmake_minimum_required(VERSION 3.13)
set(project_name TrivialCompiler)
project(${project_name} CXX)
set(CMAKE_CXX_STANDARD 17)

option(ENABLE_COVERAGE "Enable code coverage" OFF)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pedantic -Wall -Wno-missing-braces")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address")
if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -static-libasan")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -static-libsan")
    endif()
endif()

if (ENABLE_COVERAGE)
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} --coverage")
endif()

file(GLOB_RECURSE source_files "src/*.cpp")
file(GLOB_RECURSE header_files "src/*.hpp")
add_executable(${project_name} ${source_files} ${header_files})

# use precompiled header to accelerate compiling
if(COMMAND target_precompile_headers)
    target_precompile_headers(${project_name} PRIVATE "src/common.hpp")
endif()

option(FUNC_TEST "Run functional test cases" ON)
option(PERF_TEST "Run performance test cases" ON)
option(CUSTOM_TEST "Run custom test cases" ON)

message("Functional test: ${FUNC_TEST}")
message("Performance test: ${PERF_TEST}")
message("Custom test: ${CUSTOM_TEST}")

option(RUN_GCC "Run gcc for testing" OFF)
option(RUN_CLANG "Run clang for testing" OFF)

message("Run GCC: ${RUN_GCC}")
message("Run Clang: ${RUN_CLANG}")

enable_testing()

file(GLOB preliminary_func_test_cases "sysyruntimelibrary/section1/functional_test/*.sy")
file(GLOB preliminary_perf_test_cases "sysyruntimelibrary/section1/performance_test/*.sy")
file(GLOB final_perf_test_cases "sysyruntimelibrary/section2/performance_test/*.sy")
file(GLOB custom_test_cases "custom_test/*.sy")

set(all_test_cases "")

if (FUNC_TEST)
    set(all_test_cases ${all_test_cases} ${preliminary_func_test_cases})
endif()

if (PERF_TEST)
    set(all_test_cases ${all_test_cases} ${preliminary_perf_test_cases} ${final_perf_test_cases})
endif()

if (CUSTOM_TEST)
    set(all_test_cases ${all_test_cases} ${custom_test_cases})
endif()

set(run_command_prefix /usr/bin/time -v timeout -v 120)
set(test_command bash "${CMAKE_CURRENT_SOURCE_DIR}/utils/run_case.sh")

# create test cases
foreach(case_file ${all_test_cases})

    get_filename_component(case_name "${case_file}" NAME_WE)
    string(REGEX REPLACE ".sy$" ".in" case_input ${case_file})
    string(REGEX REPLACE ".sy$" ".out" case_output ${case_file})

    # check our compiler
    # .sy -> .ll
    add_custom_command(OUTPUT "${case_name}.ll"
            COMMAND ${run_command_prefix} ./${project_name} -l "${case_name}.ll" "${case_file}"
            DEPENDS ./${project_name} "${case_file}")
    add_custom_target("llvm_ir_${case_name}" DEPENDS "${case_name}.ll")
    # .sy -> .S
    add_custom_command(OUTPUT "${case_name}.S"
            COMMAND ${run_command_prefix} ./${project_name} -o "${case_name}.S" "${case_file}"
            DEPENDS ./${project_name} "${case_file}")
    add_custom_target("asm_${case_name}" DEPENDS "${case_name}.S")

    if (RUN_GCC)
        # use GCC to generate exe
        # .sy -> .o
        add_custom_command(OUTPUT "${case_name}_gcc.o"
            COMMAND arm-linux-gnueabihf-g++ -x c++ -c -Ofast -g -marm -mcpu=cortex-a72 -mfpu=neon -mfloat-abi=hard -static -include "${CMAKE_CURRENT_SOURCE_DIR}/custom_test/sylib.h" "${case_file}" -o "${case_name}_gcc.o"
            DEPENDS "${case_file}" "${case_file}")
        # .o -> exe
        add_custom_target("${case_name}_gcc"
            COMMAND arm-linux-gnueabihf-gcc -u getint -g -marm -mfpu=neon -mfloat-abi=hard -static "${case_name}_gcc.o" "${CMAKE_CURRENT_SOURCE_DIR}/sysyruntimelibrary/libsysy.a" -o "${case_name}_gcc"
            DEPENDS "${case_name}_gcc.o")
        # run exe with qemu to test
        add_custom_target("test_${case_name}_gcc"
            COMMAND ${test_command} "./${case_name}_gcc" "${case_input}" "${case_name}_gcc.out" "${case_output}"
            DEPENDS "${case_name}_gcc")
        if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
            add_test(NAME check_run_gcc_${case_name}
                COMMAND make "test_${case_name}_gcc")
        endif()
    endif()

    if (RUN_CLANG)
        # use Clang to generate exe
        # .sy -> .o
        if (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "armv7l")
            add_custom_command(OUTPUT "${case_name}_clang.o"
                COMMAND clang++ -x c++ -c -Ofast -g -mcpu=cortex-a72 -mfpu=neon -mfloat-abi=hard -static -include "${CMAKE_CURRENT_SOURCE_DIR}/custom_test/sylib.h" "${case_file}" -o "${case_name}_clang.o"
                DEPENDS "${case_file}" "${case_file}")
        else ()
            add_custom_command(OUTPUT "${case_name}_clang.o"
                COMMAND clang++ -x c++ -c -Ofast -g --sysroot /usr/arm-linux-gnueabihf --target=armv7-unknown-linux-eabi -mcpu=cortex-a72 -mfpu=neon -mfloat-abi=hard -static -include "${CMAKE_CURRENT_SOURCE_DIR}/custom_test/sylib.h" "${case_file}" -o "${case_name}_clang.o"
                DEPENDS "${case_file}" "${case_file}")
        endif ()
        # .o -> exe
        add_custom_target("${case_name}_clang"
            COMMAND arm-linux-gnueabihf-gcc -u getint -g -marm -mfpu=neon -mfloat-abi=hard -static "${case_name}_clang.o" "${CMAKE_CURRENT_SOURCE_DIR}/sysyruntimelibrary/libsysy.a" -o "${case_name}_clang"
            DEPENDS "${case_name}_clang.o")
        # run exe with qemu to test
        add_custom_target("test_${case_name}_clang"
            COMMAND ${test_command} "./${case_name}_clang" "${case_input}" "${case_name}_clang.out" "${case_output}"
            DEPENDS "${case_name}_clang")
        if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
            add_test(NAME check_run_clang_${case_name}
                COMMAND make "test_${case_name}_clang")
        endif()
    endif()

    # use LLVM to generate exe from IR
    # .ll -> .o
    add_custom_command(OUTPUT "${case_name}_llvm.o"
            COMMAND llc -O3 -march=arm -mcpu=cortex-a72 -float-abi=hard -filetype=obj "${case_name}.ll" -o "${case_name}_llvm.o"
            DEPENDS "${case_name}.ll")
    # .o -> exe
    add_custom_target("${case_name}_llvm"
            COMMAND arm-linux-gnueabihf-gcc -u getint -g -marm -mfpu=neon -mfloat-abi=hard -static "${case_name}_llvm.o" "${CMAKE_CURRENT_SOURCE_DIR}/sysyruntimelibrary/libsysy.a" -o "${case_name}_llvm"
            DEPENDS "${case_name}_llvm.o")
    # run exe with qemu to test
    add_custom_target("test_${case_name}_llvm"
            COMMAND ${test_command} "./${case_name}_llvm" "${case_input}" "${case_name}_llvm.out" "${case_output}"
            DEPENDS "${case_name}_llvm")
    if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        add_test(NAME check_run_llvm_${case_name}
                COMMAND make "test_${case_name}_llvm")
    endif()

    # use our compiler to generate exe
    # .S -> .o
    add_custom_command(OUTPUT "${case_name}_tc.o"
            COMMAND arm-linux-gnueabihf-as -g -march=armv7-a -mfloat-abi=hard "${case_name}.S" -o "${case_name}_tc.o"
            DEPENDS "${case_name}.S")
    # .o -> exe
    add_custom_target("${case_name}_tc"
            COMMAND arm-linux-gnueabihf-gcc -g -marm -march=armv7-a -mfpu=neon -mfloat-abi=hard -static "${case_name}_tc.o" "${CMAKE_CURRENT_SOURCE_DIR}/sysyruntimelibrary/libsysy.a" -o "${case_name}_tc"
            DEPENDS "${case_name}_tc.o")
    # run exe with qemu to test
    add_custom_target("test_${case_name}_tc"
            COMMAND ${test_command} "./${case_name}_tc" "${case_input}" "${case_name}_tc.out" "${case_output}"
            DEPENDS "${case_name}_tc")
    if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        add_test(NAME check_run_tc_${case_name}
                COMMAND make "test_${case_name}_tc")
    endif()

endforeach()

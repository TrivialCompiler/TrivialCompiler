cmake_minimum_required(VERSION 3.13)
set(project_name TrivialCompiler)
project(${project_name})

enable_testing()

set(CMAKE_CXX_STANDARD 17)
if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -static-libasan")
else()
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address")
endif()

file(GLOB_RECURSE source_files "src/*.cpp")
file(GLOB_RECURSE header_files "src/*.hpp")
add_executable(${project_name} ${source_files} ${header_files})

# use precompiled header to accelerate compiling
if(COMMAND target_precompile_headers)
    target_precompile_headers(${project_name} PRIVATE "src/common.hpp")
endif()


file(GLOB func_test_cases "sysyruntimelibrary/functional_test/*.sy")
file(GLOB perf_test_cases "sysyruntimelibrary/performance_test/*.sy")
file(GLOB custom_test_cases "custom_test/*.sy")
set(all_test_cases ${func_test_cases} ${perf_test_cases} ${custom_test_cases})


# create test cases
foreach(case_file ${all_test_cases})

    get_filename_component(case_name "${case_file}" NAME_WE)
    string(REGEX REPLACE ".sy$" ".in" case_input ${case_file})
    string(REGEX REPLACE ".sy$" ".out" case_output ${case_file})

    # check our compiler
    # .sy -> .bc
    add_custom_command(OUTPUT "${case_name}.bc"
            COMMAND timeout -v 3 ./${project_name} -d -l "${case_name}.bc" "${case_file}"
            DEPENDS ./${project_name})
    add_custom_target("llvm_ir_${case_name}" DEPENDS "${case_name}.bc")
    add_test(NAME check_emit_ir_${case_name}
            COMMAND make "llvm_ir_${case_name}")
    # .sy -> .S
    add_custom_command(OUTPUT "${case_name}.S"
            COMMAND timeout -v 3 ./${project_name} -d -o "${case_name}.S" "${case_file}"
            DEPENDS ./${project_name})
    add_custom_target("asm_${case_name}" DEPENDS "${case_name}.S")
    add_test(NAME check_emit_asm_${case_name}
            COMMAND make "asm_${case_name}")

    # use LLVM to generate exe
    # .bc -> .o
    add_custom_command(OUTPUT "${case_name}_llvm.o"
            COMMAND llc -O2 -march=arm -mcpu=cortex-a72 -float-abi=hard -filetype=obj "${case_name}.bc" -o "${case_name}_llvm.o"
	    DEPENDS "${case_name}.bc")
    # .o -> exe
    add_custom_target("${case_name}_llvm"
	    COMMAND arm-linux-gnueabihf-gcc -u getint -g -marm -mfpu=neon -mfloat-abi=hard -static "${case_name}_llvm.o" "${CMAKE_CURRENT_SOURCE_DIR}/sysyruntimelibrary/libsysy.a" -o "${case_name}_llvm"
	    DEPENDS "${case_name}_llvm.o")
    # run exe with qemu to test
    add_custom_target("test_${case_name}_llvm"
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/test.sh" "${case_name}_llvm" "${case_input}" "${case_name}.out" "${case_output}"
        DEPENDS "${case_name}_llvm")
    if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        add_test(NAME check_run_llvm_${case_name}
            COMMAND make "test_${case_name}_llvm")
    endif()

    # use our compiler to generate exe
    # .S -> .o
    add_custom_command(OUTPUT "${case_name}_tc.o"
            COMMAND arm-linux-gnueabihf-as -g -mfloat-abi=hard "${case_name}.S" -o "${case_name}_tc.o"
            DEPENDS "${case_name}.S")
    # .o -> exe
    add_custom_target("${case_name}_tc"
            COMMAND arm-linux-gnueabihf-gcc -u getint -g -marm -mfpu=neon -mfloat-abi=hard -static "${case_name}_tc.o" "${CMAKE_CURRENT_SOURCE_DIR}/sysyruntimelibrary/libsysy.a" -o "${case_name}_tc"
            DEPENDS "${case_name}_tc.o")
    # run exe with qemu to test
    add_custom_target("test_${case_name}_tc"
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/test.sh" "${case_name}_tc" "${case_input}" "${case_name}.out" "${case_output}"
            DEPENDS "${case_name}_tc")
    if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        add_test(NAME check_run_tc_${case_name}
                COMMAND make "test_${case_name}_tc")
    endif()

endforeach()

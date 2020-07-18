cmake_minimum_required(VERSION 3.13)
set(project_name TrivialCompiler)
project(${project_name})

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address")

file(GLOB_RECURSE source_files "src/*.cpp")
file(GLOB_RECURSE header_files "src/*.hpp")
add_executable(${project_name} ${source_files} ${header_files})

# use precompiled header to accelerate compiling
if(COMMAND target_precompile_headers)
    target_precompile_headers(${project_name} PRIVATE "src/common.hpp")
endif()


file(GLOB func_test_cases "sysyruntimelibrary/functional_test/*.sy")
file(GLOB perf_test_cases "sysyruntimelibrary/performance_test/*.sy")
set(all_test_cases ${func_test_cases} ${perf_test_cases})


# test parsing
foreach(case_file ${all_test_cases})
    get_filename_component(case_name "${case_file}" NAME_WE)
    add_test(NAME check_parse_${case_name}
            COMMAND ./${project_name} "${case_file}")
    add_test(NAME check_emit_llvm_${case_name}
            COMMAND ./${project_name} -l "${case_file}")

    add_custom_command(OUTPUT "${case_name}.bc"
            COMMAND ./${project_name} -l "${case_file}" > "${case_name}.bc"
	    DEPENDS ./${project_name})
    add_custom_command(OUTPUT "${case_name}.o"
            COMMAND llc -march=thumb -float-abi=hard -filetype=obj "${case_name}.bc" -o "${case_name}.o"
	    DEPENDS "${case_name}.bc")
    add_custom_target("${case_name}"
	    COMMAND arm-linux-gnueabihf-gcc -mthumb -mfpu=neon -mfloat-abi=hard -static "${case_name}.o" "${CMAKE_CURRENT_SOURCE_DIR}/sysyruntimelibrary/libsysy.a" -o "${case_name}"
	    DEPENDS "${case_name}.o")
    string(REGEX REPLACE ".sy$" ".in" case_input ${case_file})
    string(REGEX REPLACE ".sy$" ".out" case_output ${case_file})
    if (EXISTS ${case_input})
        add_custom_target("run_${case_name}"
	    COMMAND qemu-arm "${case_name}" < "${case_input}" || true
	    DEPENDS "${case_name}")
    else()
        add_custom_target("run_${case_name}"
	    COMMAND qemu-arm "${case_name}" || true
	    DEPENDS "${case_name}")
    endif()
    add_custom_target("test_${case_name}"
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/test.sh" ${case_name} ${case_input} "${case_name}.out" ${case_output}
        DEPENDS "${case_name}")
    add_test(NAME check_run_llvm_${case_name}
	    COMMAND make "test_${case_name}")
endforeach()

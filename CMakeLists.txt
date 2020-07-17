cmake_minimum_required(VERSION 3.15)
set(project_name TrivialCompiler)
project(${project_name})

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

file(GLOB source_files "src/*.cpp")
file(GLOB header_files "src/*.hpp")
add_executable(${project_name} ${source_files} ${header_files})


file(GLOB func_test_cases "thirdparty/sysyruntimelibrary/functional_test/*.sy")
file(GLOB perf_test_cases "thirdparty/sysyruntimelibrary/performance_test/*.sy")
set(all_test_cases ${func_test_cases} ${perf_test_cases})


# test parsing
foreach(case_file ${all_test_cases})
    get_filename_component(case_name "${case_file}" NAME_WE)
    add_test(NAME check_parse_${case_name}
            COMMAND ./${project_name} "${case_file}")
endforeach()

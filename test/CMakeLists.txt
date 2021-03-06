add_executable(TestVC4C "")
include(sources.list)
target_link_libraries(TestVC4C VC4CC ${SYSROOT_LIBRARY_FLAGS})
target_compile_options(TestVC4C PRIVATE ${VC4C_ENABLED_WARNINGS})

if(BUILD_DEBUG)
	target_compile_definitions(TestVC4C PRIVATE DEBUG_MODE=1)
endif(BUILD_DEBUG)

target_include_directories (TestVC4C PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_include_directories(TestVC4C PRIVATE ${variant_HEADERS})

add_dependencies(TestVC4C cpptest-lite-dependencies)
target_link_libraries(TestVC4C ${cpptest_LIBS})
target_include_directories (TestVC4C PRIVATE "${cpptest_HEADERS}")

if(SPIRV_CLANG_FOUND)
	target_compile_definitions(TestVC4C PRIVATE SPIRV_CLANG_PATH="${SPIRV_CLANG_FOUND}")
endif(SPIRV_CLANG_FOUND)
if(ENABLE_COVERAGE)
	target_compile_options(TestVC4C PRIVATE -fprofile-arcs -ftest-coverage --coverage)
	target_link_libraries(TestVC4C gcov "-fprofile-arcs -ftest-coverage")
endif(ENABLE_COVERAGE)

add_test(NAME Regressions COMMAND ./build/test/TestVC4C --fast-regressions WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME Emulator COMMAND ./build/test/TestVC4C --test-emulator WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME Instructions COMMAND ./build/test/TestVC4C --test-instructions WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME Operators COMMAND ./build/test/TestVC4C --test-operators WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME Stdlib COMMAND ./build/test/TestVC4C --test-stdlib WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

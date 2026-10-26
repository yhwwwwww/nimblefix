cmake_minimum_required(VERSION 3.23)
project(NimbleFIX VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(NIMBLEFIX_BUILD_TESTS "Build contract tests" ON)
option(NIMBLEFIX_BUILD_EXAMPLES "Build the codec example" ON)
option(NIMBLEFIX_BUILD_BENCHMARKS "Build the codec microbenchmark" OFF)
option(NIMBLEFIX_BUILD_SAMPLE_SCHEMA "Generate the optional sample FIX44 schema" ON)
option(NIMBLEFIX_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(nimblefix STATIC src/codec/parser.cpp src/codec/encoder.cpp src/message/decimal.cpp)
add_library(NimbleFIX::nimblefix ALIAS nimblefix)
target_compile_features(nimblefix PUBLIC cxx_std_20)
target_include_directories(nimblefix PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(nimblefix PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nimblefix PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(NIMBLEFIX_SANITIZE)
    target_compile_options(nimblefix PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(nimblefix PUBLIC -fsanitize=address,undefined)
endif()

set(NIMBLEFIX_SCHEMA_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/tools/schema/generate.py")
include(cmake/NimbleFIXSchema.cmake)
if(NIMBLEFIX_BUILD_SAMPLE_SCHEMA)
    nimblefix_generate_schema(TARGET fix44_sample NAMESPACE nimblefix::fix44_sample
        HEADER nimblefix/fix44_sample.hpp XML_FILES "${CMAKE_CURRENT_SOURCE_DIR}/schemas/fix44-sample.xml")
    add_library(NimbleFIX::fix44_sample ALIAS fix44_sample)
    install(TARGETS fix44_sample EXPORT NimbleFIXTargets)
    install(FILES "${fix44_sample_HEADER}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/nimblefix")
endif()

if(NIMBLEFIX_BUILD_EXAMPLES)
    add_executable(codec-demo examples/codec.cpp)
    target_link_libraries(codec-demo PRIVATE nimblefix)
endif()

if(NIMBLEFIX_BUILD_TESTS)
    enable_testing()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    foreach(name parser message contract generated)
        if((name STREQUAL "contract" OR name STREQUAL "generated") AND NOT NIMBLEFIX_BUILD_SAMPLE_SCHEMA)
            continue()
        endif()
        add_executable(${name}-test tests/${name}_test.cpp)
        target_link_libraries(${name}-test PRIVATE nimblefix)
        if(name STREQUAL "contract" OR name STREQUAL "generated")
            target_link_libraries(${name}-test PRIVATE fix44_sample)
        endif()
        add_test(NAME ${name} COMMAND ${name}-test)
    endforeach()
    add_test(NAME xml-codegen COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen_test.py")
    add_test(NAME compile-fail COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail_test.py"
        --cxx "${CMAKE_CXX_COMPILER}")
    if(NIMBLEFIX_BUILD_EXAMPLES)
        add_test(NAME codec-demo COMMAND codec-demo --self-test)
    endif()
endif()

if(NIMBLEFIX_BUILD_BENCHMARKS)
    if(NOT NIMBLEFIX_BUILD_SAMPLE_SCHEMA)
        message(FATAL_ERROR "The comparison benchmark requires NIMBLEFIX_BUILD_SAMPLE_SCHEMA=ON")
    endif()
    add_executable(codec-bench bench/codec_bench.cpp)
    target_link_libraries(codec-bench PRIVATE nimblefix fix44_sample)
    target_link_options(codec-bench PRIVATE -Wl,--wrap=malloc)
endif()

install(TARGETS nimblefix EXPORT NimbleFIXTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/nimblefix DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES tools/schema/generate.py DESTINATION "${CMAKE_INSTALL_DATADIR}/nimblefix/schema")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/NimbleFIX")
install(EXPORT NimbleFIXTargets NAMESPACE NimbleFIX:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/NimbleFIX")
configure_package_config_file(cmake/NimbleFIXConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/NimbleFIXConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/NimbleFIX"
    PATH_VARS CMAKE_INSTALL_DATADIR)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/NimbleFIXConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/NimbleFIXConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/NimbleFIXConfigVersion.cmake" cmake/NimbleFIXSchema.cmake
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/NimbleFIX")

cmake_minimum_required(VERSION 3.20)

project(fastfix VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(FASTFIX_BUILD_TOOLS "Build FastFix command-line tools" ON)
option(FASTFIX_BUILD_TESTS "Build the FastFix Catch2 test suite" ON)
option(FASTFIX_BUILD_BENCHMARKS "Build the FastFix and QuickFIX benchmark drivers" ON)
option(FASTFIX_BUILD_FUZZERS "Build the standalone fuzz corpus runners" ON)
option(FASTFIX_BUILD_LIBFUZZER "Build the libFuzzer entrypoint" OFF)
option(FASTFIX_ENABLE_LIBURING "Enable io_uring when liburing headers and library are available" ON)

include(CTest)
include(GNUInstallDirs)
include(CheckIncludeFileCXX)

find_package(Threads REQUIRED)

set(FASTFIX_PROJECT_ROOT "${PROJECT_SOURCE_DIR}")
set(FASTFIX_VENDOR_HINT "Run 'git submodule update --init --recursive'.")
set(FASTFIX_CATCH2_ROOT "${PROJECT_SOURCE_DIR}/deps/src/Catch2")
set(FASTFIX_CATCH2_OVERRIDE_DIR "${PROJECT_SOURCE_DIR}/deps/include")
set(FASTFIX_PUGIXML_ROOT "${PROJECT_SOURCE_DIR}/deps/src/pugixml")
set(FASTFIX_QUICKFIX_ROOT "${PROJECT_SOURCE_DIR}/bench/vendor/quickfix")
set(FASTFIX_QUICKFIX_OVERRIDE_DIR "${PROJECT_SOURCE_DIR}/deps/include/quickfix")
set(FASTFIX_GENERATED_DIR "${FASTFIX_PROJECT_ROOT}/build/generated")
set(FASTFIX_OUTPUT_ROOT "${FASTFIX_PROJECT_ROOT}/build")
set(FASTFIX_BENCH_OUTPUT_DIR "${FASTFIX_OUTPUT_ROOT}/bench")
set(FASTFIX_FIX44_XML "${FASTFIX_QUICKFIX_ROOT}/spec/FIX44.xml")
set(FASTFIX_FIX44_FFD "${FASTFIX_BENCH_OUTPUT_DIR}/quickfix_FIX44.ffd")
set(FASTFIX_FIX44_ART "${FASTFIX_BENCH_OUTPUT_DIR}/quickfix_FIX44.art")
set(FASTFIX_FIX44_BUILDERS "${FASTFIX_GENERATED_DIR}/fix44_builders.h")
set(FASTFIX_SAMPLE_ART "${FASTFIX_OUTPUT_ROOT}/sample-basic.art")
set(FASTFIX_SAMPLE_BUILDERS "${FASTFIX_GENERATED_DIR}/sample_basic_builders.h")

file(MAKE_DIRECTORY "${FASTFIX_GENERATED_DIR}")
file(MAKE_DIRECTORY "${FASTFIX_BENCH_OUTPUT_DIR}")

function(fastfix_require_checkout required_path label)
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Missing ${label} at ${required_path}. ${FASTFIX_VENDOR_HINT}")
    endif()
endfunction()

fastfix_require_checkout("${FASTFIX_CATCH2_ROOT}/extras/catch_amalgamated.cpp" "Catch2 submodule checkout")
fastfix_require_checkout("${FASTFIX_CATCH2_OVERRIDE_DIR}/catch2/catch_user_config.hpp" "Catch2 override header")
fastfix_require_checkout("${FASTFIX_PUGIXML_ROOT}/src/pugixml.cpp" "pugixml submodule checkout")
fastfix_require_checkout("${FASTFIX_QUICKFIX_ROOT}/spec/FIX44.xml" "QuickFIX submodule checkout")
fastfix_require_checkout("${FASTFIX_QUICKFIX_OVERRIDE_DIR}/Allocator.h" "QuickFIX override header")

check_include_file_cxx("liburing.h" FASTFIX_HAVE_LIBURING_HEADER)
find_library(FASTFIX_URING_LIBRARY NAMES uring)

if(FASTFIX_ENABLE_LIBURING AND FASTFIX_HAVE_LIBURING_HEADER AND FASTFIX_URING_LIBRARY)
    set(FASTFIX_USE_LIBURING ON)
    message(STATUS "FastFix: enabling io_uring backend")
else()
    set(FASTFIX_USE_LIBURING OFF)
    message(STATUS "FastFix: building without io_uring backend")
endif()

add_library(fastfix-thirdparty-pugixml STATIC
    "${FASTFIX_PUGIXML_ROOT}/src/pugixml.cpp"
)
target_include_directories(fastfix-thirdparty-pugixml
    PUBLIC
        "${FASTFIX_PUGIXML_ROOT}/src"
)

add_library(fastfix-thirdparty-catch2-main STATIC
    "${FASTFIX_CATCH2_ROOT}/extras/catch_amalgamated.cpp"
)
target_include_directories(fastfix-thirdparty-catch2-main
    PUBLIC
        "${FASTFIX_CATCH2_OVERRIDE_DIR}"
        "${FASTFIX_CATCH2_ROOT}/src"
    PRIVATE
        "${FASTFIX_CATCH2_ROOT}/extras"
)
target_compile_features(fastfix-thirdparty-catch2-main PUBLIC cxx_std_14)

file(GLOB FASTFIX_LIBRARY_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/profile/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/session/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/store/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/message/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/codec/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/transport/*.cpp"
)

add_library(fastfix STATIC ${FASTFIX_LIBRARY_SOURCES})
target_include_directories(fastfix
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)
target_compile_definitions(fastfix
    PUBLIC
        FASTFIX_PROJECT_DIR=\"${FASTFIX_PROJECT_ROOT}\"
)
target_link_libraries(fastfix PUBLIC Threads::Threads)

if(FASTFIX_USE_LIBURING)
    target_link_libraries(fastfix PUBLIC "${FASTFIX_URING_LIBRARY}")
else()
    target_compile_definitions(fastfix PRIVATE FASTFIX_DISABLE_LIBURING)
endif()

function(fastfix_add_binary target_name)
    add_executable(${target_name} ${ARGN})
    target_link_libraries(${target_name} PRIVATE fastfix)
endfunction()

file(GLOB FASTFIX_DICTGEN_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/tools/dictgen/*.cpp"
)
fastfix_add_binary(fastfix-dictgen ${FASTFIX_DICTGEN_SOURCES})

file(GLOB FASTFIX_XML2FFD_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/tools/xml2ffd/*.cpp"
)
add_executable(fastfix-xml2ffd ${FASTFIX_XML2FFD_SOURCES})
target_link_libraries(fastfix-xml2ffd PRIVATE fastfix fastfix-thirdparty-pugixml)

if(FASTFIX_BUILD_TOOLS)
    file(GLOB FASTFIX_INTEROP_RUNNER_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/interop-runner/*.cpp"
    )
    fastfix_add_binary(fastfix-interop-runner ${FASTFIX_INTEROP_RUNNER_SOURCES})

    file(GLOB FASTFIX_SOAK_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/soak/*.cpp"
    )
    fastfix_add_binary(fastfix-soak ${FASTFIX_SOAK_SOURCES})

    file(GLOB FASTFIX_INITIATOR_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/initiator/*.cpp"
    )
    fastfix_add_binary(fastfix-initiator ${FASTFIX_INITIATOR_SOURCES})

    file(GLOB FASTFIX_ACCEPTOR_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/acceptor/*.cpp"
    )
    fastfix_add_binary(fastfix-acceptor ${FASTFIX_ACCEPTOR_SOURCES})
endif()

if(FASTFIX_BUILD_FUZZERS)
    file(GLOB FASTFIX_FUZZ_CONFIG_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/fuzz-config/*.cpp"
    )
    fastfix_add_binary(fastfix-fuzz-config ${FASTFIX_FUZZ_CONFIG_SOURCES})

    file(GLOB FASTFIX_FUZZ_DICTGEN_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/fuzz-dictgen/*.cpp"
    )
    fastfix_add_binary(fastfix-fuzz-dictgen ${FASTFIX_FUZZ_DICTGEN_SOURCES})

    add_executable(fastfix-fuzz-codec
        "${PROJECT_SOURCE_DIR}/tools/fuzz-codec/main.cpp"
    )
    target_link_libraries(fastfix-fuzz-codec PRIVATE fastfix)
endif()

if(FASTFIX_BUILD_LIBFUZZER)
    add_executable(fastfix-fuzz-codec-libfuzzer
        "${PROJECT_SOURCE_DIR}/tools/fuzz-codec/fuzz_entry.cpp"
    )
    target_link_libraries(fastfix-fuzz-codec-libfuzzer PRIVATE fastfix)
    target_compile_options(fastfix-fuzz-codec-libfuzzer PRIVATE -fsanitize=fuzzer)
    target_link_options(fastfix-fuzz-codec-libfuzzer PRIVATE -fsanitize=fuzzer)
endif()

add_custom_command(
    OUTPUT
        "${FASTFIX_FIX44_FFD}"
        "${FASTFIX_FIX44_ART}"
        "${FASTFIX_FIX44_BUILDERS}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${FASTFIX_BENCH_OUTPUT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${FASTFIX_GENERATED_DIR}"
    COMMAND $<TARGET_FILE:fastfix-xml2ffd>
        --xml "${FASTFIX_FIX44_XML}"
        --output "${FASTFIX_FIX44_FFD}"
        --profile-id 4400
    COMMAND $<TARGET_FILE:fastfix-dictgen>
        --input "${FASTFIX_FIX44_FFD}"
        --output "${FASTFIX_FIX44_ART}"
        --cpp-builders "${FASTFIX_FIX44_BUILDERS}"
    DEPENDS
        fastfix-xml2ffd
        fastfix-dictgen
        "${FASTFIX_FIX44_XML}"
    VERBATIM
)
add_custom_target(fastfix-fix44-assets
    DEPENDS
        "${FASTFIX_FIX44_FFD}"
        "${FASTFIX_FIX44_ART}"
        "${FASTFIX_FIX44_BUILDERS}"
)

add_custom_command(
    OUTPUT
        "${FASTFIX_SAMPLE_ART}"
        "${FASTFIX_SAMPLE_BUILDERS}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${FASTFIX_OUTPUT_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${FASTFIX_GENERATED_DIR}"
    COMMAND $<TARGET_FILE:fastfix-dictgen>
        --input "${PROJECT_SOURCE_DIR}/samples/basic_profile.ffd"
        --merge "${PROJECT_SOURCE_DIR}/samples/basic_overlay.ffd"
        --output "${FASTFIX_SAMPLE_ART}"
        --cpp-builders "${FASTFIX_SAMPLE_BUILDERS}"
    DEPENDS
        fastfix-dictgen
        "${PROJECT_SOURCE_DIR}/samples/basic_profile.ffd"
        "${PROJECT_SOURCE_DIR}/samples/basic_overlay.ffd"
    VERBATIM
)
add_custom_target(fastfix-sample-assets
    DEPENDS
        "${FASTFIX_SAMPLE_ART}"
        "${FASTFIX_SAMPLE_BUILDERS}"
)

add_custom_target(fastfix-generated-assets
    DEPENDS
        fastfix-fix44-assets
        fastfix-sample-assets
)

if(FASTFIX_BUILD_BENCHMARKS)
    add_executable(fastfix-bench
        "${PROJECT_SOURCE_DIR}/bench/main.cpp"
    )
    target_link_libraries(fastfix-bench PRIVATE fastfix)
    target_include_directories(fastfix-bench PRIVATE "${FASTFIX_GENERATED_DIR}")
    add_dependencies(fastfix-bench fastfix-fix44-assets)

    if(WIN32)
        message(FATAL_ERROR "The QuickFIX comparison benchmark target currently supports Unix-like builds only")
    endif()

    set(FASTFIX_VENDOR_QUICKFIX_SOURCES
        Acceptor.cpp
        DataDictionary.cpp
        DataDictionaryProvider.cpp
        Dictionary.cpp
        FieldConvertors.cpp
        FieldMap.cpp
        FieldTypes.cpp
        FileLog.cpp
        FileStore.cpp
        Group.cpp
        HostDetailsProvider.cpp
        HttpConnection.cpp
        HttpMessage.cpp
        HttpParser.cpp
        HttpServer.cpp
        Initiator.cpp
        Log.cpp
        Message.cpp
        MessageSorters.cpp
        MessageStore.cpp
        NullStore.cpp
        Parser.cpp
        PUGIXML_DOMDocument.cpp
        Session.cpp
        SessionFactory.cpp
        SessionSettings.cpp
        Settings.cpp
        SocketAcceptor.cpp
        SocketConnection.cpp
        SocketConnector.cpp
        SocketInitiator.cpp
        SocketMonitor_UNIX.cpp
        SocketServer.cpp
        stdafx.cpp
        ThreadedSocketAcceptor.cpp
        ThreadedSocketConnection.cpp
        ThreadedSocketInitiator.cpp
        TimeRange.cpp
        Utility.cpp
        pugixml.cpp
    )
    list(TRANSFORM FASTFIX_VENDOR_QUICKFIX_SOURCES PREPEND "${FASTFIX_QUICKFIX_ROOT}/src/C++/")

    add_library(fastfix-vendor-quickfix STATIC ${FASTFIX_VENDOR_QUICKFIX_SOURCES})
    target_compile_definitions(fastfix-vendor-quickfix PRIVATE NOMINMAX _FILE_OFFSET_BITS=64)
    target_compile_features(fastfix-vendor-quickfix PUBLIC cxx_std_17)
    target_include_directories(fastfix-vendor-quickfix
        PUBLIC
            "${FASTFIX_QUICKFIX_OVERRIDE_DIR}"
            "${FASTFIX_QUICKFIX_ROOT}/src/C++"
        PRIVATE
            "${FASTFIX_QUICKFIX_ROOT}"
            "${FASTFIX_QUICKFIX_ROOT}/src"
    )
    target_link_libraries(fastfix-vendor-quickfix PUBLIC Threads::Threads)

    add_executable(fastfix-quickfix-cpp-bench
        "${PROJECT_SOURCE_DIR}/bench/quickfix_main.cpp"
    )
    set_target_properties(fastfix-quickfix-cpp-bench PROPERTIES OUTPUT_NAME "quickfix-cpp-bench")
    target_include_directories(fastfix-quickfix-cpp-bench
        PRIVATE
            "${PROJECT_SOURCE_DIR}/bench"
    )
    target_link_libraries(fastfix-quickfix-cpp-bench PRIVATE fastfix-vendor-quickfix Threads::Threads)
endif()

if(FASTFIX_BUILD_TESTS)
    file(GLOB FASTFIX_TEST_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tests/*.cpp"
    )
    add_executable(fastfix-tests
        ${FASTFIX_TEST_SOURCES}
        "${PROJECT_SOURCE_DIR}/tools/xml2ffd/xml2ffd.cpp"
    )
    target_include_directories(fastfix-tests
        PRIVATE
            "${FASTFIX_GENERATED_DIR}"
            "${PROJECT_SOURCE_DIR}/tests/generated"
    )
    target_link_libraries(fastfix-tests
        PRIVATE
            fastfix
            fastfix-thirdparty-catch2-main
            fastfix-thirdparty-pugixml
    )
    add_dependencies(fastfix-tests fastfix-generated-assets)
    add_test(NAME fastfix-tests COMMAND fastfix-tests)
endif()

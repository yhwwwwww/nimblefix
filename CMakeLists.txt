cmake_minimum_required(VERSION 3.20)

project(nimblefix VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(NIMBLEFIX_BUILD_TOOLS "Build NimbleFIX command-line tools" ON)
option(NIMBLEFIX_BUILD_TESTS "Build the NimbleFIX Catch2 test suite" ON)
option(NIMBLEFIX_BUILD_BENCHMARKS "Build the NimbleFIX and QuickFIX benchmark drivers" ON)
option(NIMBLEFIX_BUILD_FUZZERS "Build the standalone fuzz corpus runners" ON)
option(NIMBLEFIX_BUILD_LIBFUZZER "Build the libFuzzer entrypoint" OFF)
option(NIMBLEFIX_ENABLE_LIBURING "Enable io_uring when liburing headers and library are available" ON)
option(NIMBLEFIX_ENABLE_TLS "Enable optional TLS support via OpenSSL" OFF)

include(CTest)
include(GNUInstallDirs)
include(CheckIncludeFileCXX)

find_package(Threads REQUIRED)

if(NIMBLEFIX_ENABLE_TLS)
    find_package(OpenSSL REQUIRED)
    message(STATUS "NimbleFIX: enabling optional TLS support via OpenSSL")
else()
    message(STATUS "NimbleFIX: building without optional TLS support")
endif()

set(NIMBLEFIX_PROJECT_ROOT "${PROJECT_SOURCE_DIR}")
set(NIMBLEFIX_PUBLIC_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include/public")
set(NIMBLEFIX_INTERNAL_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include/internal")
set(NIMBLEFIX_VENDOR_HINT "Run 'git submodule update --init --recursive'.")
set(NIMBLEFIX_CATCH2_ROOT "${PROJECT_SOURCE_DIR}/deps/src/Catch2")
set(NIMBLEFIX_CATCH2_OVERRIDE_DIR "${PROJECT_SOURCE_DIR}/deps/include")
set(NIMBLEFIX_PUGIXML_ROOT "${PROJECT_SOURCE_DIR}/deps/src/pugixml")
set(NIMBLEFIX_QUICKFIX_ROOT "${PROJECT_SOURCE_DIR}/bench/vendor/quickfix")
set(NIMBLEFIX_QUICKFIX_OVERRIDE_DIR "${PROJECT_SOURCE_DIR}/deps/include/quickfix")
set(NIMBLEFIX_GENERATED_DIR "${NIMBLEFIX_PROJECT_ROOT}/build/generated")
set(NIMBLEFIX_OUTPUT_ROOT "${NIMBLEFIX_PROJECT_ROOT}/build")
set(NIMBLEFIX_BENCH_OUTPUT_DIR "${NIMBLEFIX_OUTPUT_ROOT}/bench")
set(NIMBLEFIX_FIX44_XML "${NIMBLEFIX_QUICKFIX_ROOT}/spec/FIX44.xml")
set(NIMBLEFIX_FIX44_NFD "${NIMBLEFIX_BENCH_OUTPUT_DIR}/quickfix_FIX44.nfd")
set(NIMBLEFIX_FIX44_NFA "${NIMBLEFIX_BENCH_OUTPUT_DIR}/quickfix_FIX44.nfa")
set(NIMBLEFIX_FIX44_API "${NIMBLEFIX_GENERATED_DIR}/fix44_api.h")
set(NIMBLEFIX_SAMPLE_NFA "${NIMBLEFIX_OUTPUT_ROOT}/sample-basic.nfa")
set(NIMBLEFIX_SAMPLE_API "${NIMBLEFIX_GENERATED_DIR}/sample_basic_api.h")

file(MAKE_DIRECTORY "${NIMBLEFIX_GENERATED_DIR}")
file(MAKE_DIRECTORY "${NIMBLEFIX_BENCH_OUTPUT_DIR}")

function(nimblefix_require_checkout required_path label)
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Missing ${label} at ${required_path}. ${NIMBLEFIX_VENDOR_HINT}")
    endif()
endfunction()

nimblefix_require_checkout("${NIMBLEFIX_CATCH2_ROOT}/extras/catch_amalgamated.cpp" "Catch2 submodule checkout")
nimblefix_require_checkout("${NIMBLEFIX_CATCH2_OVERRIDE_DIR}/catch2/catch_user_config.hpp" "Catch2 override header")
nimblefix_require_checkout("${NIMBLEFIX_PUGIXML_ROOT}/src/pugixml.cpp" "pugixml submodule checkout")
nimblefix_require_checkout("${NIMBLEFIX_QUICKFIX_ROOT}/spec/FIX44.xml" "QuickFIX submodule checkout")
nimblefix_require_checkout("${NIMBLEFIX_QUICKFIX_OVERRIDE_DIR}/Allocator.h" "QuickFIX override header")

check_include_file_cxx("liburing.h" NIMBLEFIX_HAVE_LIBURING_HEADER)
find_library(NIMBLEFIX_URING_LIBRARY NAMES uring)

if(NIMBLEFIX_ENABLE_LIBURING AND NIMBLEFIX_HAVE_LIBURING_HEADER AND NIMBLEFIX_URING_LIBRARY)
    set(NIMBLEFIX_USE_LIBURING ON)
    message(STATUS "NimbleFIX: enabling io_uring backend")
else()
    set(NIMBLEFIX_USE_LIBURING OFF)
    message(STATUS "NimbleFIX: building without io_uring backend")
endif()

add_library(nimblefix-thirdparty-pugixml STATIC
    "${NIMBLEFIX_PUGIXML_ROOT}/src/pugixml.cpp"
)
target_include_directories(nimblefix-thirdparty-pugixml
    PUBLIC
        "${NIMBLEFIX_PUGIXML_ROOT}/src"
)

add_library(nimblefix-thirdparty-catch2-main STATIC
    "${NIMBLEFIX_CATCH2_ROOT}/extras/catch_amalgamated.cpp"
)
target_include_directories(nimblefix-thirdparty-catch2-main
    PUBLIC
        "${NIMBLEFIX_CATCH2_OVERRIDE_DIR}"
        "${NIMBLEFIX_CATCH2_ROOT}/src"
    PRIVATE
        "${NIMBLEFIX_CATCH2_ROOT}/extras"
)
target_compile_features(nimblefix-thirdparty-catch2-main PUBLIC cxx_std_14)

file(GLOB NIMBLEFIX_LIBRARY_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/profile/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/session/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/store/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/message/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/codec/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/transport/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/tools/msgdump/*.cpp"
)

add_library(nimblefix STATIC ${NIMBLEFIX_LIBRARY_SOURCES})
target_include_directories(nimblefix
    PUBLIC
        "${NIMBLEFIX_PUBLIC_INCLUDE_DIR}"
    PRIVATE
        "${NIMBLEFIX_INTERNAL_INCLUDE_DIR}"
)
target_compile_definitions(nimblefix
    PUBLIC
        NIMBLEFIX_PROJECT_DIR=\"${NIMBLEFIX_PROJECT_ROOT}\"
)
target_link_libraries(nimblefix PUBLIC Threads::Threads)

if(NIMBLEFIX_ENABLE_TLS)
    target_compile_definitions(nimblefix PUBLIC NIMBLEFIX_ENABLE_TLS)
    target_link_libraries(nimblefix PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

if(NIMBLEFIX_USE_LIBURING)
    target_link_libraries(nimblefix PUBLIC "${NIMBLEFIX_URING_LIBRARY}")
else()
    target_compile_definitions(nimblefix PRIVATE NIMBLEFIX_DISABLE_LIBURING)
endif()

function(nimblefix_add_binary target_name)
    add_executable(${target_name} ${ARGN})
    target_link_libraries(${target_name} PRIVATE nimblefix)
    target_include_directories(${target_name}
        PRIVATE
            "${NIMBLEFIX_PUBLIC_INCLUDE_DIR}"
            "${NIMBLEFIX_INTERNAL_INCLUDE_DIR}"
    )
endfunction()

function(nimblefix_add_internal_headers target_name)
    target_include_directories(${target_name}
        PRIVATE
            "${NIMBLEFIX_PUBLIC_INCLUDE_DIR}"
            "${NIMBLEFIX_INTERNAL_INCLUDE_DIR}"
    )
endfunction()

install(TARGETS nimblefix
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(DIRECTORY "${NIMBLEFIX_PUBLIC_INCLUDE_DIR}/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

file(GLOB NIMBLEFIX_DICTGEN_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/tools/dictgen/*.cpp"
)
nimblefix_add_binary(nimblefix-dictgen ${NIMBLEFIX_DICTGEN_SOURCES})

file(GLOB NIMBLEFIX_XML2NFD_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/tools/xml2nfd/*.cpp"
)
add_executable(nimblefix-xml2nfd ${NIMBLEFIX_XML2NFD_SOURCES})
target_link_libraries(nimblefix-xml2nfd PRIVATE nimblefix nimblefix-thirdparty-pugixml)
nimblefix_add_internal_headers(nimblefix-xml2nfd)

if(NIMBLEFIX_BUILD_TOOLS)
    file(GLOB NIMBLEFIX_ORCHESTRA_IMPORT_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/orchestra-import/*.cpp"
    )
    add_executable(nimblefix-orchestra-import ${NIMBLEFIX_ORCHESTRA_IMPORT_SOURCES})
    target_link_libraries(nimblefix-orchestra-import PRIVATE nimblefix nimblefix-thirdparty-pugixml)
    nimblefix_add_internal_headers(nimblefix-orchestra-import)

    file(GLOB NIMBLEFIX_FIX_SESSION_TESTCASE_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/fix-session-testcases/*.cpp"
    )
    nimblefix_add_binary(nimblefix-fix-session-testcases ${NIMBLEFIX_FIX_SESSION_TESTCASE_SOURCES})

    file(GLOB NIMBLEFIX_INTEROP_RUNNER_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/interop-runner/*.cpp"
    )
    nimblefix_add_binary(nimblefix-interop-runner ${NIMBLEFIX_INTEROP_RUNNER_SOURCES})

    file(GLOB NIMBLEFIX_SOAK_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/soak/*.cpp"
    )
    nimblefix_add_binary(nimblefix-soak ${NIMBLEFIX_SOAK_SOURCES})

    file(GLOB NIMBLEFIX_INITIATOR_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/initiator/*.cpp"
    )
    nimblefix_add_binary(nimblefix-initiator ${NIMBLEFIX_INITIATOR_SOURCES})

    file(GLOB NIMBLEFIX_ACCEPTOR_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/acceptor/*.cpp"
    )
    nimblefix_add_binary(nimblefix-acceptor ${NIMBLEFIX_ACCEPTOR_SOURCES})

    file(GLOB NIMBLEFIX_MSGDUMP_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/msgdump/*.cpp"
    )
    nimblefix_add_binary(nimblefix-msgdump ${NIMBLEFIX_MSGDUMP_SOURCES})
endif()

if(NIMBLEFIX_BUILD_FUZZERS)
    file(GLOB NIMBLEFIX_FUZZ_CONFIG_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/fuzz-config/*.cpp"
    )
    nimblefix_add_binary(nimblefix-fuzz-config ${NIMBLEFIX_FUZZ_CONFIG_SOURCES})

    file(GLOB NIMBLEFIX_FUZZ_DICTGEN_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/fuzz-dictgen/*.cpp"
    )
    nimblefix_add_binary(nimblefix-fuzz-dictgen ${NIMBLEFIX_FUZZ_DICTGEN_SOURCES})

    add_executable(nimblefix-fuzz-codec
        "${PROJECT_SOURCE_DIR}/tools/fuzz-codec/main.cpp"
    )
    target_link_libraries(nimblefix-fuzz-codec PRIVATE nimblefix)
    nimblefix_add_internal_headers(nimblefix-fuzz-codec)
endif()

if(NIMBLEFIX_BUILD_LIBFUZZER)
    add_executable(nimblefix-fuzz-codec-libfuzzer
        "${PROJECT_SOURCE_DIR}/tools/fuzz-codec/fuzz_entry.cpp"
    )
    target_link_libraries(nimblefix-fuzz-codec-libfuzzer PRIVATE nimblefix)
    nimblefix_add_internal_headers(nimblefix-fuzz-codec-libfuzzer)
    target_compile_options(nimblefix-fuzz-codec-libfuzzer PRIVATE -fsanitize=fuzzer)
    target_link_options(nimblefix-fuzz-codec-libfuzzer PRIVATE -fsanitize=fuzzer)
endif()

add_custom_command(
    OUTPUT
        "${NIMBLEFIX_FIX44_NFD}"
        "${NIMBLEFIX_FIX44_NFA}"
        "${NIMBLEFIX_FIX44_API}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${NIMBLEFIX_BENCH_OUTPUT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${NIMBLEFIX_GENERATED_DIR}"
    COMMAND $<TARGET_FILE:nimblefix-xml2nfd>
        --xml "${NIMBLEFIX_FIX44_XML}"
        --output "${NIMBLEFIX_FIX44_NFD}"
        --profile-id 4400
    COMMAND $<TARGET_FILE:nimblefix-dictgen>
        --input "${NIMBLEFIX_FIX44_NFD}"
        --output "${NIMBLEFIX_FIX44_NFA}"
        --cpp-api "${NIMBLEFIX_FIX44_API}"
    DEPENDS
        nimblefix-xml2nfd
        nimblefix-dictgen
        "${NIMBLEFIX_FIX44_XML}"
    VERBATIM
)
add_custom_target(nimblefix-fix44-assets
    DEPENDS
        "${NIMBLEFIX_FIX44_NFD}"
        "${NIMBLEFIX_FIX44_NFA}"
        "${NIMBLEFIX_FIX44_API}"
)

add_custom_command(
    OUTPUT
        "${NIMBLEFIX_SAMPLE_NFA}"
        "${NIMBLEFIX_SAMPLE_API}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${NIMBLEFIX_OUTPUT_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${NIMBLEFIX_GENERATED_DIR}"
    COMMAND $<TARGET_FILE:nimblefix-dictgen>
        --input "${PROJECT_SOURCE_DIR}/samples/basic_profile.nfd"
        --merge "${PROJECT_SOURCE_DIR}/samples/basic_overlay.nfd"
        --output "${NIMBLEFIX_SAMPLE_NFA}"
        --cpp-api "${NIMBLEFIX_SAMPLE_API}"
    DEPENDS
        nimblefix-dictgen
        "${PROJECT_SOURCE_DIR}/samples/basic_profile.nfd"
        "${PROJECT_SOURCE_DIR}/samples/basic_overlay.nfd"
    VERBATIM
)
add_custom_target(nimblefix-sample-assets
    DEPENDS
        "${NIMBLEFIX_SAMPLE_NFA}"
        "${NIMBLEFIX_SAMPLE_API}"
)

add_custom_target(nimblefix-generated-assets
    DEPENDS
        nimblefix-fix44-assets
        nimblefix-sample-assets
)

if(NIMBLEFIX_BUILD_BENCHMARKS)
    add_executable(nimblefix-bench
        "${PROJECT_SOURCE_DIR}/bench/main.cpp"
    )
    target_link_libraries(nimblefix-bench PRIVATE nimblefix)
    target_include_directories(nimblefix-bench PRIVATE "${NIMBLEFIX_GENERATED_DIR}")
    nimblefix_add_internal_headers(nimblefix-bench)
    add_dependencies(nimblefix-bench nimblefix-fix44-assets)

    if(WIN32)
        message(FATAL_ERROR "The QuickFIX comparison benchmark target currently supports Unix-like builds only")
    endif()

    set(NIMBLEFIX_VENDOR_QUICKFIX_SOURCES
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
    list(TRANSFORM NIMBLEFIX_VENDOR_QUICKFIX_SOURCES PREPEND "${NIMBLEFIX_QUICKFIX_ROOT}/src/C++/")

    add_library(nimblefix-vendor-quickfix STATIC ${NIMBLEFIX_VENDOR_QUICKFIX_SOURCES})
    target_compile_definitions(nimblefix-vendor-quickfix PRIVATE NOMINMAX _FILE_OFFSET_BITS=64)
    target_compile_features(nimblefix-vendor-quickfix PUBLIC cxx_std_17)
    target_include_directories(nimblefix-vendor-quickfix
        PUBLIC
            "${NIMBLEFIX_QUICKFIX_OVERRIDE_DIR}"
            "${NIMBLEFIX_QUICKFIX_ROOT}/src/C++"
        PRIVATE
            "${NIMBLEFIX_QUICKFIX_ROOT}"
            "${NIMBLEFIX_QUICKFIX_ROOT}/src"
    )
    target_link_libraries(nimblefix-vendor-quickfix PUBLIC Threads::Threads)

    add_executable(nimblefix-quickfix-cpp-bench
        "${PROJECT_SOURCE_DIR}/bench/quickfix_main.cpp"
    )
    set_target_properties(nimblefix-quickfix-cpp-bench PROPERTIES OUTPUT_NAME "quickfix-cpp-bench")
    target_include_directories(nimblefix-quickfix-cpp-bench
        PRIVATE
            "${PROJECT_SOURCE_DIR}/bench"
    )
    nimblefix_add_internal_headers(nimblefix-quickfix-cpp-bench)
    target_link_libraries(nimblefix-quickfix-cpp-bench PRIVATE nimblefix-vendor-quickfix Threads::Threads)

    # Two-process loopback benchmark: NimbleFIX
    add_executable(nimblefix-loopback
        "${PROJECT_SOURCE_DIR}/bench/nimblefix_loopback.cpp"
    )
    target_link_libraries(nimblefix-loopback PRIVATE nimblefix)
    target_include_directories(nimblefix-loopback PRIVATE "${NIMBLEFIX_GENERATED_DIR}")
    nimblefix_add_internal_headers(nimblefix-loopback)
    add_dependencies(nimblefix-loopback nimblefix-fix44-assets)

    # Raw TCP RTT benchmark (no FIX protocol)
    add_executable(tcp-rtt
        "${PROJECT_SOURCE_DIR}/bench/tcp_rtt.cpp"
    )
    target_link_libraries(tcp-rtt PRIVATE Threads::Threads)

    # Transport RTT benchmark: plain TCP baseline plus optional TLS run.
    add_executable(nimblefix-tls-transport-bench
        "${PROJECT_SOURCE_DIR}/bench/tls_transport.cpp"
    )
    target_link_libraries(nimblefix-tls-transport-bench PRIVATE nimblefix Threads::Threads)

    # Two-process loopback benchmark: QuickFIX
    add_executable(quickfix-loopback
        "${PROJECT_SOURCE_DIR}/bench/quickfix_loopback.cpp"
    )
    target_include_directories(quickfix-loopback
        PRIVATE
            "${PROJECT_SOURCE_DIR}/bench"
    )
    target_link_libraries(quickfix-loopback PRIVATE nimblefix-vendor-quickfix Threads::Threads)
endif()

if(NIMBLEFIX_BUILD_TESTS)
    file(GLOB NIMBLEFIX_TEST_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tests/*.cpp"
    )
    add_executable(nimblefix-tests
        ${NIMBLEFIX_TEST_SOURCES}
        "${PROJECT_SOURCE_DIR}/tools/xml2nfd/xml2nfd.cpp"
        "${PROJECT_SOURCE_DIR}/tools/orchestra-import/orchestra_import.cpp"
    )
    target_include_directories(nimblefix-tests
        PRIVATE
            "${NIMBLEFIX_GENERATED_DIR}"
            "${PROJECT_SOURCE_DIR}/tests/generated"
            "${PROJECT_SOURCE_DIR}/tools/orchestra-import"
    )
    target_link_libraries(nimblefix-tests
        PRIVATE
            nimblefix
            nimblefix-thirdparty-catch2-main
            nimblefix-thirdparty-pugixml
    )
    nimblefix_add_internal_headers(nimblefix-tests)
    add_dependencies(nimblefix-tests nimblefix-generated-assets)
    add_test(NAME nimblefix-tests COMMAND nimblefix-tests)
endif()

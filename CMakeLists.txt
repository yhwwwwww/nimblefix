cmake_minimum_required(VERSION 3.23)
project(NimbleFIX VERSION 0.3.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(NIMBLEFIX_BUILD_TESTS "Build contract tests" ON)
option(NIMBLEFIX_BUILD_EXAMPLES "Build codec and TCP examples" ON)
option(NIMBLEFIX_BUILD_BENCHMARKS "Build codec, session and TCP benchmarks" OFF)
option(NIMBLEFIX_BUILD_SAMPLE_SCHEMA "Generate the optional sample FIX44 schema" ON)
option(NIMBLEFIX_BUILD_STANDARD_SCHEMAS "Generate complete standard FIX bindings" ${NIMBLEFIX_BUILD_SAMPLE_SCHEMA})
option(NIMBLEFIX_BUILD_CLI "Build optional standalone nimblefix CLI application" ON)
option(NIMBLEFIX_ENABLE_TLS "Enable system OpenSSL nonblocking TLS" OFF)
option(NIMBLEFIX_ENABLE_IO_URING "Enable optional Linux io_uring Proactor I/O backend via system liburing" OFF)
option(NIMBLEFIX_ENABLE_XML "Build optional system libxml2 runtime XML component" OFF)
option(NIMBLEFIX_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(NIMBLEFIX_THREAD_SANITIZE "Enable ThreadSanitizer in a separate build" OFF)

# Layer 0: Lightweight initiator-first zero-overhead core (Codec + v2 Journal + SessionCore + TcpTransport).
file(GLOB core_store_sources CONFIGURE_DEPENDS src/store/*.cpp)
add_library(nimblefix_core STATIC
    src/codec/parser.cpp
    src/codec/encoder.cpp
    src/message/decimal.cpp
    ${core_store_sources}
    src/session/core.cpp
    src/session/settings.cpp
    src/transport/tcp.cpp)
find_package(Threads REQUIRED)
target_link_libraries(nimblefix_core PUBLIC Threads::Threads)
add_library(NimbleFIX::nimblefix_core ALIAS nimblefix_core)
target_compile_features(nimblefix_core PUBLIC cxx_std_20)
target_include_directories(nimblefix_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(nimblefix_core PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nimblefix_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Layer 0 + optional TLS: DirectTlsTransport + OpenSSL 3 wrapper (no Engine/Acceptor/Queued).
add_library(nimblefix_core_tls STATIC
    src/transport/tls.cpp
    src/transport/direct_tls.cpp)
target_link_libraries(nimblefix_core_tls PUBLIC nimblefix_core)
if(NIMBLEFIX_ENABLE_TLS)
    find_package(OpenSSL 1.1.1 REQUIRED)
    target_link_libraries(nimblefix_core_tls PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(nimblefix_core_tls PRIVATE NIMBLEFIX_ENABLE_TLS=1)
endif()
add_library(NimbleFIX::nimblefix_core_tls ALIAS nimblefix_core_tls)
target_compile_features(nimblefix_core_tls PUBLIC cxx_std_20)
set_target_properties(nimblefix_core_tls PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nimblefix_core_tls PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Layer 1/2: Full runtime library (SessionDriver + Engine + Acceptor + Queued + optional TLS).
file(GLOB runtime_sources CONFIGURE_DEPENDS src/runtime/*.cpp)
add_library(nimblefix STATIC
    src/session/driver.cpp
    ${runtime_sources})
target_link_libraries(nimblefix PUBLIC nimblefix_core_tls)
if(NIMBLEFIX_ENABLE_TLS)
    target_compile_definitions(nimblefix PRIVATE NIMBLEFIX_ENABLE_TLS=1)
endif()
if(NIMBLEFIX_ENABLE_IO_URING)
    find_path(LIBURING_INCLUDE_DIR liburing.h REQUIRED)
    find_library(LIBURING_LIBRARY uring REQUIRED)
    target_include_directories(nimblefix PRIVATE "${LIBURING_INCLUDE_DIR}")
    target_link_libraries(nimblefix PUBLIC
        "$<BUILD_INTERFACE:${LIBURING_LIBRARY}>"
        "$<INSTALL_INTERFACE:uring>")
    target_compile_definitions(nimblefix PRIVATE NIMBLEFIX_ENABLE_IO_URING=1)
endif()
add_library(NimbleFIX::nimblefix ALIAS nimblefix)
target_compile_features(nimblefix PUBLIC cxx_std_20)
set_target_properties(nimblefix PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nimblefix PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(NIMBLEFIX_SANITIZE)
    target_compile_options(nimblefix_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(nimblefix_core PUBLIC -fsanitize=address,undefined)
endif()
if(NIMBLEFIX_THREAD_SANITIZE)
    if(NIMBLEFIX_SANITIZE)
        message(FATAL_ERROR "ThreadSanitizer needs a separate configuration")
    endif()
    target_compile_options(nimblefix_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(nimblefix_core PUBLIC -fsanitize=thread)
endif()

add_executable(nimblefix-schema-gen tools/schema/generate.cpp)
add_executable(NimbleFIX::nimblefix-schema-gen ALIAS nimblefix-schema-gen)
target_compile_features(nimblefix-schema-gen PRIVATE cxx_std_20)
set_target_properties(nimblefix-schema-gen PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nimblefix-schema-gen PRIVATE -Wall -Wextra -Wpedantic)
endif()
set(NIMBLEFIX_SCHEMA_GENERATOR NimbleFIX::nimblefix-schema-gen)
include(cmake/NimbleFIXSchema.cmake)
if(NIMBLEFIX_BUILD_SAMPLE_SCHEMA)
    nimblefix_generate_schema(TARGET fix44_sample NAMESPACE nimblefix::fix44_sample
        HEADER nimblefix/fix44_sample.hpp XML_FILES "${CMAKE_CURRENT_SOURCE_DIR}/schemas/fix44-sample.xml")
    add_library(NimbleFIX::fix44_sample ALIAS fix44_sample)
    install(TARGETS fix44_sample EXPORT NimbleFIXTargets)
    install(FILES "${fix44_sample_HEADER}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/nimblefix")
endif()

if(NIMBLEFIX_BUILD_STANDARD_SCHEMAS)
    foreach(standard fix42 fix43 fix44 fixt11 fix50sp2)
        string(TOUPPER "${standard}" standard_xml)
        if(standard STREQUAL "fix50sp2")
            set(standard_xml_files
                "${CMAKE_CURRENT_SOURCE_DIR}/schemas/standard/FIXT11.xml"
                "${CMAKE_CURRENT_SOURCE_DIR}/schemas/standard/FIX50SP2.xml")
        else()
            set(standard_xml_files
                "${CMAKE_CURRENT_SOURCE_DIR}/schemas/standard/${standard_xml}.xml")
        endif()
        nimblefix_generate_schema(TARGET ${standard} NAMESPACE nimblefix::${standard}
            HEADER nimblefix/${standard}.hpp XML_FILES ${standard_xml_files})
        add_library(NimbleFIX::${standard} ALIAS ${standard})
        install(TARGETS ${standard} EXPORT NimbleFIXTargets)
        install(FILES "${${standard}_HEADER}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/nimblefix")
    endforeach()
endif()

if(NIMBLEFIX_ENABLE_XML)
    find_package(LibXml2 2.9 REQUIRED)
    add_library(nimblefix_xml STATIC src/schema/xml_schema.cpp)
    target_link_libraries(nimblefix_xml PUBLIC nimblefix PRIVATE LibXml2::LibXml2)
    add_library(NimbleFIX::nimblefix_xml ALIAS nimblefix_xml)
    install(TARGETS nimblefix_xml EXPORT NimbleFIXTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()

if(NIMBLEFIX_BUILD_CLI)
    add_executable(nimblefix-cli
        tools/nimblefix/main.cpp
        tools/nimblefix/config.cpp
        tools/nimblefix/builtin_fix42.cpp
        tools/nimblefix/builtin_fix43.cpp
        tools/nimblefix/builtin_fix44.cpp
        tools/nimblefix/builtin_fix50sp2.cpp)
    target_include_directories(nimblefix-cli PRIVATE tools/nimblefix third_party/tomlplusplus)
    set_target_properties(nimblefix-cli PROPERTIES OUTPUT_NAME nimblefix)
    if(NIMBLEFIX_BUILD_BENCHMARKS)
        target_compile_definitions(nimblefix-cli PRIVATE NIMBLEFIX_ENABLE_BENCH=1)
    endif()
    target_link_libraries(nimblefix-cli PRIVATE nimblefix)
    if(NIMBLEFIX_BUILD_STANDARD_SCHEMAS)
        target_link_libraries(nimblefix-cli PRIVATE fix42 fix43 fix44 fix50sp2)
        target_compile_definitions(nimblefix-cli PRIVATE NIMBLEFIX_BUILTIN_STANDARD_SCHEMAS=1)
    endif()
    if(NIMBLEFIX_ENABLE_XML)
        target_link_libraries(nimblefix-cli PRIVATE nimblefix_xml)
        target_compile_definitions(nimblefix-cli PRIVATE NIMBLEFIX_ENABLE_XML=1)
    endif()
    add_dependencies(nimblefix-cli nimblefix-schema-gen)
    install(TARGETS nimblefix-cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES third_party/tomlplusplus/LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/NimbleFIX/third-party")
    install(FILES third_party/tomlplusplus/LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/nimblefix/third-party/tomlplusplus")
endif()

if(NIMBLEFIX_BUILD_EXAMPLES)
    add_executable(codec-demo examples/codec.cpp)
    target_link_libraries(codec-demo PRIVATE nimblefix)
    add_executable(hello-fix tests/hello_fix.cpp)
    target_link_libraries(hello-fix PRIVATE nimblefix)
    if(NIMBLEFIX_BUILD_SAMPLE_SCHEMA)
        target_link_libraries(hello-fix PRIVATE fix44_sample)
        target_compile_definitions(hello-fix PRIVATE NIMBLEFIX_SAMPLE_SCHEMA=1)
    endif()
    foreach(role initiator acceptor)
        if(NIMBLEFIX_BUILD_STANDARD_SCHEMAS)
            add_executable(${role}-example examples/${role}.cpp)
        else()
            add_executable(${role}-example examples/flat_${role}.cpp)
        endif()
        target_link_libraries(${role}-example PRIVATE nimblefix)
        if(NIMBLEFIX_BUILD_STANDARD_SCHEMAS)
            target_link_libraries(${role}-example PRIVATE fix44)
        endif()
    endforeach()
    if(NIMBLEFIX_BUILD_SAMPLE_SCHEMA)
        add_executable(direct-initiator-example examples/direct_initiator.cpp)
        target_link_libraries(direct-initiator-example PRIVATE nimblefix_core fix44_sample)
        install(TARGETS direct-initiator-example RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
    install(TARGETS codec-demo hello-fix initiator-example acceptor-example
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

if(NIMBLEFIX_BUILD_TESTS)
    enable_testing()
    add_executable(initiator-probe tests/initiator_probe.cpp)
    target_link_libraries(initiator-probe PRIVATE nimblefix)
    add_executable(initiator_reconnect-test tests/initiator_reconnect_test.cpp)
    target_compile_features(initiator_reconnect-test PRIVATE cxx_std_20)
    add_test(NAME initiator-reconnect COMMAND initiator_reconnect-test
        --binary $<TARGET_FILE:initiator-probe> --mode inbound)
    set_tests_properties(initiator-reconnect PROPERTIES TIMEOUT 30)
    add_test(NAME outbound-reconnect COMMAND initiator_reconnect-test
        --binary $<TARGET_FILE:initiator-probe> --mode outbound)
    set_tests_properties(outbound-reconnect PROPERTIES TIMEOUT 30)
    foreach(name parser message contract generated journal session session_boundary session_limits network settings journal_reader journal_rotation protocol_core protocol_session protocol_security queued_runtime sender_schema schema_codec dynamic_message engine_features producer_lane queued_shutdown sender_expiry io_backend backpressure p1_features)
        if((name STREQUAL "contract" OR name STREQUAL "generated" OR name STREQUAL "sender_schema" OR name STREQUAL "io_backend" OR name STREQUAL "backpressure" OR name STREQUAL "p1_features") AND NOT NIMBLEFIX_BUILD_SAMPLE_SCHEMA)
            continue()
        endif()
        add_executable(${name}-test tests/${name}_test.cpp)
        target_link_libraries(${name}-test PRIVATE nimblefix)
        if(name STREQUAL "contract" OR name STREQUAL "generated" OR name STREQUAL "sender_schema" OR name STREQUAL "io_backend" OR name STREQUAL "backpressure" OR name STREQUAL "p1_features")
            target_link_libraries(${name}-test PRIVATE fix44_sample)
        endif()
        add_test(NAME ${name} COMMAND ${name}-test)
    endforeach()
    if(NIMBLEFIX_BUILD_STANDARD_SCHEMAS)
        add_executable(schema_nested_group-test tests/schema_nested_group_test.cpp)
        target_link_libraries(schema_nested_group-test PRIVATE nimblefix fix44)
        add_test(NAME schema_nested_group COMMAND schema_nested_group-test)

        add_executable(multi_version_typed-test tests/multi_version_typed_test.cpp)
        target_link_libraries(multi_version_typed-test PRIVATE nimblefix fix42 fix43 fix44 fix50sp2)
        if(NIMBLEFIX_ENABLE_XML)
            target_link_libraries(multi_version_typed-test PRIVATE nimblefix_xml)
            target_compile_definitions(multi_version_typed-test PRIVATE NIMBLEFIX_ENABLE_XML=1)
        endif()
        add_test(NAME multi_version_typed COMMAND multi_version_typed-test "${CMAKE_CURRENT_SOURCE_DIR}/schemas/standard")
        set_tests_properties(multi_version_typed PROPERTIES TIMEOUT 60)
    endif()
    if(NIMBLEFIX_BUILD_SAMPLE_SCHEMA)
        add_executable(direct_initiator-test tests/direct_initiator_test.cpp)
        target_link_libraries(direct_initiator-test PRIVATE nimblefix_core fix44_sample)
        if(NOT NIMBLEFIX_SANITIZE AND NOT NIMBLEFIX_THREAD_SANITIZE)
            target_compile_definitions(direct_initiator-test PRIVATE NIMBLEFIX_AUDIT_WRAPS=1)
            target_link_options(direct_initiator-test PRIVATE
                -Wl,--wrap=malloc -Wl,--wrap=pthread_mutex_lock
                -Wl,--wrap=fsync -Wl,--wrap=fdatasync -Wl,--wrap=msync)
        endif()
        add_test(NAME direct_initiator COMMAND direct_initiator-test)
    endif()
    if(NIMBLEFIX_ENABLE_XML)
        add_executable(xml-schema-test tests/xml_schema_test.cpp)
        target_link_libraries(xml-schema-test PRIVATE nimblefix_xml)
        add_test(NAME runtime-xml COMMAND xml-schema-test "${CMAKE_CURRENT_SOURCE_DIR}/schemas")
    endif()
    if(NIMBLEFIX_BUILD_CLI)
        add_executable(cli-test tests/cli_test.cpp)
        target_compile_features(cli-test PRIVATE cxx_std_20)
        add_test(NAME cli COMMAND cli-test
            --binary $<TARGET_FILE:nimblefix-cli> --root "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(cli PROPERTIES TIMEOUT 60)
        add_executable(protocol_versions-test tests/protocol_versions_test.cpp)
        target_compile_features(protocol_versions-test PRIVATE cxx_std_20)
        add_test(NAME protocol-versions COMMAND protocol_versions-test
            --binary $<TARGET_FILE:nimblefix-cli>)
        set_tests_properties(protocol-versions PROPERTIES TIMEOUT 90)
    endif()
    add_executable(queued-crash-probe tests/queued_crash_probe.cpp)
    target_link_libraries(queued-crash-probe PRIVATE nimblefix)
    add_executable(queued_crash-test tests/queued_crash_test.cpp)
    target_compile_features(queued_crash-test PRIVATE cxx_std_20)
    add_test(NAME queued-crash COMMAND queued_crash-test
        --binary $<TARGET_FILE:queued-crash-probe>)
    set_tests_properties(queued-crash PROPERTIES TIMEOUT 30)
    add_executable(xmake_schema-test tests/xmake_schema_test.cpp)
    target_compile_features(xmake_schema-test PRIVATE cxx_std_20)
    add_test(NAME xmake-schema COMMAND xmake_schema-test
        --rule "${CMAKE_CURRENT_SOURCE_DIR}/xmake/nimblefix-schema.lua"
        --generator $<TARGET_FILE:nimblefix-schema-gen>
        --include-dir "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set_tests_properties(xmake-schema PROPERTIES TIMEOUT 60)
    add_executable(install-test tests/install_test.cpp)
    target_compile_features(install-test PRIVATE cxx_std_20)
    add_executable(engine-tls-probe tests/engine_tls_probe.cpp)
    target_link_libraries(engine-tls-probe PRIVATE nimblefix)
    if(NIMBLEFIX_ENABLE_TLS)
        add_test(NAME engine-tls COMMAND engine-tls-probe)
        set_tests_properties(engine-tls PROPERTIES TIMEOUT 60)
    endif()
    add_executable(tls-transport-test tests/tls_transport_test.cpp)
    target_link_libraries(tls-transport-test PRIVATE nimblefix)
    if(NIMBLEFIX_ENABLE_TLS)
        add_test(NAME tls-transport COMMAND tls-transport-test)
        set_tests_properties(tls-transport PROPERTIES TIMEOUT 30)
    else()
        add_test(NAME tls-unavailable COMMAND tls-transport-test)
    endif()
    if(NIMBLEFIX_BUILD_SAMPLE_SCHEMA)
        add_executable(direct-tls-initiator-test tests/direct_tls_initiator_test.cpp)
        target_link_libraries(direct-tls-initiator-test PRIVATE nimblefix_core_tls fix44_sample)
        if(NIMBLEFIX_ENABLE_TLS)
            add_test(NAME direct-tls-initiator COMMAND direct-tls-initiator-test)
            set_tests_properties(direct-tls-initiator PROPERTIES TIMEOUT 30)
        else()
            add_test(NAME direct-tls-unavailable COMMAND direct-tls-initiator-test)
        endif()
    endif()
    add_executable(codegen-test tests/codegen_test.cpp)
    target_compile_features(codegen-test PRIVATE cxx_std_20)
    add_test(NAME xml-codegen COMMAND codegen-test
        --generator $<TARGET_FILE:nimblefix-schema-gen> --root "${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(compile_fail-test tests/compile_fail_test.cpp)
    target_compile_features(compile_fail-test PRIVATE cxx_std_20)
    add_test(NAME compile-fail COMMAND compile_fail-test
        --generator $<TARGET_FILE:nimblefix-schema-gen>
        --cxx "${CMAKE_CXX_COMPILER}" --root "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NIMBLEFIX_BUILD_EXAMPLES)
        add_test(NAME codec-demo COMMAND codec-demo --self-test)
        add_test(NAME hello-fix COMMAND hello-fix --self-test --restart)
        set_tests_properties(hello-fix PROPERTIES TIMEOUT 30)
        if(NIMBLEFIX_BUILD_SAMPLE_SCHEMA)
            add_test(NAME hello-fix-typed COMMAND hello-fix --self-test --typed --restart)
            set_tests_properties(hello-fix-typed PROPERTIES TIMEOUT 30)
        endif()
        add_executable(endpoint-test tests/endpoint_test.cpp)
        target_compile_features(endpoint-test PRIVATE cxx_std_20)
        add_test(NAME endpoint-examples COMMAND endpoint-test
            --acceptor $<TARGET_FILE:acceptor-example> --initiator $<TARGET_FILE:initiator-example>)
        set_tests_properties(endpoint-examples PROPERTIES TIMEOUT 40)
    endif()
endif()

set(NIMBLEFIX_QUICKFIX_DIR "" CACHE PATH "Optional installed QuickFIX prefix for independent interoperability")
option(NIMBLEFIX_ENABLE_QUICKFIX "Build bundled third_party/quickfix submodule for oracle tests and parity benchmarks" OFF)

set(NIMBLEFIX_HAVE_QUICKFIX OFF)
if(NIMBLEFIX_QUICKFIX_DIR)
    find_path(QUICKFIX_INCLUDE_DIR quickfix/Application.h PATHS "${NIMBLEFIX_QUICKFIX_DIR}/include" NO_DEFAULT_PATH REQUIRED)
    find_library(QUICKFIX_LIBRARY quickfix PATHS "${NIMBLEFIX_QUICKFIX_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
    find_package(OpenSSL REQUIRED)
    add_library(nimblefix_quickfix INTERFACE)
    target_include_directories(nimblefix_quickfix SYSTEM INTERFACE "${QUICKFIX_INCLUDE_DIR}")
    target_link_libraries(nimblefix_quickfix INTERFACE "${QUICKFIX_LIBRARY}" OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
    set(NIMBLEFIX_QUICKFIX_FIX44_XML "${NIMBLEFIX_QUICKFIX_DIR}/share/quickfix/FIX44.xml")
    set(NIMBLEFIX_HAVE_QUICKFIX ON)
elseif((NIMBLEFIX_ENABLE_QUICKFIX OR NIMBLEFIX_BUILD_BENCHMARKS) AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickfix/src/C++/Session.cpp")
    set(QUICKFIX_GEN_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/quickfix-include")
    file(MAKE_DIRECTORY "${QUICKFIX_GEN_INCLUDE}")
    if(NOT EXISTS "${QUICKFIX_GEN_INCLUDE}/Allocator.h")
        file(WRITE "${QUICKFIX_GEN_INCLUDE}/Allocator.h" "/* Generated out-of-source for bundled QuickFIX */\n")
    endif()
    if(NOT EXISTS "${QUICKFIX_GEN_INCLUDE}/quickfix")
        file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickfix/src/C++"
            "${QUICKFIX_GEN_INCLUDE}/quickfix" SYMBOLIC)
    endif()
    set(QUICKFIX_SUBMODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickfix/src/C++")
    add_library(nimblefix_quickfix STATIC
        "${QUICKFIX_SUBMODULE_DIR}/Acceptor.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/DataDictionary.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/DataDictionaryProvider.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/Dictionary.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/FieldConvertors.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/FieldMap.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/FieldTypes.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/FileLog.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/FileStore.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/Group.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/HostDetailsProvider.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/HttpConnection.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/HttpMessage.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/HttpParser.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/HttpServer.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/Initiator.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/Log.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/Message.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/MessageSorters.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/MessageStore.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/NullStore.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/Parser.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/pugixml.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/PUGIXML_DOMDocument.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/Session.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/SessionFactory.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/SessionSettings.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/Settings.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/SocketAcceptor.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/SocketConnection.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/SocketConnector.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/SocketInitiator.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/SocketMonitor_UNIX.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/SocketServer.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/stdafx.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/ThreadedSocketAcceptor.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/ThreadedSocketConnection.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/ThreadedSocketInitiator.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/TimeRange.cpp"
        "${QUICKFIX_SUBMODULE_DIR}/Utility.cpp")
    target_compile_features(nimblefix_quickfix PRIVATE cxx_std_17)
    target_compile_definitions(nimblefix_quickfix PRIVATE
        _FILE_OFFSET_BITS=64 GETHOSTBYNAME_R_INPUTS_RESULT=1 HAVE_CXX17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(nimblefix_quickfix PRIVATE -w)
    endif()
    target_include_directories(nimblefix_quickfix PRIVATE "${QUICKFIX_GEN_INCLUDE}" "${QUICKFIX_SUBMODULE_DIR}")
    target_include_directories(nimblefix_quickfix SYSTEM INTERFACE "${QUICKFIX_GEN_INCLUDE}")
    target_link_libraries(nimblefix_quickfix PUBLIC Threads::Threads)
    set(NIMBLEFIX_QUICKFIX_FIX44_XML "${CMAKE_CURRENT_SOURCE_DIR}/schemas/standard/FIX44.xml")
    set(NIMBLEFIX_HAVE_QUICKFIX ON)
endif()

if(NIMBLEFIX_HAVE_QUICKFIX AND NIMBLEFIX_BUILD_TESTS AND (NIMBLEFIX_QUICKFIX_DIR OR NIMBLEFIX_ENABLE_QUICKFIX))
    foreach(oracle quickfix quickfix_network quickfix_dual_role)
        if(oracle STREQUAL "quickfix_dual_role" AND NOT NIMBLEFIX_BUILD_STANDARD_SCHEMAS)
            continue()
        endif()
        add_executable(${oracle}-test tests/${oracle}_test.cpp)
        target_link_libraries(${oracle}-test PRIVATE nimblefix nimblefix_quickfix)
        if(oracle STREQUAL "quickfix_dual_role")
            target_link_libraries(${oracle}-test PRIVATE fix44)
        endif()
        add_test(NAME ${oracle} COMMAND ${oracle}-test "${NIMBLEFIX_QUICKFIX_FIX44_XML}")
        set_tests_properties(${oracle} PROPERTIES TIMEOUT 90)
    endforeach()
endif()

if(NIMBLEFIX_BUILD_BENCHMARKS)
    if(NOT NIMBLEFIX_BUILD_SAMPLE_SCHEMA)
        message(FATAL_ERROR "The comparison benchmark requires NIMBLEFIX_BUILD_SAMPLE_SCHEMA=ON")
    endif()
    if(NIMBLEFIX_HAVE_QUICKFIX)
        add_executable(quickfix-tcp-server bench/quickfix_tcp_server.cpp)
        target_compile_features(quickfix-tcp-server PRIVATE cxx_std_20)
        target_link_libraries(quickfix-tcp-server PRIVATE nimblefix_quickfix)
        if(NIMBLEFIX_QUICKFIX_DIR)
            find_package(ZLIB REQUIRED)
            target_link_libraries(quickfix-tcp-server PRIVATE ZLIB::ZLIB)
        endif()
        if(NIMBLEFIX_BUILD_STANDARD_SCHEMAS)
            add_executable(quickfix-parity-bench bench/quickfix_parity_bench.cpp)
            target_link_libraries(quickfix-parity-bench PRIVATE nimblefix fix44 nimblefix_quickfix)
            foreach(symbol malloc calloc realloc aligned_alloc posix_memalign)
                target_link_options(quickfix-parity-bench PRIVATE "-Wl,--wrap=${symbol}")
            endforeach()
            if(NIMBLEFIX_QUICKFIX_DIR)
                target_link_libraries(quickfix-parity-bench PRIVATE ZLIB::ZLIB)
            endif()
        endif()
    endif()
    if(NIMBLEFIX_BUILD_STANDARD_SCHEMAS)
        add_executable(schema-bench bench/schema_bench.cpp)
        target_link_libraries(schema-bench PRIVATE nimblefix fix44)
        target_link_options(schema-bench PRIVATE -Wl,--wrap=malloc)
    endif()
    add_executable(isolation-server bench/isolation_server.cpp)
    target_link_libraries(isolation-server PRIVATE nimblefix)
    add_executable(runtime-bench bench/runtime_bench.cpp)
    target_link_libraries(runtime-bench PRIVATE nimblefix)
    add_executable(journal-bench bench/journal_bench.cpp)
    target_link_libraries(journal-bench PRIVATE nimblefix)
    foreach(symbol malloc calloc realloc pthread_mutex_lock fsync fdatasync msync send)
        target_link_options(runtime-bench PRIVATE "-Wl,--wrap=${symbol}")
    endforeach()
    if(NIMBLEFIX_BUILD_TESTS)
        add_executable(bench-smoke-test bench/bench_smoke_test.cpp)
        target_compile_features(bench-smoke-test PRIVATE cxx_std_20)
        add_test(NAME journal-benchmark-smoke COMMAND bench-smoke-test
            --journal-bench $<TARGET_FILE:journal-bench>)
        set_tests_properties(journal-benchmark-smoke PROPERTIES TIMEOUT 60)
        add_test(NAME runtime-benchmark-smoke COMMAND bench-smoke-test
            --runtime-bench $<TARGET_FILE:runtime-bench>)
        set_tests_properties(runtime-benchmark-smoke PROPERTIES TIMEOUT 120)
        if(NIMBLEFIX_BUILD_STANDARD_SCHEMAS)
            add_test(NAME schema-benchmark-smoke COMMAND bench-smoke-test
                --schema-bench $<TARGET_FILE:schema-bench>)
            set_tests_properties(schema-benchmark-smoke PROPERTIES TIMEOUT 60)
        endif()
    endif()
    add_executable(codec-bench bench/codec_bench.cpp)
    target_link_libraries(codec-bench PRIVATE nimblefix fix44_sample)
    target_link_options(codec-bench PRIVATE -Wl,--wrap=malloc)
    if(NIMBLEFIX_BUILD_STANDARD_SCHEMAS)
        add_executable(direct-initiator-bench bench/direct_initiator_bench.cpp)
        target_link_libraries(direct-initiator-bench PRIVATE nimblefix_core fix44)
        add_executable(acceptor-bench bench/acceptor_bench.cpp)
        target_link_libraries(acceptor-bench PRIVATE nimblefix fix44)
        add_executable(initiator-burst-bench bench/initiator_burst_bench.cpp)
        target_link_libraries(initiator-burst-bench PRIVATE nimblefix fix44)
        foreach(bench_target direct-initiator-bench acceptor-bench initiator-burst-bench)
            foreach(symbol malloc calloc realloc aligned_alloc posix_memalign)
                target_link_options(${bench_target} PRIVATE "-Wl,--wrap=${symbol}")
            endforeach()
        endforeach()
    endif()
    add_executable(session-bench bench/session_bench.cpp)
    target_link_libraries(session-bench PRIVATE nimblefix)
    target_link_options(session-bench PRIVATE -Wl,--wrap=malloc -Wl,--wrap=fsync -Wl,--wrap=fdatasync -Wl,--wrap=msync)
    add_executable(tcp-bench-server bench/tcp_server.cpp)
    target_link_libraries(tcp-bench-server PRIVATE nimblefix)
    add_executable(null-tcp-server bench/null_tcp_server.cpp)
endif()

option(NIMBLEFIX_BUILD_FUZZERS "Build Clang libFuzzer codec/schema targets" OFF)
if(NIMBLEFIX_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "Fuzzers require Clang libFuzzer")
    endif()
    foreach(fuzzer parser schema)
        if(fuzzer STREQUAL "schema" AND NOT NIMBLEFIX_BUILD_STANDARD_SCHEMAS)
            continue()
        endif()
        add_executable(${fuzzer}-fuzz tests/fuzz_${fuzzer}.cpp src/codec/parser.cpp src/codec/encoder.cpp src/message/decimal.cpp)
        target_include_directories(${fuzzer}-fuzz PRIVATE include)
        target_compile_features(${fuzzer}-fuzz PRIVATE cxx_std_20)
        target_compile_options(${fuzzer}-fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
        target_link_options(${fuzzer}-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
        if(fuzzer STREQUAL "schema")
            target_link_libraries(${fuzzer}-fuzz PRIVATE fix44)
        endif()
    endforeach()
endif()

install(TARGETS nimblefix_core nimblefix_core_tls nimblefix nimblefix-schema-gen
    EXPORT NimbleFIXTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/nimblefix DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES xmake/nimblefix-schema.lua DESTINATION "${CMAKE_INSTALL_DATADIR}/nimblefix/xmake")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/NimbleFIX")
install(FILES third_party/quickfix-LICENSE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/NimbleFIX/third-party")
install(FILES third_party/README.md DESTINATION "${CMAKE_INSTALL_DATADIR}/nimblefix/third-party")
install(FILES third_party/quickfix-LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/nimblefix/third-party")
install(DIRECTORY schemas/standard DESTINATION "${CMAKE_INSTALL_DATADIR}/nimblefix/schemas")
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

set_project("nimblefix")
set_version("0.3.0")
set_xmakever("2.9.8")
set_languages("cxx20")
add_rules("mode.debug", "mode.release")
set_warnings("allextra")

option("tests")
    set_default(true)
    set_showmenu(true)
    set_description("Build codec, journal, session and TCP contract tests")
option_end()

option("quickfix_dir")
    set_showmenu(true)
    set_description("Optional installed QuickFIX prefix for independent oracle tests")
option_end()

option("quickfix")
    set_default(false)
    set_showmenu(true)
    set_description("Build bundled third_party/quickfix submodule for oracle tests and parity benchmarks")
option_end()

option("fuzzers")
    set_default(false)
    set_showmenu(true)
    set_description("Build the Clang libFuzzer parser target")
option_end()

option("benchmarks")
    set_default(false)
    set_showmenu(true)
    set_description("Build codec, appended session and TCP benchmarks")
option_end()

option("sanitize")
    set_default(false)
    set_showmenu(true)
    set_description("Enable AddressSanitizer and UndefinedBehaviorSanitizer")
option_end()

option("tls")
    set_default(false)
    set_showmenu(true)
    set_description("Enable OpenSSL nonblocking TLS (system OpenSSL, no downloads)")
option_end()

option("io_uring")
    set_default(false)
    set_showmenu(true)
    set_description("Enable optional Linux io_uring Proactor I/O backend (system liburing)")
option_end()

option("xml")
    set_default(false)
    set_showmenu(true)
    set_description("Build the optional runtime XML component (system libxml2)")
option_end()

option("cli")
    set_default(true)
    set_showmenu(true)
    set_description("Build optional standalone nimblefix CLI application")
option_end()

option("standard_schemas")
    set_default(true)
    set_showmenu(true)
    set_description("Generate complete standard FIX schema headers")
option_end()

option("thread_sanitize")
    set_default(false)
    set_showmenu(true)
    set_description("Enable ThreadSanitizer for lifecycle and concurrency checks")
option_end()

if has_config("sanitize") then
    add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", {force = true})
    add_ldflags("-fsanitize=address,undefined", {force = true})
end

if has_config("thread_sanitize") then
    if has_config("sanitize") then raise("ThreadSanitizer must use a separate configuration") end
    add_cxxflags("-fsanitize=thread", "-fno-omit-frame-pointer", {force = true})
    add_ldflags("-fsanitize=thread", {force = true})
end

local generated_dir = path.join(get_config("builddir") or get_config("buildir") or "build", "generated")
local sample_header = path.join(generated_dir, "nimblefix", "fix44_sample.hpp")

target("nimblefix-core")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_headerfiles("include/(nimblefix/**.hpp)")
    add_files("src/codec/parser.cpp", "src/codec/encoder.cpp", "src/message/decimal.cpp")
    add_files("src/store/*.cpp", "src/session/core.cpp", "src/session/settings.cpp", "src/transport/tcp.cpp")
    add_syslinks("pthread", {public = true})
target_end()

target("nimblefix-core-tls")
    set_kind("static")
    add_deps("nimblefix-core")
    add_includedirs("include", {public = true})
    add_files("src/transport/tls.cpp", "src/transport/direct_tls.cpp")
    if has_config("tls") then
        add_defines("NIMBLEFIX_ENABLE_TLS=1")
        add_syslinks("ssl", "crypto", {public = true})
    end
target_end()

target("nimblefix")
    set_kind("static")
    add_deps("nimblefix-core-tls")
    add_includedirs("include", {public = true})
    add_files("src/session/driver.cpp", "src/runtime/*.cpp")
    if has_config("tls") then
        add_defines("NIMBLEFIX_ENABLE_TLS=1")
    end
    if has_config("io_uring") then
        add_defines("NIMBLEFIX_ENABLE_IO_URING=1")
        add_syslinks("uring", {public = true})
    end
target_end()

target("nimblefix-schema-gen")
    set_kind("binary")
    set_policy("build.fence", true)
    add_files("tools/schema/generate.cpp")
target_end()

if has_config("standard_schemas") then
    for _, schema in ipairs({"fix42", "fix43", "fix44", "fixt11", "fix50sp2"}) do
        target(schema .. "-schema")
            set_kind("phony")
            set_policy("build.fence", true)
            set_default(false)
            add_deps("nimblefix-schema-gen")
            on_build(function (target)
                import("core.project.depend")
                import("core.project.project")
                local gen = project.target("nimblefix-schema-gen"):targetfile()
                local output = path.join(generated_dir, "nimblefix", schema .. ".hpp")
                local xml = path.join("schemas", "standard", schema:upper() .. ".xml")
                depend.on_changed(function ()
                    os.vrunv(gen, {"--output", output, "--namespace", "nimblefix::" .. schema, xml})
                end, {files = {xml, gen}, values = {output},
                      changed = not os.isfile(output)})
            end)
        target_end()
    end
end

if has_config("xml") then
    target("nimblefix-xml")
        set_kind("static")
        add_files("src/schema/xml_schema.cpp")
        add_deps("nimblefix")
        on_load(function (target)
            import("lib.detect.find_package")
            local package = find_package("pkgconfig::libxml-2.0")
            if not package then raise("--xml=y requires the system libxml2 development package") end
            target:add("includedirs", package.includedirs or {})
            target:add("linkdirs", package.linkdirs or {}, {public = true})
            target:add("syslinks", package.links or {"xml2"}, {public = true})
        end)
    target_end()
end

if has_config("cli") then
    target("nimblefix-cli")
        set_kind("binary")
        set_basename("nimblefix")
        set_rundir("$(projectdir)")
        add_files("tools/nimblefix/main.cpp", "tools/nimblefix/config.cpp")
        add_includedirs("tools/nimblefix", "third_party/tomlplusplus")
        if has_config("benchmarks") then add_defines("NIMBLEFIX_ENABLE_BENCH=1") end
        add_deps("nimblefix")
        if has_config("standard_schemas") then
            add_deps("fix44-schema")
            add_includedirs(generated_dir)
            add_defines("NIMBLEFIX_BUILTIN_FIX44=1")
        end
        if has_config("xml") then
            add_deps("nimblefix-xml")
            add_defines("NIMBLEFIX_ENABLE_XML=1")
        end
        add_deps("nimblefix-schema-gen")
    target_end()
end

target("hello-fix")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_files("tests/hello_fix.cpp")
    add_deps("nimblefix", "fix44-sample")
    add_includedirs(generated_dir)
    add_defines("NIMBLEFIX_SAMPLE_SCHEMA=1")
target_end()

target("fix44-sample")
    set_kind("phony")
    set_policy("build.fence", true)
    set_default(false)
    add_deps("nimblefix-schema-gen")
    on_build(function ()
        import("core.project.depend")
        import("core.project.project")
        local gen = project.target("nimblefix-schema-gen"):targetfile()
        depend.on_changed(function ()
            os.vrunv(gen, {"--output", sample_header,
                "--namespace", "nimblefix::fix44_sample", "schemas/fix44-sample.xml"})
        end, {files = {"schemas/fix44-sample.xml", gen},
              values = {sample_header}, changed = not os.isfile(sample_header)})
    end)
target_end()

target("direct-initiator-example")
    set_kind("binary")
    add_files("examples/direct_initiator.cpp")
    add_deps("nimblefix-core", "fix44-sample")
    add_includedirs(generated_dir)
target_end()

target("codec-demo")
    set_kind("binary")
    add_files("examples/codec.cpp")
    add_deps("nimblefix")
target_end()

for _, role in ipairs({"initiator", "acceptor"}) do
    target(role .. "-example")
        set_kind("binary")
        set_rundir("$(projectdir)")
        if has_config("standard_schemas") then
            add_files("examples/" .. role .. ".cpp")
        else
            add_files("examples/flat_" .. role .. ".cpp")
        end
        add_deps("nimblefix")
        if has_config("standard_schemas") then
            add_deps("fix44-schema")
            add_includedirs(generated_dir)
        end
    target_end()
end

if has_config("tests") then
    target("initiator-probe")
        set_kind("binary")
        add_files("tests/initiator_probe.cpp")
        add_deps("nimblefix")
    target_end()
    for _, name in ipairs({"parser", "message", "generated", "contract", "journal", "session", "session_boundary", "session_limits", "network", "settings", "journal_reader", "journal_rotation", "protocol_core", "protocol_session", "protocol_security", "queued_runtime", "sender_schema", "schema_codec", "dynamic_message", "engine_features", "producer_lane", "queued_shutdown", "sender_expiry", "io_backend", "backpressure", "p1_features"}) do
        target(name .. "-test")
            set_kind("binary")
            add_files("tests/" .. name .. "_test.cpp")
            add_deps("nimblefix")
            if name == "generated" or name == "contract" or name == "sender_schema" or name == "io_backend" or name == "backpressure" or name == "p1_features" then
                add_deps("fix44-sample")
                add_includedirs(generated_dir)
            end
            add_tests("default", {run_timeout = 30000})
        target_end()
    end
    if has_config("standard_schemas") then
        target("schema_nested_group-test")
            set_kind("binary")
            add_files("tests/schema_nested_group_test.cpp")
            add_deps("nimblefix", "fix44-schema")
            add_includedirs(generated_dir)
            add_tests("default", {run_timeout = 30000})
        target_end()
    end
    if has_config("xml") then
        target("xml-schema-test")
            set_kind("binary")
            add_files("tests/xml_schema_test.cpp")
            add_deps("nimblefix-xml")
            add_tests("default", {runargs = {"schemas"}})
        target_end()
    end
    target("direct_initiator-test")
        set_kind("binary")
        add_files("tests/direct_initiator_test.cpp")
        add_deps("nimblefix-core", "fix44-sample")
        add_includedirs(generated_dir)
        if not has_config("sanitize") and not has_config("thread_sanitize") then
            add_defines("NIMBLEFIX_AUDIT_WRAPS=1")
            for _, symbol in ipairs({"malloc", "pthread_mutex_lock", "fsync", "fdatasync", "msync"}) do
                add_ldflags("-Wl,--wrap=" .. symbol, {force = true})
            end
        end
        add_tests("default", {run_timeout = 30000})
    target_end()

    target("queued-crash-probe")
        set_kind("binary")
        add_files("tests/queued_crash_probe.cpp")
        add_deps("nimblefix")
    target_end()
    target("engine-tls-probe")
        set_kind("binary")
        add_files("tests/engine_tls_probe.cpp")
        add_deps("nimblefix")
    target_end()
    target("tls-transport-test")
        set_kind("binary")
        add_files("tests/tls_transport_test.cpp")
        add_deps("nimblefix")
        if not has_config("tls") then add_tests("unavailable") end
    target_end()
    target("direct-tls-initiator-test")
        set_kind("binary")
        add_files("tests/direct_tls_initiator_test.cpp")
        add_deps("nimblefix-core-tls", "fix44-sample")
        add_includedirs(generated_dir)
        if not has_config("tls") then add_tests("unavailable") end
    target_end()

    for _, harness in ipairs({"codegen", "compile_fail", "initiator_reconnect", "queued_crash", "cli", "protocol_versions", "endpoint", "xmake_schema", "install"}) do
        target(harness .. "-test")
            set_kind("binary")
            add_files("tests/" .. harness .. "_test.cpp")
        target_end()
    end
end

local use_submodule_quickfix = not has_config("quickfix_dir")
    and (has_config("quickfix") or has_config("benchmarks"))
    and os.isfile("third_party/quickfix/src/C++/Session.cpp")
local quickfix_incdir = path.join(get_config("builddir") or get_config("buildir") or "build", "quickfix-include")

if use_submodule_quickfix then
    target("quickfix-headers")
        set_kind("phony")
        set_policy("build.fence", true)
        set_default(false)
        on_build(function ()
            os.mkdir(quickfix_incdir)
            local alloc_hdr = path.join(quickfix_incdir, "Allocator.h")
            if not os.isfile(alloc_hdr) then
                io.writefile(alloc_hdr, "/* Generated out-of-source for bundled QuickFIX */\n")
            end
            local link_path = path.join(quickfix_incdir, "quickfix")
            local target_dir = path.absolute("third_party/quickfix/src/C++")
            if not os.isdir(link_path) then
                os.ln(target_dir, link_path)
            end
        end)
    target_end()

    target("quickfix-bundled")
        set_kind("static")
        set_default(false)
        set_languages("cxx17")
        set_warnings("none")
        add_deps("quickfix-headers")
        add_defines("_FILE_OFFSET_BITS=64", "GETHOSTBYNAME_R_INPUTS_RESULT=1", "HAVE_CXX17")
        add_includedirs(quickfix_incdir, "third_party/quickfix/src/C++")
        add_sysincludedirs(quickfix_incdir, {public = true})
        add_syslinks("pthread", {public = true})
        add_files(
            "third_party/quickfix/src/C++/Acceptor.cpp",
            "third_party/quickfix/src/C++/DataDictionary.cpp",
            "third_party/quickfix/src/C++/DataDictionaryProvider.cpp",
            "third_party/quickfix/src/C++/Dictionary.cpp",
            "third_party/quickfix/src/C++/FieldConvertors.cpp",
            "third_party/quickfix/src/C++/FieldMap.cpp",
            "third_party/quickfix/src/C++/FieldTypes.cpp",
            "third_party/quickfix/src/C++/FileLog.cpp",
            "third_party/quickfix/src/C++/FileStore.cpp",
            "third_party/quickfix/src/C++/Group.cpp",
            "third_party/quickfix/src/C++/HostDetailsProvider.cpp",
            "third_party/quickfix/src/C++/HttpConnection.cpp",
            "third_party/quickfix/src/C++/HttpMessage.cpp",
            "third_party/quickfix/src/C++/HttpParser.cpp",
            "third_party/quickfix/src/C++/HttpServer.cpp",
            "third_party/quickfix/src/C++/Initiator.cpp",
            "third_party/quickfix/src/C++/Log.cpp",
            "third_party/quickfix/src/C++/Message.cpp",
            "third_party/quickfix/src/C++/MessageSorters.cpp",
            "third_party/quickfix/src/C++/MessageStore.cpp",
            "third_party/quickfix/src/C++/NullStore.cpp",
            "third_party/quickfix/src/C++/Parser.cpp",
            "third_party/quickfix/src/C++/pugixml.cpp",
            "third_party/quickfix/src/C++/PUGIXML_DOMDocument.cpp",
            "third_party/quickfix/src/C++/Session.cpp",
            "third_party/quickfix/src/C++/SessionFactory.cpp",
            "third_party/quickfix/src/C++/SessionSettings.cpp",
            "third_party/quickfix/src/C++/Settings.cpp",
            "third_party/quickfix/src/C++/SocketAcceptor.cpp",
            "third_party/quickfix/src/C++/SocketConnection.cpp",
            "third_party/quickfix/src/C++/SocketConnector.cpp",
            "third_party/quickfix/src/C++/SocketInitiator.cpp",
            "third_party/quickfix/src/C++/SocketMonitor_UNIX.cpp",
            "third_party/quickfix/src/C++/SocketServer.cpp",
            "third_party/quickfix/src/C++/stdafx.cpp",
            "third_party/quickfix/src/C++/ThreadedSocketAcceptor.cpp",
            "third_party/quickfix/src/C++/ThreadedSocketConnection.cpp",
            "third_party/quickfix/src/C++/ThreadedSocketInitiator.cpp",
            "third_party/quickfix/src/C++/TimeRange.cpp",
            "third_party/quickfix/src/C++/Utility.cpp"
        )
    target_end()
end

if has_config("quickfix_dir") or (has_config("quickfix") and use_submodule_quickfix) then
    for _, quickfix_name in ipairs({"quickfix", "quickfix_network", "quickfix_dual_role"}) do
    target(quickfix_name .. "-test")
        set_kind("binary")
        set_rundir("$(projectdir)")
        add_files("tests/" .. quickfix_name .. "_test.cpp")
        add_deps("nimblefix")
        if has_config("quickfix_dir") then
            add_sysincludedirs(path.join(get_config("quickfix_dir"), "include"))
            add_linkdirs(path.join(get_config("quickfix_dir"), "lib"))
            add_links("quickfix")
            add_syslinks("pthread", "ssl", "crypto")
        else
            add_deps("quickfix-bundled")
        end
        if quickfix_name == "quickfix_dual_role" then
            add_deps("fix44-schema")
            add_includedirs(generated_dir)
        end
        local dict_path = has_config("quickfix_dir")
            and path.join(get_config("quickfix_dir"), "share", "quickfix", "FIX44.xml")
            or path.join(os.projectdir(), "schemas/standard/FIX44.xml")
        add_tests("oracle", {runargs = {dict_path}, run_timeout = 30000})
    target_end()
    end
end

if has_config("benchmarks") then
    if has_config("quickfix_dir") or use_submodule_quickfix then
        target("quickfix-tcp-server")
            set_kind("binary")
            add_files("bench/quickfix_tcp_server.cpp")
            if has_config("quickfix_dir") then
                add_sysincludedirs(path.join(get_config("quickfix_dir"), "include"))
                add_linkdirs(path.join(get_config("quickfix_dir"), "lib"))
                add_links("quickfix")
                add_syslinks("pthread", "ssl", "crypto", "z")
            else
                add_deps("quickfix-bundled")
            end
        target_end()
        if has_config("standard_schemas") then
            target("quickfix-parity-bench")
                set_kind("binary")
                set_rundir("$(projectdir)")
                add_files("bench/quickfix_parity_bench.cpp")
                add_deps("nimblefix", "fix44-schema")
                add_includedirs(generated_dir)
                if has_config("quickfix_dir") then
                    add_sysincludedirs(path.join(get_config("quickfix_dir"), "include"))
                    add_linkdirs(path.join(get_config("quickfix_dir"), "lib"))
                    add_links("quickfix")
                    add_syslinks("pthread", "ssl", "crypto", "z")
                else
                    add_deps("quickfix-bundled")
                end
                for _, symbol in ipairs({"malloc", "calloc", "realloc", "aligned_alloc", "posix_memalign"}) do
                    add_ldflags("-Wl,--wrap=" .. symbol, {force = true})
                end
            target_end()
        end
    end
    target("runtime-bench")
        set_kind("binary")
        add_files("bench/runtime_bench.cpp")
        add_deps("nimblefix")
        for _, symbol in ipairs({"malloc", "calloc", "realloc", "pthread_mutex_lock", "fsync", "fdatasync", "msync", "send"}) do
            add_ldflags("-Wl,--wrap=" .. symbol, {force = true})
        end
    target_end()
    target("journal-bench")
        set_kind("binary")
        add_files("bench/journal_bench.cpp")
        add_deps("nimblefix")
    target_end()
    if has_config("standard_schemas") then
        target("schema-bench")
            set_kind("binary")
            add_files("bench/schema_bench.cpp")
            add_deps("nimblefix", "fix44-schema")
            add_includedirs(generated_dir)
            add_ldflags("-Wl,--wrap=malloc", {force=true})
        target_end()
    end
    target("isolation-server")
        set_kind("binary")
        add_files("bench/isolation_server.cpp")
        add_deps("nimblefix")
    target_end()
    target("session-bench")
        set_kind("binary")
        add_files("bench/session_bench.cpp")
        add_deps("nimblefix")
        add_ldflags("-Wl,--wrap=malloc", "-Wl,--wrap=fsync", "-Wl,--wrap=fdatasync", "-Wl,--wrap=msync", {force=true})
    target_end()
    target("null-tcp-server")
        set_kind("binary")
        add_files("bench/null_tcp_server.cpp")
    target_end()
    target("tcp-bench-server")
        set_kind("binary")
        add_files("bench/tcp_server.cpp")
        add_deps("nimblefix")
    target_end()
    target("codec-bench")
        set_kind("binary")
        add_files("bench/codec_bench.cpp")
        add_deps("nimblefix", "fix44-sample")
        add_includedirs(generated_dir)
        add_ldflags("-Wl,--wrap=malloc", {force = true})
    target_end()
    if has_config("standard_schemas") then
        target("direct-initiator-bench")
            set_kind("binary")
            add_files("bench/direct_initiator_bench.cpp")
            add_deps("nimblefix-core", "fix44-schema")
            add_includedirs(generated_dir)
            for _, symbol in ipairs({"malloc", "calloc", "realloc", "aligned_alloc", "posix_memalign"}) do
                add_ldflags("-Wl,--wrap=" .. symbol, {force = true})
            end
        target_end()
        target("acceptor-bench")
            set_kind("binary")
            add_files("bench/acceptor_bench.cpp")
            add_deps("nimblefix", "fix44-schema")
            add_includedirs(generated_dir)
            for _, symbol in ipairs({"malloc", "calloc", "realloc", "aligned_alloc", "posix_memalign"}) do
                add_ldflags("-Wl,--wrap=" .. symbol, {force = true})
            end
        target_end()
        target("initiator-burst-bench")
            set_kind("binary")
            add_files("bench/initiator_burst_bench.cpp")
            add_deps("nimblefix", "fix44-schema")
            add_includedirs(generated_dir)
            for _, symbol in ipairs({"malloc", "calloc", "realloc", "aligned_alloc", "posix_memalign"}) do
                add_ldflags("-Wl,--wrap=" .. symbol, {force = true})
            end
        target_end()
    end
    if has_config("tests") then
        target("bench-smoke-test")
            set_kind("binary")
            add_files("bench/bench_smoke_test.cpp")
        target_end()
    end
end

if has_config("fuzzers") then
    if has_config("standard_schemas") then
        target("schema-fuzz")
            set_kind("binary")
            add_files("tests/fuzz_schema.cpp", "src/codec/parser.cpp", "src/codec/encoder.cpp", "src/message/decimal.cpp")
            add_includedirs("include", generated_dir)
            add_deps("fix44-schema")
            add_cxxflags("-fsanitize=fuzzer,address,undefined", "-fno-omit-frame-pointer", {force = true})
            add_ldflags("-fsanitize=fuzzer,address,undefined", {force = true})
        target_end()
    end
    target("parser-fuzz")
        set_kind("binary")
        set_rundir("$(projectdir)")
        add_files("tests/fuzz_parser.cpp", "src/codec/parser.cpp", "src/message/decimal.cpp")
        add_includedirs("include")
        add_cxxflags("-fsanitize=fuzzer,address,undefined", "-fno-omit-frame-pointer", {force = true})
        add_ldflags("-fsanitize=fuzzer,address,undefined", {force = true})
    target_end()
end

task("check")
    set_menu {usage = "xmake check", description = "Build and run codec, session, TCP, XML and compile-fail contracts (zero Python)"}
    on_run(function ()
        import("core.project.config")
        import("core.project.project")
        config.load()
        os.vrunv(os.programfile(), {"build"})
        os.vrunv(os.programfile(), {"test"})
        local gen = project.target("nimblefix-schema-gen"):targetfile()
        local root = os.projectdir()
        local cxx = config.get("cxx") or os.getenv("CXX") or "g++"
        os.vrunv(project.target("codegen-test"):targetfile(), {"--generator", gen, "--root", root})
        os.vrunv(project.target("compile_fail-test"):targetfile(), {"--generator", gen, "--cxx", cxx, "--root", root})
        os.vrunv(os.programfile(), {"run", "codec-demo", "--self-test"})
        os.vrunv(os.programfile(), {"run", "hello-fix", "--self-test", "--restart"})
        os.vrunv(os.programfile(), {"run", "hello-fix", "--self-test", "--typed", "--restart"})
        local probe = project.target("initiator-probe"):targetfile()
        local reconnect = project.target("initiator_reconnect-test"):targetfile()
        os.vrunv(reconnect, {"--binary", probe, "--mode", "inbound"})
        os.vrunv(reconnect, {"--binary", probe, "--mode", "outbound"})
        os.vrunv(project.target("endpoint-test"):targetfile(), {
            "--acceptor", project.target("acceptor-example"):targetfile(),
            "--initiator", project.target("initiator-example"):targetfile()})
        os.vrunv(project.target("queued_crash-test"):targetfile(), {
            "--binary", project.target("queued-crash-probe"):targetfile()})
        os.vrunv(project.target("xmake_schema-test"):targetfile(), {
            "--rule", path.join(root, "xmake", "nimblefix-schema.lua"),
            "--generator", gen,
            "--include-dir", path.join(root, "include")})
        if config.get("cli") then
            local cli = project.target("nimblefix-cli"):targetfile()
            os.vrunv(project.target("cli-test"):targetfile(), {"--binary", cli, "--root", root})
            os.vrunv(project.target("protocol_versions-test"):targetfile(), {"--binary", cli})
        end
        if config.get("tls") then
            os.vrunv(project.target("engine-tls-probe"):targetfile(), {})
            os.vrunv(project.target("tls-transport-test"):targetfile(), {})
            os.vrunv(project.target("direct-tls-initiator-test"):targetfile(), {})
        end
        if config.get("benchmarks") then
            local smoke = project.target("bench-smoke-test"):targetfile()
            local smoke_args = {
                "--journal-bench", project.target("journal-bench"):targetfile(),
                "--runtime-bench", project.target("runtime-bench"):targetfile()}
            if config.get("standard_schemas") then
                table.insert(smoke_args, "--schema-bench")
                table.insert(smoke_args, project.target("schema-bench"):targetfile())
            end
            os.vrunv(smoke, smoke_args)
        end
        print("All codec, journal, session, TCP, schema, and compile-fail contracts passed (zero Python).")
    end)
task_end()

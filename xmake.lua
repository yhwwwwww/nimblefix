set_project("nimblefix")
set_version("0.2.0")
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

option("xml")
    set_default(false)
    set_showmenu(true)
    set_description("Build the optional runtime XML component (system libxml2)")
option_end()

option("cli")
    set_default(true)
    set_showmenu(true)
    set_description("Build the TOML configuration library and nimblefix CLI")
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

target("nimblefix")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_headerfiles("include/(nimblefix/**.hpp)")
    add_files("src/codec/parser.cpp", "src/codec/encoder.cpp", "src/message/decimal.cpp")
    add_files("src/store/*.cpp", "src/session/*.cpp", "src/runtime/*.cpp", "src/transport/*.cpp")
    if has_config("tls") then
        add_defines("NIMBLEFIX_ENABLE_TLS=1")
        add_syslinks("ssl", "crypto", {public = true})
    end
    add_syslinks("pthread", {public = true})
target_end()

if has_config("standard_schemas") then
    for _, schema in ipairs({"fix42", "fix43", "fix44", "fixt11", "fix50sp2"}) do
        target(schema .. "-schema")
            set_kind("phony")
            set_policy("build.fence", true)
            set_default(false)
            on_build(function ()
                os.vrunv("python3", {"tools/schema/generate.py", "--output",
                    path.join(generated_dir, "nimblefix", schema .. ".hpp"), "--namespace", "nimblefix::" .. schema,
                    path.join("schemas", "standard", schema:upper() .. ".xml")})
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
    target("nimblefix-config")
        set_kind("static")
        add_files("src/config/config.cpp")
        add_includedirs("third_party/tomlplusplus")
        add_deps("nimblefix")
    target_end()
    target("nimblefix-cli")
        set_kind("binary")
        set_basename("nimblefix")
        set_rundir("$(projectdir)")
        add_files("tools/nimblefix/main.cpp")
        if has_config("benchmarks") then add_defines("NIMBLEFIX_ENABLE_BENCH=1") end
        add_deps("nimblefix-config")
        if has_config("standard_schemas") then
            add_deps("fix44-schema")
            add_includedirs(generated_dir)
            add_defines("NIMBLEFIX_BUILTIN_FIX44=1")
        end
        if has_config("xml") then
            add_deps("nimblefix-xml")
            add_defines("NIMBLEFIX_ENABLE_XML=1")
        end
        after_build(function (target)
            os.cp("tools/schema/generate.py", path.join(target:targetdir(), "share", "nimblefix", "schema", "generate.py"))
        end)
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
    on_build(function ()
        os.vrunv("python3", {"tools/schema/generate.py", "--output", sample_header,
            "--namespace", "nimblefix::fix44_sample", "schemas/fix44-sample.xml"})
    end)
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
    for _, name in ipairs({"parser", "message", "generated", "contract", "journal", "session", "session_boundary", "session_limits", "network", "settings", "journal_reader", "journal_rotation", "protocol_core", "protocol_session", "queued_runtime", "sender_schema", "schema_codec", "dynamic_message", "engine_features", "producer_lane", "queued_shutdown", "sender_expiry"}) do
        target(name .. "-test")
            set_kind("binary")
            add_files("tests/" .. name .. "_test.cpp")
            add_deps("nimblefix")
            if name == "generated" or name == "contract" or name == "sender_schema" then
                add_deps("fix44-sample")
                add_includedirs(generated_dir)
            end
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
end

if has_config("quickfix_dir") then
    for _, quickfix_name in ipairs({"quickfix", "quickfix_network", "quickfix_dual_role"}) do
    target(quickfix_name .. "-test")
        set_kind("binary")
        add_files("tests/" .. quickfix_name .. "_test.cpp")
        add_deps("nimblefix")
        add_sysincludedirs(path.join(get_config("quickfix_dir"), "include"))
        add_linkdirs(path.join(get_config("quickfix_dir"), "lib"))
        add_links("quickfix")
        add_syslinks("pthread", "ssl", "crypto")
        if quickfix_name == "quickfix_dual_role" then
            add_deps("fix44-schema")
            add_includedirs(generated_dir)
        end
        add_tests("oracle", {runargs = {path.join(get_config("quickfix_dir"), "share", "quickfix", "FIX44.xml")}})
    target_end()
    end
end

if has_config("benchmarks") then
    if has_config("quickfix_dir") then
        target("quickfix-tcp-server")
            set_kind("binary")
            add_files("bench/quickfix_tcp_server.cpp")
            add_sysincludedirs(path.join(get_config("quickfix_dir"), "include"))
            add_linkdirs(path.join(get_config("quickfix_dir"), "lib"))
            add_links("quickfix")
            add_syslinks("pthread", "ssl", "crypto", "z")
        target_end()
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
    set_menu {usage = "xmake check", description = "Build and run codec, session, TCP, XML and compile-fail contracts"}
    on_run(function ()
        import("core.project.config")
        config.load()
        os.vrunv(os.programfile(), {"build"})
        os.vrunv(os.programfile(), {"test"})
        os.vrunv("python3", {"tests/codegen_test.py"})
        os.vrunv("python3", {"tests/compile_fail_test.py"})
        os.vrunv(os.programfile(), {"run", "codec-demo", "--self-test"})
        os.vrunv(os.programfile(), {"run", "hello-fix", "--self-test", "--restart"})
        os.vrunv(os.programfile(), {"run", "hello-fix", "--self-test", "--typed", "--restart"})
        import("core.project.project")
        local probe = project.target("initiator-probe")
        os.vrunv("python3", {"tests/initiator_reconnect_test.py", "--binary", probe:targetfile()})
        os.vrunv("python3", {"tests/outbound_reconnect_test.py", "--binary", probe:targetfile()})
        os.vrunv("python3", {"tests/endpoint_test.py", "--acceptor", project.target("acceptor-example"):targetfile(),
            "--initiator", project.target("initiator-example"):targetfile()})
        os.vrunv("python3", {"tests/queued_crash_test.py", "--binary", project.target("queued-crash-probe"):targetfile()})
        os.vrunv("python3", {"tests/xmake_schema_test.py"})
        if config.get("cli") then
            os.vrunv("python3", {"tests/cli_test.py", "--binary", project.target("nimblefix-cli"):targetfile()})
            os.vrunv("python3", {"tests/protocol_versions_test.py", "--binary", project.target("nimblefix-cli"):targetfile()})
        end
        if config.get("tls") then
            os.vrunv("python3", {"tests/engine_tls_test.py", "--binary", project.target("engine-tls-probe"):targetfile()})
            os.vrunv("python3", {"tests/tls_transport_test.py", "--binary", project.target("tls-transport-test"):targetfile()})
        end
        if config.get("benchmarks") then
            os.vrunv("python3", {"bench/journal_run.py", "--binary", project.target("journal-bench"):targetfile(),
                "--output", "build/bench-smoke/journal", "--samples", "100", "--runs", "1",
                "--reopens", "2", "--segment-bytes", "8192", "--max-segments", "32"})
            os.vrunv("python3", {"bench/runtime_run.py", "--binary", project.target("runtime-bench"):targetfile(),
                "--output", "build/bench-smoke/runtime", "--samples", "100", "--warmup", "20"})
            if config.get("standard_schemas") then
                os.vrunv("python3", {"bench/schema_run.py", "--binary", project.target("schema-bench"):targetfile(),
                    "--generated-header", path.join(config.get("builddir") or config.get("buildir") or "build", "generated", "nimblefix", "fix44.hpp"),
                    "--output", "build/bench-smoke/schema", "--samples", "100", "--runs", "1",
                    "--workset", "16", "--skip-size-probe", "--skip-perf"})
            end
        end
        print("All codec, journal, session, TCP, schema, and compile-fail contracts passed.")
    end)
task_end()

set_project("nimblefix")
set_version("0.1.0")
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

if has_config("sanitize") then
    add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", {force = true})
    add_ldflags("-fsanitize=address,undefined", {force = true})
end

local generated_dir = path.join(get_config("builddir") or get_config("buildir") or "build", "generated")
local sample_header = path.join(generated_dir, "nimblefix", "fix44_sample.hpp")

target("nimblefix")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_headerfiles("include/(nimblefix/**.hpp)")
    add_files("src/codec/parser.cpp", "src/codec/encoder.cpp", "src/message/decimal.cpp")
    add_files("src/store/*.cpp", "src/session/*.cpp", "src/runtime/*.cpp", "src/transport/*.cpp")
    add_syslinks("pthread", {public = true})
target_end()

target("hello-fix")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_files("examples/hello_fix.cpp")
    add_deps("nimblefix", "fix44-sample")
    add_includedirs(generated_dir)
    add_defines("NIMBLEFIX_SAMPLE_SCHEMA=1")
target_end()

target("fix44-sample")
    set_kind("phony")
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
        add_files("examples/" .. role .. ".cpp")
        add_deps("nimblefix")
    target_end()
end

if has_config("tests") then
    target("initiator-probe")
        set_kind("binary")
        add_files("tests/initiator_probe.cpp")
        add_deps("nimblefix")
    target_end()
    for _, name in ipairs({"parser", "message", "generated", "contract", "journal", "session", "session_boundary", "session_limits", "network"}) do
        target(name .. "-test")
            set_kind("binary")
            add_files("tests/" .. name .. "_test.cpp")
            add_deps("nimblefix")
            if name == "generated" or name == "contract" then
                add_deps("fix44-sample")
                add_includedirs(generated_dir)
            end
            add_tests("default", {run_timeout = 30000})
        target_end()
    end
end

if has_config("quickfix_dir") then
    for _, quickfix_name in ipairs({"quickfix", "quickfix_network"}) do
    target(quickfix_name .. "-test")
        set_kind("binary")
        add_files("tests/" .. quickfix_name .. "_test.cpp")
        add_deps("nimblefix")
        add_includedirs(path.join(get_config("quickfix_dir"), "include"))
        add_linkdirs(path.join(get_config("quickfix_dir"), "lib"))
        add_links("quickfix")
        add_syslinks("pthread")
        add_tests("oracle", {runargs = {path.join(get_config("quickfix_dir"), "share", "quickfix", "FIX44.xml")}})
    target_end()
    end
end

if has_config("benchmarks") then
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
        print("All codec, journal, session, TCP, schema, and compile-fail contracts passed.")
    end)
task_end()

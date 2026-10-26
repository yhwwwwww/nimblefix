set_project("nimblefix")
set_version("0.1.0")
set_xmakever("2.9.8")
set_languages("cxx20")
add_rules("mode.debug", "mode.release")
set_warnings("allextra")

option("tests")
    set_default(true)
    set_showmenu(true)
    set_description("Build codec contract tests")
option_end()

option("fuzzers")
    set_default(false)
    set_showmenu(true)
    set_description("Build the Clang libFuzzer parser target")
option_end()

option("benchmarks")
    set_default(false)
    set_showmenu(true)
    set_description("Build the codec microbenchmark")
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

if has_config("tests") then
    for _, name in ipairs({"parser", "message", "generated", "contract"}) do
        target(name .. "-test")
            set_kind("binary")
            add_files("tests/" .. name .. "_test.cpp")
            add_deps("nimblefix")
            if name == "generated" or name == "contract" then
                add_deps("fix44-sample")
                add_includedirs(generated_dir)
            end
            add_tests("default")
        target_end()
    end
end

if has_config("benchmarks") then
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
        add_files("tests/fuzz_parser.cpp", "src/codec/parser.cpp", "src/message/decimal.cpp")
        add_includedirs("include")
        add_cxxflags("-fsanitize=fuzzer,address,undefined", "-fno-omit-frame-pointer", {force = true})
        add_ldflags("-fsanitize=fuzzer,address,undefined", {force = true})
    target_end()
end

task("check")
    set_menu {usage = "xmake check", description = "Build and run codec, XML, and compile-fail contracts"}
    on_run(function ()
        os.vrunv(os.programfile(), {"build"})
        os.vrunv(os.programfile(), {"test"})
        os.vrunv("python3", {"tests/codegen_test.py"})
        os.vrunv("python3", {"tests/compile_fail_test.py"})
        os.vrunv(os.programfile(), {"run", "codec-demo", "--self-test"})
        print("All codec, schema, and compile-fail contracts passed.")
    end)
task_end()

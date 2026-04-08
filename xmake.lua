set_project("fastfix")
set_version("0.1.0")
set_languages("cxx20")
set_warnings("all")
add_rules("mode.debug", "mode.release")
add_requires("catch2 ^3.4", {configs = {header_only = false}})
add_requires("pugixml")

includes("config/deps.lua")

local local_deps = FASTFIX_LOCAL_DEPS or {}

local function apply_local_dep(target_name, dep_name)
    local dep = local_deps[dep_name]
    if dep == nil then
        return
    end

    if dep.include_dirs ~= nil and #dep.include_dirs > 0 then
        target(target_name)
            add_includedirs(table.unpack(dep.include_dirs), {public = true})
    end

    if dep.defines ~= nil and #dep.defines > 0 then
        target(target_name)
            add_defines(table.unpack(dep.defines), {public = true})
    end

    if dep.link_dirs ~= nil and #dep.link_dirs > 0 then
        target(target_name)
            add_linkdirs(table.unpack(dep.link_dirs))
    end

    if dep.links ~= nil and #dep.links > 0 then
        target(target_name)
            add_links(table.unpack(dep.links))
    end
end

target("fastfix")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_defines("FASTFIX_PROJECT_DIR=\"$(projectdir)\"", {public = true})
    add_files("src/profile/*.cpp", "src/runtime/*.cpp", "src/session/*.cpp", "src/store/*.cpp", "src/message/*.cpp", "src/codec/*.cpp", "src/transport/*.cpp")
    add_syslinks("uring")

target("fastfix-dictgen")
    set_kind("binary")
    add_deps("fastfix")
    add_files("tools/dictgen/*.cpp")

target("fastfix-interop-runner")
    set_kind("binary")
    add_deps("fastfix")
    add_files("tools/interop-runner/*.cpp")

target("fastfix-soak")
    set_kind("binary")
    add_deps("fastfix")
    add_files("tools/soak/*.cpp")

target("fastfix-fuzz-config")
    set_kind("binary")
    add_deps("fastfix")
    add_files("tools/fuzz-config/*.cpp")

target("fastfix-fuzz-dictgen")
    set_kind("binary")
    add_deps("fastfix")
    add_files("tools/fuzz-dictgen/*.cpp")

target("fastfix-fuzz-codec")
    set_kind("binary")
    add_deps("fastfix")
    add_files("tools/fuzz-codec/main.cpp")

target("fastfix-fuzz-codec-libfuzzer")
    set_kind("binary")
    add_deps("fastfix")
    add_files("tools/fuzz-codec/fuzz_entry.cpp")
    add_cxflags("-fsanitize=fuzzer", {force = true})
    add_ldflags("-fsanitize=fuzzer", {force = true})

target("fastfix-initiator")
    set_kind("binary")
    add_deps("fastfix")
    add_files("tools/initiator/*.cpp")

target("fastfix-acceptor")
    set_kind("binary")
    add_deps("fastfix")
    add_files("tools/acceptor/*.cpp")

target("fastfix-xml2ffd")
    set_kind("binary")
    add_deps("fastfix")
    add_packages("pugixml")
    add_files("tools/xml2ffd/*.cpp")

target("fastfix-bench")
    set_kind("binary")
    add_deps("fastfix")
    add_includedirs("build/generated")
    add_files("bench/main.cpp")

target("fastfix-tests")
    set_kind("binary")
    add_deps("fastfix")
    add_packages("catch2", "pugixml")
    add_includedirs("build/generated")
    add_files("tests/*.cpp", "tools/xml2ffd/xml2ffd.cpp")
    remove_files("tests/test_main.cpp")

apply_local_dep("fastfix", "fastfix")
apply_local_dep("fastfix-dictgen", "fastfix-dictgen")
apply_local_dep("fastfix-interop-runner", "fastfix-interop-runner")
apply_local_dep("fastfix-soak", "fastfix-soak")
apply_local_dep("fastfix-fuzz-config", "fastfix-fuzz-config")
apply_local_dep("fastfix-fuzz-dictgen", "fastfix-fuzz-dictgen")
apply_local_dep("fastfix-fuzz-codec", "fastfix-fuzz-codec")
apply_local_dep("fastfix-fuzz-codec-libfuzzer", "fastfix-fuzz-codec-libfuzzer")
apply_local_dep("fastfix-initiator", "fastfix-initiator")
apply_local_dep("fastfix-acceptor", "fastfix-acceptor")
apply_local_dep("fastfix-bench", "fastfix-bench")
apply_local_dep("fastfix-tests", "fastfix-tests")

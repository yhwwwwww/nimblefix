set_project("fastfix")
set_version("0.1.0")
set_languages("cxx20")
set_warnings("all")
add_rules("mode.debug", "mode.release")

includes("config/deps.lua")

local local_deps = FASTFIX_LOCAL_DEPS or {}
local vendor_hint = "run `git submodule update --init --recursive`"
local catch2_override_root = path.join("deps", "include")
local quickfix_root = path.join("bench", "vendor", "quickfix")

local function ensure_vendor_file(required_path, label)
    if not os.isfile(required_path) then
        os.raise("missing %s at %s; %s", label, required_path, vendor_hint)
    end
end

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

local function generated_tool_path(name)
    local buildir = get_config("buildir") or "build"
    local plat = get_config("plat") or os.host()
    local arch = get_config("arch") or os.arch()
    local mode = get_config("mode") or "release"
    local suffix = is_plat("windows") and ".exe" or ""
    return path.join(buildir, plat, arch, mode, name .. suffix)
end

local function output_is_stale(output_path, input_paths)
    if not os.isfile(output_path) then
        return true
    end

    local output_mtime = os.mtime(output_path)
    if output_mtime == nil then
        return true
    end

    for _, input_path in ipairs(input_paths) do
        local input_mtime = os.mtime(input_path)
        if input_mtime ~= nil and input_mtime > output_mtime then
            return true
        end
    end
    return false
end

local function any_output_is_stale(output_paths, input_paths)
    for _, output_path in ipairs(output_paths) do
        if output_is_stale(output_path, input_paths) then
            return true
        end
    end
    return false
end

local function enqueue_fix44_assets(batchcmds)
    local xml2ffd_bin = generated_tool_path("fastfix-xml2ffd")
    local dictgen_bin = generated_tool_path("fastfix-dictgen")
    local fix44_xml = path.join(quickfix_root, "spec", "FIX44.xml")
    local fix44_ffd = path.join("build", "bench", "quickfix_FIX44.ffd")
    local fix44_art = path.join("build", "bench", "quickfix_FIX44.art")
    local fix44_builders = path.join("build", "generated", "fix44_builders.h")

    if not os.isfile(xml2ffd_bin) then
        os.raise("missing build tool: %s", xml2ffd_bin)
    end
    if not os.isfile(dictgen_bin) then
        os.raise("missing build tool: %s", dictgen_bin)
    end
    if not os.isfile(fix44_xml) then
        os.raise("missing QuickFIX FIX44 dictionary at %s; %s", fix44_xml, vendor_hint)
    end

    batchcmds:mkdir(path.join("build", "bench"))
    batchcmds:mkdir(path.join("build", "generated"))

    local needs_fix44_ffd = output_is_stale(fix44_ffd, {fix44_xml, xml2ffd_bin})
    if needs_fix44_ffd then
        batchcmds:show("[fastfix] regenerating %s", fix44_ffd)
        batchcmds:vrunv(xml2ffd_bin, {
            "--xml", fix44_xml,
            "--output", fix44_ffd,
            "--profile-id", "4400"
        })
    end

    if needs_fix44_ffd or any_output_is_stale({fix44_art, fix44_builders}, {fix44_ffd, dictgen_bin}) then
        batchcmds:show("[fastfix] regenerating %s and %s", fix44_art, fix44_builders)
        batchcmds:vrunv(dictgen_bin, {
            "--input", fix44_ffd,
            "--output", fix44_art,
            "--cpp-builders", fix44_builders
        })
    end
end

ensure_vendor_file(path.join("deps", "src", "pugixml", "src", "pugixml.cpp"), "pugixml submodule checkout")
ensure_vendor_file(path.join("deps", "src", "Catch2", "extras", "catch_amalgamated.cpp"), "Catch2 submodule checkout")
ensure_vendor_file(path.join(catch2_override_root, "catch2", "catch_user_config.hpp"), "Catch2 override header")
ensure_vendor_file(path.join("deps", "include", "quickfix", "Allocator.h"), "QuickFIX override header")
ensure_vendor_file(path.join(quickfix_root, "spec", "FIX44.xml"), "QuickFIX submodule checkout")

local quickfix_vendor_sources = {
    path.join(quickfix_root, "src", "C++", "Acceptor.cpp"),
    path.join(quickfix_root, "src", "C++", "DataDictionary.cpp"),
    path.join(quickfix_root, "src", "C++", "DataDictionaryProvider.cpp"),
    path.join(quickfix_root, "src", "C++", "Dictionary.cpp"),
    path.join(quickfix_root, "src", "C++", "FieldConvertors.cpp"),
    path.join(quickfix_root, "src", "C++", "FieldMap.cpp"),
    path.join(quickfix_root, "src", "C++", "FieldTypes.cpp"),
    path.join(quickfix_root, "src", "C++", "FileLog.cpp"),
    path.join(quickfix_root, "src", "C++", "FileStore.cpp"),
    path.join(quickfix_root, "src", "C++", "Group.cpp"),
    path.join(quickfix_root, "src", "C++", "HostDetailsProvider.cpp"),
    path.join(quickfix_root, "src", "C++", "HttpConnection.cpp"),
    path.join(quickfix_root, "src", "C++", "HttpMessage.cpp"),
    path.join(quickfix_root, "src", "C++", "HttpParser.cpp"),
    path.join(quickfix_root, "src", "C++", "HttpServer.cpp"),
    path.join(quickfix_root, "src", "C++", "Initiator.cpp"),
    path.join(quickfix_root, "src", "C++", "Log.cpp"),
    path.join(quickfix_root, "src", "C++", "Message.cpp"),
    path.join(quickfix_root, "src", "C++", "MessageSorters.cpp"),
    path.join(quickfix_root, "src", "C++", "MessageStore.cpp"),
    path.join(quickfix_root, "src", "C++", "NullStore.cpp"),
    path.join(quickfix_root, "src", "C++", "Parser.cpp"),
    path.join(quickfix_root, "src", "C++", "PUGIXML_DOMDocument.cpp"),
    path.join(quickfix_root, "src", "C++", "Session.cpp"),
    path.join(quickfix_root, "src", "C++", "SessionFactory.cpp"),
    path.join(quickfix_root, "src", "C++", "SessionSettings.cpp"),
    path.join(quickfix_root, "src", "C++", "Settings.cpp"),
    path.join(quickfix_root, "src", "C++", "SocketAcceptor.cpp"),
    path.join(quickfix_root, "src", "C++", "SocketConnection.cpp"),
    path.join(quickfix_root, "src", "C++", "SocketConnector.cpp"),
    path.join(quickfix_root, "src", "C++", "SocketInitiator.cpp"),
    path.join(quickfix_root, "src", "C++", "SocketMonitor_UNIX.cpp"),
    path.join(quickfix_root, "src", "C++", "SocketServer.cpp"),
    path.join(quickfix_root, "src", "C++", "stdafx.cpp"),
    path.join(quickfix_root, "src", "C++", "ThreadedSocketAcceptor.cpp"),
    path.join(quickfix_root, "src", "C++", "ThreadedSocketConnection.cpp"),
    path.join(quickfix_root, "src", "C++", "ThreadedSocketInitiator.cpp"),
    path.join(quickfix_root, "src", "C++", "TimeRange.cpp"),
    path.join(quickfix_root, "src", "C++", "Utility.cpp"),
    path.join(quickfix_root, "src", "C++", "pugixml.cpp")
}

local function enqueue_sample_assets(batchcmds)
    local dictgen_bin = generated_tool_path("fastfix-dictgen")
    local sample_profile = path.join("samples", "basic_profile.ffd")
    local sample_overlay = path.join("samples", "basic_overlay.ffd")
    local sample_art = path.join("build", "sample-basic.art")
    local sample_builders = path.join("build", "generated", "sample_basic_builders.h")

    if not os.isfile(dictgen_bin) then
        os.raise("missing build tool: %s", dictgen_bin)
    end
    if not os.isfile(sample_profile) then
        os.raise("missing sample profile: %s", sample_profile)
    end
    if not os.isfile(sample_overlay) then
        os.raise("missing sample overlay: %s", sample_overlay)
    end

    batchcmds:mkdir("build")
    batchcmds:mkdir(path.join("build", "generated"))

    if any_output_is_stale({sample_art, sample_builders}, {sample_profile, sample_overlay, dictgen_bin}) then
        batchcmds:show("[fastfix] regenerating %s and %s", sample_art, sample_builders)
        batchcmds:vrunv(dictgen_bin, {
            "--input", sample_profile,
            "--merge", sample_overlay,
            "--output", sample_art,
            "--cpp-builders", sample_builders
        })
    end
end

rule("fastfix.fix44_assets")
    before_buildcmd(function (target, batchcmds, opt)
        enqueue_fix44_assets(batchcmds)
    end)

rule("fastfix.sample_assets")
    before_buildcmd(function (target, batchcmds, opt)
        enqueue_sample_assets(batchcmds)
    end)

target("fastfix-thirdparty-pugixml")
    set_kind("static")
    set_default(false)
    set_warnings("none")
    add_includedirs("deps/src/pugixml/src", {public = true})
    add_files("deps/src/pugixml/src/pugixml.cpp")

target("fastfix-thirdparty-catch2-main")
    set_kind("static")
    set_default(false)
    set_warnings("none")
    add_includedirs(catch2_override_root, {public = true})
    add_includedirs("deps/src/Catch2/src", {public = true})
    add_includedirs("deps/src/Catch2/extras")
    add_files("deps/src/Catch2/extras/catch_amalgamated.cpp")

target("fastfix")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_defines("FASTFIX_PROJECT_DIR=\"$(projectdir)\"", {public = true})
    add_files("src/profile/*.cpp", "src/runtime/*.cpp", "src/session/*.cpp", "src/store/*.cpp", "src/message/*.cpp", "src/codec/*.cpp", "src/transport/*.cpp")
    if os.isfile("/usr/include/liburing.h") or os.isfile("/usr/local/include/liburing.h") then
        add_syslinks("uring")
    else
        add_defines("FASTFIX_DISABLE_LIBURING")
    end

target("fastfix-dictgen")
    set_kind("binary")
    set_policy("build.fence", true)
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
    set_default(false)
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
    set_policy("build.fence", true)
    add_deps("fastfix", "fastfix-thirdparty-pugixml")
    add_files("tools/xml2ffd/*.cpp")

target("fastfix-fix44-assets")
    set_kind("phony")
    set_default(false)
    set_policy("build.fence", true)
    add_deps("fastfix-xml2ffd", "fastfix-dictgen")
    add_rules("fastfix.fix44_assets")

target("fastfix-sample-assets")
    set_kind("phony")
    set_default(false)
    set_policy("build.fence", true)
    add_deps("fastfix-dictgen")
    add_rules("fastfix.sample_assets")

target("fastfix-generated-assets")
    set_kind("phony")
    set_default(false)
    set_policy("build.fence", true)
    add_deps("fastfix-fix44-assets", "fastfix-sample-assets")

target("fastfix-bench")
    set_kind("binary")
    add_deps("fastfix", "fastfix-fix44-assets")
    add_includedirs("build/generated")
    add_files("bench/main.cpp")

target("fastfix-vendor-quickfix")
    set_kind("static")
    set_default(false)
    set_warnings("none")
    set_languages("cxx17")
    add_defines("NOMINMAX", "_FILE_OFFSET_BITS=64")
    add_includedirs(path.join("deps", "include", "quickfix"), path.join(quickfix_root, "src", "C++"), {public = true})
    add_includedirs(quickfix_root, path.join(quickfix_root, "src"))
    add_files(table.unpack(quickfix_vendor_sources))
    if is_plat("linux") then
        add_syslinks("pthread")
    end

target("fastfix-quickfix-cpp-bench")
    set_kind("binary")
    set_default(false)
    set_basename("quickfix-cpp-bench")
    set_languages("cxx17")
    add_deps("fastfix-vendor-quickfix")
    add_includedirs("bench")
    add_files("bench/quickfix_main.cpp")
    if is_plat("linux") then
        add_syslinks("pthread")
    end

target("fastfix-tests")
    set_kind("binary")
    add_deps("fastfix", "fastfix-thirdparty-catch2-main", "fastfix-thirdparty-pugixml", "fastfix-generated-assets")
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
apply_local_dep("fastfix-vendor-quickfix", "fastfix-vendor-quickfix")
apply_local_dep("fastfix-quickfix-cpp-bench", "fastfix-quickfix-cpp-bench")
apply_local_dep("fastfix-tests", "fastfix-tests")

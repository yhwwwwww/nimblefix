set_project("nimblefix")
set_version("0.1.0")
set_languages("cxx20")
set_warnings("all")
add_rules("mode.debug", "mode.release")

option("nimblefix_enable_tls")
    set_default(false)
    set_showmenu(true)
    set_description("Enable optional TLS support via OpenSSL")
option_end()

if has_config("nimblefix_enable_tls") then
    add_requires("openssl")
end

includes("config/deps.lua")

local local_deps = NIMBLEFIX_LOCAL_DEPS or {}
local vendor_hint = "run `git submodule update --init --recursive`"
local catch2_override_root = path.join("deps", "include")
local quickfix_root = path.join("bench", "vendor", "quickfix")
local public_include_root = path.join("include", "public")
local internal_include_root = path.join("include", "internal")
local table_unpack = table.unpack or unpack
local xmake_raise = raise or os.raise or function (format, ...)
    error(string.format(format, ...), 0)
end

local function batchcmds_show(batchcmds, format, ...)
    if batchcmds.show ~= nil then
        batchcmds:show(format, ...)
    elseif batchcmds.show_progress ~= nil then
        batchcmds:show_progress(0, format, ...)
    end
end

local function ensure_vendor_file(required_path, label)
    if not os.isfile(required_path) then
        xmake_raise("missing %s at %s; %s", label, required_path, vendor_hint)
    end
end

local function apply_local_dep(target_name, dep_name)
    local dep = local_deps[dep_name]
    if dep == nil then
        return
    end

    if dep.include_dirs ~= nil and #dep.include_dirs > 0 then
        target(target_name)
            add_includedirs(table_unpack(dep.include_dirs), {public = true})
    end

    if dep.defines ~= nil and #dep.defines > 0 then
        target(target_name)
            add_defines(table_unpack(dep.defines), {public = true})
    end

    if dep.link_dirs ~= nil and #dep.link_dirs > 0 then
        target(target_name)
            add_linkdirs(table_unpack(dep.link_dirs))
    end

    if dep.links ~= nil and #dep.links > 0 then
        target(target_name)
            add_links(table_unpack(dep.links))
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

local function add_internal_repo_includedirs(target_name)
    target(target_name)
    add_sysincludedirs(internal_include_root)
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
    local xml2ffd_bin = generated_tool_path("nimblefix-xml2ffd")
    local dictgen_bin = generated_tool_path("nimblefix-dictgen")
    local fix44_xml = path.join(quickfix_root, "spec", "FIX44.xml")
    local fix44_ffd = path.join("build", "bench", "quickfix_FIX44.ffd")
    local fix44_art = path.join("build", "bench", "quickfix_FIX44.art")
    local fix44_builders = path.join("build", "generated", "fix44_builders.h")

    if not os.isfile(xml2ffd_bin) then
        xmake_raise("missing build tool: %s", xml2ffd_bin)
    end
    if not os.isfile(dictgen_bin) then
        xmake_raise("missing build tool: %s", dictgen_bin)
    end
    if not os.isfile(fix44_xml) then
        xmake_raise("missing QuickFIX FIX44 dictionary at %s; %s", fix44_xml, vendor_hint)
    end

    batchcmds:mkdir(path.join("build", "bench"))
    batchcmds:mkdir(path.join("build", "generated"))

    local needs_fix44_ffd = output_is_stale(fix44_ffd, {fix44_xml, xml2ffd_bin})
    if needs_fix44_ffd then
        batchcmds_show(batchcmds, "[nimblefix] regenerating %s", fix44_ffd)
        batchcmds:vrunv(xml2ffd_bin, {
            "--xml", fix44_xml,
            "--output", fix44_ffd,
            "--profile-id", "4400"
        })
    end

    if needs_fix44_ffd or any_output_is_stale({fix44_art, fix44_builders}, {fix44_ffd, dictgen_bin}) then
        batchcmds_show(batchcmds, "[nimblefix] regenerating %s and %s", fix44_art, fix44_builders)
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
    local dictgen_bin = generated_tool_path("nimblefix-dictgen")
    local sample_profile = path.join("samples", "basic_profile.ffd")
    local sample_overlay = path.join("samples", "basic_overlay.ffd")
    local sample_art = path.join("build", "sample-basic.art")
    local sample_builders = path.join("build", "generated", "sample_basic_builders.h")

    if not os.isfile(dictgen_bin) then
        xmake_raise("missing build tool: %s", dictgen_bin)
    end
    if not os.isfile(sample_profile) then
        xmake_raise("missing sample profile: %s", sample_profile)
    end
    if not os.isfile(sample_overlay) then
        xmake_raise("missing sample overlay: %s", sample_overlay)
    end

    batchcmds:mkdir("build")
    batchcmds:mkdir(path.join("build", "generated"))

    if any_output_is_stale({sample_art, sample_builders}, {sample_profile, sample_overlay, dictgen_bin}) then
        batchcmds_show(batchcmds, "[nimblefix] regenerating %s and %s", sample_art, sample_builders)
        batchcmds:vrunv(dictgen_bin, {
            "--input", sample_profile,
            "--merge", sample_overlay,
            "--output", sample_art,
            "--cpp-builders", sample_builders
        })
    end
end

rule("nimblefix.fix44_assets")
    before_buildcmd(function (target, batchcmds, opt)
        enqueue_fix44_assets(batchcmds)
    end)

rule("nimblefix.sample_assets")
    before_buildcmd(function (target, batchcmds, opt)
        enqueue_sample_assets(batchcmds)
    end)

target("nimblefix-thirdparty-pugixml")
    set_kind("static")
    set_default(false)
    set_warnings("none")
    add_includedirs("deps/src/pugixml/src", {public = true})
    add_files("deps/src/pugixml/src/pugixml.cpp")

target("nimblefix-thirdparty-catch2-main")
    set_kind("static")
    set_default(false)
    set_warnings("none")
    add_includedirs(catch2_override_root, {public = true})
    add_includedirs("deps/src/Catch2/src", {public = true})
    add_includedirs("deps/src/Catch2/extras")
    add_files("deps/src/Catch2/extras/catch_amalgamated.cpp")

target("nimblefix")
    set_kind("static")
    add_includedirs(public_include_root, {public = true})
    add_includedirs(internal_include_root)
    add_defines("NIMBLEFIX_PROJECT_DIR=\"$(projectdir)\"", {public = true})
    if has_config("nimblefix_enable_tls") then
        add_packages("openssl", {public = true})
        add_defines("NIMBLEFIX_ENABLE_TLS", {public = true})
    end
    add_files("src/profile/*.cpp", "src/runtime/*.cpp", "src/session/*.cpp", "src/store/*.cpp", "src/message/*.cpp", "src/codec/*.cpp", "src/transport/*.cpp")
    if os.isfile("/usr/include/liburing.h") or os.isfile("/usr/local/include/liburing.h") then
        add_syslinks("uring")
    else
        add_defines("NIMBLEFIX_DISABLE_LIBURING")
    end

target("nimblefix-dictgen")
    set_kind("binary")
    set_policy("build.fence", true)
    add_deps("nimblefix")
    add_files("tools/dictgen/*.cpp")

target("nimblefix-interop-runner")
    set_kind("binary")
    add_deps("nimblefix")
    add_files("tools/interop-runner/*.cpp")

target("nimblefix-fix-session-testcases")
    set_kind("binary")
    add_deps("nimblefix")
    add_files("tools/fix-session-testcases/*.cpp")

target("nimblefix-soak")
    set_kind("binary")
    add_deps("nimblefix")
    add_files("tools/soak/*.cpp")

target("nimblefix-fuzz-config")
    set_kind("binary")
    add_deps("nimblefix")
    add_files("tools/fuzz-config/*.cpp")

target("nimblefix-fuzz-dictgen")
    set_kind("binary")
    add_deps("nimblefix")
    add_files("tools/fuzz-dictgen/*.cpp")

target("nimblefix-fuzz-codec")
    set_kind("binary")
    add_deps("nimblefix")
    add_files("tools/fuzz-codec/main.cpp")

target("nimblefix-fuzz-codec-libfuzzer")
    set_kind("binary")
    set_default(false)
    add_deps("nimblefix")
    add_files("tools/fuzz-codec/fuzz_entry.cpp")
    add_cxflags("-fsanitize=fuzzer", {force = true})
    add_ldflags("-fsanitize=fuzzer", {force = true})

target("nimblefix-initiator")
    set_kind("binary")
    add_deps("nimblefix")
    add_files("tools/initiator/*.cpp")

target("nimblefix-acceptor")
    set_kind("binary")
    add_deps("nimblefix")
    add_files("tools/acceptor/*.cpp")

target("nimblefix-xml2ffd")
    set_kind("binary")
    set_policy("build.fence", true)
    add_deps("nimblefix", "nimblefix-thirdparty-pugixml")
    add_files("tools/xml2ffd/*.cpp")

target("nimblefix-fix44-assets")
    set_kind("phony")
    set_default(false)
    set_policy("build.fence", true)
    add_deps("nimblefix-xml2ffd", "nimblefix-dictgen")
    add_rules("nimblefix.fix44_assets")

target("nimblefix-sample-assets")
    set_kind("phony")
    set_default(false)
    set_policy("build.fence", true)
    add_deps("nimblefix-dictgen")
    add_rules("nimblefix.sample_assets")

target("nimblefix-generated-assets")
    set_kind("phony")
    set_default(false)
    set_policy("build.fence", true)
    add_deps("nimblefix-fix44-assets", "nimblefix-sample-assets")

target("nimblefix-bench")
    set_kind("binary")
    add_deps("nimblefix", "nimblefix-fix44-assets")
    add_includedirs("build/generated")
    add_files("bench/main.cpp")

target("nimblefix-tls-transport-bench")
    set_kind("binary")
    set_default(false)
    add_deps("nimblefix")
    add_files("bench/tls_transport.cpp")

target("nimblefix-vendor-quickfix")
    set_kind("static")
    set_default(false)
    set_warnings("none")
    set_languages("cxx17")
    add_defines("NOMINMAX", "_FILE_OFFSET_BITS=64")
    add_includedirs(path.join("deps", "include", "quickfix"), path.join(quickfix_root, "src", "C++"), {public = true})
    add_includedirs(quickfix_root, path.join(quickfix_root, "src"))
    add_files(table_unpack(quickfix_vendor_sources))
    if is_plat("linux") then
        add_syslinks("pthread")
    end

target("nimblefix-quickfix-cpp-bench")
    set_kind("binary")
    set_default(false)
    set_basename("quickfix-cpp-bench")
    set_languages("cxx17")
    add_deps("nimblefix-vendor-quickfix")
    add_includedirs("bench")
    add_files("bench/quickfix_main.cpp")
    if is_plat("linux") then
        add_syslinks("pthread")
    end

target("nimblefix-tests")
    set_kind("binary")
    add_deps("nimblefix", "nimblefix-thirdparty-catch2-main", "nimblefix-thirdparty-pugixml", "nimblefix-generated-assets")
    add_includedirs("build/generated")
    add_files("tests/*.cpp", "tools/xml2ffd/xml2ffd.cpp")
    remove_files("tests/test_main.cpp")

add_internal_repo_includedirs("nimblefix-dictgen")
add_internal_repo_includedirs("nimblefix-fix-session-testcases")
add_internal_repo_includedirs("nimblefix-interop-runner")
add_internal_repo_includedirs("nimblefix-soak")
add_internal_repo_includedirs("nimblefix-fuzz-config")
add_internal_repo_includedirs("nimblefix-fuzz-dictgen")
add_internal_repo_includedirs("nimblefix-fuzz-codec")
add_internal_repo_includedirs("nimblefix-fuzz-codec-libfuzzer")
add_internal_repo_includedirs("nimblefix-initiator")
add_internal_repo_includedirs("nimblefix-acceptor")
add_internal_repo_includedirs("nimblefix-xml2ffd")
add_internal_repo_includedirs("nimblefix-bench")
add_internal_repo_includedirs("nimblefix-tls-transport-bench")
add_internal_repo_includedirs("nimblefix-tests")

apply_local_dep("nimblefix", "nimblefix")
apply_local_dep("nimblefix-dictgen", "nimblefix-dictgen")
apply_local_dep("nimblefix-fix-session-testcases", "nimblefix-fix-session-testcases")
apply_local_dep("nimblefix-interop-runner", "nimblefix-interop-runner")
apply_local_dep("nimblefix-soak", "nimblefix-soak")
apply_local_dep("nimblefix-fuzz-config", "nimblefix-fuzz-config")
apply_local_dep("nimblefix-fuzz-dictgen", "nimblefix-fuzz-dictgen")
apply_local_dep("nimblefix-fuzz-codec", "nimblefix-fuzz-codec")
apply_local_dep("nimblefix-fuzz-codec-libfuzzer", "nimblefix-fuzz-codec-libfuzzer")
apply_local_dep("nimblefix-initiator", "nimblefix-initiator")
apply_local_dep("nimblefix-acceptor", "nimblefix-acceptor")
apply_local_dep("nimblefix-bench", "nimblefix-bench")
apply_local_dep("nimblefix-tls-transport-bench", "nimblefix-tls-transport-bench")
apply_local_dep("nimblefix-vendor-quickfix", "nimblefix-vendor-quickfix")
apply_local_dep("nimblefix-quickfix-cpp-bench", "nimblefix-quickfix-cpp-bench")
apply_local_dep("nimblefix-tests", "nimblefix-tests")

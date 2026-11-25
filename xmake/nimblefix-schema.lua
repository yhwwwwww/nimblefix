-- Reusable XML -> C++20 schema codegen rule using the native nimblefix-schema-gen binary.
-- Zero Python dependency; generator is resolved relative to this rule or supplied explicitly.
local rule_directory = os.scriptdir()

rule("nimblefix.schema")
    on_load(function (target)
        local name_space = target:values("nimblefix.schema.namespace") or "venue"
        local header = target:values("nimblefix.schema.header") or "venue.hpp"
        if path.is_absolute(header) then raise("nimblefix.schema.header must be relative") end
        for part in header:gmatch("[^/\\]+") do
            if part == ".." then raise("nimblefix.schema.header cannot traverse its output directory") end
        end
        local xml_files = table.wrap(target:values("nimblefix.schema.xml_files"))
        if #xml_files == 0 or #xml_files > 32 then raise("nimblefix.schema requires 1..32 XML inputs") end
        for index, filename in ipairs(xml_files) do xml_files[index] = path.absolute(filename, os.projectdir()) end
        local generator = target:values("nimblefix.schema.generator")
        if not generator then
            for _, candidate in ipairs({
                    path.join(rule_directory, "..", "..", "..", "bin", "nimblefix-schema-gen"),
                    path.join(rule_directory, "..", "bin", "nimblefix-schema-gen")}) do
                if os.isfile(candidate) then generator = candidate; break end
            end
        end
        if not generator or not os.isfile(generator) then
            raise("set nimblefix.schema.generator to the installed nimblefix-schema-gen executable")
        end
        local output_dir = path.join(target:autogendir(), "nimblefix-schema")
        target:add("includedirs", output_dir)
        local messages = table.wrap(target:values("nimblefix.schema.messages"))
        target:data_set("nimblefix.schema", {namespace = name_space, header = path.join(output_dir, header),
            generator = path.absolute(generator), xml = xml_files, messages = messages})
    end)
    before_build(function (target)
        import("core.project.depend")
        local settings = target:data("nimblefix.schema")
        local inputs = table.join({settings.generator}, settings.xml)
        local msg_csv = table.concat(settings.messages or {}, ",")
        depend.on_changed(function ()
            os.mkdir(path.directory(settings.header))
            local argv = {"--namespace", settings.namespace, "--output", settings.header}
            if #msg_csv > 0 then table.insert(argv, "--messages"); table.insert(argv, msg_csv) end
            os.vrunv(settings.generator, table.join(argv, settings.xml))
        end, {dependfile = path.join(target:autogendir(), "nimblefix-schema.depend"), files = inputs,
            values = {settings.namespace, settings.header, msg_csv}, changed = not os.isfile(settings.header)})
    end)
rule_end()

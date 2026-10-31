-- Reusable XML -> C++ rule. The generator is resolved relative to this rule,
-- or explicitly supplied by the consuming target; no network fetch occurs.
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
            for _, candidate in ipairs({path.join(rule_directory, "..", "schema", "generate.py"),
                    path.join(rule_directory, "..", "tools", "schema", "generate.py")}) do
                if os.isfile(candidate) then generator = candidate; break end
            end
        end
        if not generator or not os.isfile(generator) then raise("set nimblefix.schema.generator to the installed generate.py") end
        local output_dir = path.join(target:autogendir(), "nimblefix-schema")
        target:add("includedirs", output_dir)
        target:data_set("nimblefix.schema", {namespace = name_space, header = path.join(output_dir, header),
            generator = path.absolute(generator), xml = xml_files})
    end)
    before_build(function (target)
        import("core.project.depend")
        local settings = target:data("nimblefix.schema")
        local inputs = table.join({settings.generator}, settings.xml)
        depend.on_changed(function ()
            os.mkdir(path.directory(settings.header))
            os.vrunv("python3", table.join({settings.generator, "--namespace", settings.namespace,
                "--output", settings.header}, settings.xml))
        end, {dependfile = path.join(target:autogendir(), "nimblefix-schema.depend"), files = inputs,
            values = {settings.namespace, settings.header}, changed = not os.isfile(settings.header)})
    end)
rule_end()

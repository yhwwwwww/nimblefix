#!/usr/bin/env python3
"""Exercise the reusable XML rule with an isolated xmake configuration/build."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def invoke(command, cwd, env):
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"{' '.join(map(str, command))}\n{result.stdout}")
    return result.stdout


def build_project(xmake, project, output, env):
    configure = [xmake, "f", "-P", str(project), "-o", str(output), "-m", "release", "-y"]
    if env.get("CC"):
        configure.append(f"--cc={env['CC']}")
    if env.get("CXX"):
        configure.append(f"--cxx={env['CXX']}")
        configure.append(f"--ld={env['CXX']}")
    invoke(configure, project, env)
    invoke([xmake, "-P", str(project), "-j", "1"], project, env)


def lua_string(value):
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def write_project(path, rule, include_dir, fields, message, namespace="external::venue",
    header="order.hpp", generator=None, reverse=False):
    inputs = ["messages.xml", "fields.xml"] if reverse else ["fields.xml", "messages.xml"]
    values = ", ".join(lua_string(value) for value in inputs)
    generator_value = ""
    if generator is not None:
        generator_value = f'    set_values("nimblefix.schema.generator", {lua_string(generator)})\n'
    content = f'''set_project("nimblefix-schema-install-test")
set_languages("cxx20")
includes({lua_string(rule)})
add_includedirs({lua_string(include_dir)})

target("schema-consumer")
    set_kind("binary")
    add_rules("nimblefix.schema")
    set_values("nimblefix.schema.namespace", {lua_string(namespace)})
    set_values("nimblefix.schema.header", {lua_string(header)})
    set_values("nimblefix.schema.xml_files", {{{values}}})
{generator_value}    add_files("main.cpp")
target_end()
'''
    path.joinpath("xmake.lua").write_text(content, encoding="utf-8")
    path.joinpath("fields.xml").write_text(fields, encoding="utf-8")
    path.joinpath("messages.xml").write_text(message, encoding="utf-8")
    path.joinpath("main.cpp").write_text(
        f'#include <array>\n#include "{header}"\n'
        'int main() { std::array<char, 128> buf{}; '
        'auto r = nimblefix::encode_body_stream<external::venue::CustomOrder>(buf, [](auto& w) { w.cl_ord_id("EXTERNAL"); }); '
        'return r.ok() ? 0 : 1; }\n', encoding="utf-8")


def fingerprint(header):
    text = header.read_text(encoding="utf-8")
    match = re.search(r"normalized fingerprint ([0-9a-f]{64})", text)
    if not match:
        raise RuntimeError(f"generated header has no normalized fingerprint: {header}")
    return match.group(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--xmake", default=shutil.which("xmake") or "xmake")
    parser.add_argument("--rule", type=Path, default=ROOT / "xmake/nimblefix-schema.lua")
    parser.add_argument("--generator", type=Path, default=ROOT / "tools/schema/generate.py")
    parser.add_argument("--include-dir", type=Path, default=ROOT / "include")
    args = parser.parse_args()
    rule = args.rule.resolve()
    generator = args.generator.resolve()
    include_dir = args.include_dir.resolve()
    for path in (rule, generator, include_dir):
        if not path.exists():
            raise RuntimeError(f"required installed/repository input is missing: {path}")

    with tempfile.TemporaryDirectory(prefix="nimblefix-xmake-schema-") as temporary:
        base = Path(temporary)
        project = base / "external-project"
        build_dir = base / "external-build"
        project.mkdir()
        environment = os.environ.copy()
        # Keep both project and global xmake state away from the user's shared
        # project cache and the repository build directory.
        environment["XMAKE_CONFIGDIR"] = str(base / "xmake-config")
        environment["XMAKE_GLOBALDIR"] = str(base / "xmake-global")

        fields_xml = '''<?xml version="1.0"?>
<fix type="FIX" major="4" minor="4">
  <fields><field number="11" name="ClOrdID" type="STRING">
    <value enum="EXTERNAL" description="Initial fingerprint marker"/>
  </field></fields>
</fix>
'''
        message_xml = '''<?xml version="1.0"?>
<fix type="FIX" major="4" minor="4">
  <messages><message name="CustomOrder" msgtype="Z" msgcat="app">
    <field name="ClOrdID" required="Y"/>
  </message></messages>
</fix>
'''

        # First build uses the rule's default sibling-generator lookup. With an
        # installed prefix this checks the relocated xmake/ and schema/ layout.
        write_project(project, rule, include_dir, fields_xml, message_xml)
        build_project(args.xmake, project, build_dir, environment)
        generated = list(build_dir.rglob("order.hpp"))
        if len(generated) != 1:
            raise RuntimeError(f"expected one generated order.hpp, found {generated}")
        output = generated[0]
        original_bytes = output.read_bytes()
        original_fingerprint = fingerprint(output)
        text = output.read_text(encoding="utf-8")
        if "CustomOrderStreamWriter" not in text or "FieldWireType" not in text:
            raise RuntimeError("generated header is missing typed schema metadata")

        # Wrap the same generator only to count invocations. This lets the
        # test distinguish a no-op dependency check from a rerun whose output
        # happens to be byte-identical.
        call_log = base / "generator-calls.txt"
        wrapper = project / "counted-generator.py"
        wrapper.write_text(
            "import pathlib, subprocess, sys\n"
            f"pathlib.Path({str(call_log)!r}).open('a', encoding='utf-8').write('call\\n')\n"
            f"subprocess.run([sys.executable, {str(generator)!r}, *sys.argv[1:]], check=True)\n",
            encoding="utf-8")
        write_project(project, rule, include_dir, fields_xml, message_xml,
            generator=wrapper)
        build_project(args.xmake, project, build_dir, environment)
        if call_log.read_text(encoding="utf-8").splitlines() != ["call"]:
            raise RuntimeError("changing the generator dependency did not trigger exactly one generation")

        build_project(args.xmake, project, build_dir, environment)
        if call_log.read_text(encoding="utf-8").splitlines() != ["call"]:
            raise RuntimeError("unchanged XML and namespace reran the schema generator")
        if output.read_bytes() != original_bytes:
            raise RuntimeError("no-op build changed generated output")

        fields_xml = fields_xml.replace("Initial fingerprint marker", "Changed fingerprint marker with new length")
        project.joinpath("fields.xml").write_text(fields_xml, encoding="utf-8")
        timestamp = time.time_ns() + 2_000_000_000
        os.utime(project / "fields.xml", ns=(timestamp, timestamp))
        build_project(args.xmake, project, build_dir, environment)
        changed_fingerprint = fingerprint(output)
        if changed_fingerprint == original_fingerprint or "Changed fingerprint marker" not in output.read_text(encoding="utf-8"):
            raise RuntimeError("XML edit did not regenerate the header fingerprint and enum metadata")
        if call_log.read_text(encoding="utf-8").splitlines() != ["call", "call"]:
            raise RuntimeError("XML change did not cause exactly one schema generation")

        # XML inputs are peer documents and may be listed in either order.
        before_reverse = output.read_bytes()
        write_project(project, rule, include_dir, fields_xml, message_xml,
            generator=wrapper, reverse=True)
        build_project(args.xmake, project, build_dir, environment)
        if output.read_bytes() != before_reverse or fingerprint(output) != changed_fingerprint:
            raise RuntimeError("multi-file XML generation depends on input order")

        # Namespace and output names are rule values; both must invalidate the
        # generated output even when XML inputs are unchanged.
        write_project(project, rule, include_dir, fields_xml, message_xml,
            namespace="external::renamed", generator=wrapper, reverse=True)
        source = project / "main.cpp"
        source.write_text(
            '#include <array>\n#include "order.hpp"\n'
            'int main() { std::array<char, 128> buf{}; '
            'auto r = nimblefix::encode_body_stream<external::renamed::CustomOrder>(buf, [](auto& w) { w.cl_ord_id("EXTERNAL"); }); '
            'return r.ok() ? 0 : 1; }\n', encoding="utf-8")
        build_project(args.xmake, project, build_dir, environment)
        if "namespace external::renamed" not in output.read_text(encoding="utf-8"):
            raise RuntimeError("namespace change was not reflected in the generated header")

        write_project(project, rule, include_dir, fields_xml, message_xml,
            namespace="external::renamed", header="renamed.hpp", generator=wrapper, reverse=True)
        source.write_text(
            '#include <array>\n#include "renamed.hpp"\n'
            'int main() { std::array<char, 128> buf{}; '
            'auto r = nimblefix::encode_body_stream<external::renamed::CustomOrder>(buf, [](auto& w) { w.cl_ord_id("EXTERNAL"); }); '
            'return r.ok() ? 0 : 1; }\n', encoding="utf-8")
        build_project(args.xmake, project, build_dir, environment)
        renamed = list(build_dir.rglob("renamed.hpp"))
        if len(renamed) != 1 or "namespace external::renamed" not in renamed[0].read_text(encoding="utf-8"):
            raise RuntimeError("header path change did not produce the requested generated header")
        binary = [path for path in build_dir.rglob("schema-consumer") if path.is_file() and os.access(path, os.X_OK)]
        if len(binary) != 1:
            raise RuntimeError(f"expected one compiled external consumer, found {binary}")
        invoke([str(binary[0])], base, environment)

    print("xmake XML rule default lookup, multi-input merge, incrementality, fingerprint, namespace, and header changes passed")


if __name__ == "__main__":
    main()

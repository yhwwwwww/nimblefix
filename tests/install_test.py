#!/usr/bin/env python3
"""Build from a read-only copy; install and consume without the source/build tree."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*command, cwd=None):
    result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"{' '.join(map(str, command))}\n{result.stdout}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--core-only", action="store_true")
    modes.add_argument("--features", action="store_true",
        help="enable system TLS, runtime XML, and all standard schema bindings")
    parser.add_argument("--examples", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nimblefix-install-") as temporary:
        base = Path(temporary)
        source, build, prefix = base / "source", base / "build", base / "prefix"
        source.mkdir()
        for directory in ("include", "src", "cmake", "tools/schema", "tools/nimblefix", "schemas", "examples", "xmake"):
            shutil.copytree(ROOT / directory, source / directory)
        (source / "third_party/tomlplusplus").mkdir(parents=True)
        for filename in (
            "CMakeLists.txt",
            "LICENSE",
            "third_party/README.md",
            "third_party/quickfix-LICENSE",
            "third_party/tomlplusplus/LICENSE",
            "third_party/tomlplusplus/toml.hpp",
        ):
            shutil.copyfile(ROOT / filename, source / filename)
        if args.examples:
            (source / "tests").mkdir()
            shutil.copyfile(ROOT / "tests/hello_fix.cpp", source / "tests/hello_fix.cpp")
        for item in source.rglob("*"):
            item.chmod(0o555 if item.is_dir() else 0o444)
        source.chmod(0o555)
        try:
            configure = ["cmake", "-S", str(source), "-B", str(build),
                "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_CXX_COMPILER={args.cxx}",
                "-DNIMBLEFIX_BUILD_TESTS=OFF", f"-DNIMBLEFIX_BUILD_EXAMPLES={'ON' if args.examples else 'OFF'}",
                "-DNIMBLEFIX_BUILD_CLI=ON",
                f"-DNIMBLEFIX_BUILD_SAMPLE_SCHEMA={'ON' if not args.core_only and (not args.features or args.examples) else 'OFF'}",
                f"-DNIMBLEFIX_BUILD_STANDARD_SCHEMAS={'ON' if args.features else 'OFF'}",
                f"-DNIMBLEFIX_ENABLE_TLS={'ON' if args.features else 'OFF'}",
                f"-DNIMBLEFIX_ENABLE_XML={'ON' if args.features else 'OFF'}"]
            if args.core_only:
                # Make accidental optional package discovery a configure-time
                # failure, proving the core/CLI install has no such dependency.
                configure.extend(("-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE",
                    "-DCMAKE_DISABLE_FIND_PACKAGE_LibXml2=TRUE",
                    "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE"))
            run(*configure)
            run("cmake", "--build", str(build), "--parallel", "1" if args.features else "2")
            run("cmake", "--install", str(build), "--prefix", str(prefix))
            # Relocate the installed package and make both build paths unavailable.
            relocated = base / "relocated"
            prefix.rename(relocated)
            source.rename(base / "hidden-source")
            build.rename(base / "hidden-build")
            consumer, external_build = base / "outside-consumer", base / "outside-build"
            shutil.copytree(ROOT / "tests/consumer", consumer)
            run("cmake", "-S", str(consumer), "-B", str(external_build),
                f"-DCMAKE_PREFIX_PATH={relocated}", f"-DCMAKE_CXX_COMPILER={args.cxx}",
                f"-DCONSUMER_SCHEMA={'OFF' if args.core_only else 'ON'}",
                f"-DCONSUMER_FEATURES={'ON' if args.features else 'OFF'}")
            run("cmake", "--build", str(external_build), "--parallel", "1" if args.features else "2")
            run(str(external_build / "consumer"), cwd=base)
            cli = relocated / "bin" / "nimblefix"
            doctor = json.loads(subprocess.run([str(cli), "doctor", "--json"], cwd=base,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True).stdout)
            if doctor["tls"] != bool(args.features):
                raise RuntimeError(f"installed CLI reports tls={doctor['tls']}, expected {bool(args.features)}")
            cli_config_dir = base / "installed cli config"
            initialized = subprocess.run([str(cli), "init", "--directory", str(cli_config_dir), "--json"],
                cwd=base, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
            config_path = Path(json.loads(initialized.stdout)["acceptor"])
            validated = subprocess.run([str(cli), "validate", "--config", str(config_path), "--json"],
                cwd=base, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
            if not json.loads(validated.stdout).get("valid"):
                raise RuntimeError("installed CLI did not validate its generated acceptor config")
            if args.examples:
                run(str(relocated / "bin/hello-fix"), "--self-test", "--restart", cwd=base)
                if not args.core_only:
                    run(str(relocated / "bin/hello-fix"), "--self-test", "--typed", "--restart", cwd=base)
                run(sys.executable, str(ROOT / "tests/endpoint_test.py"),
                    "--acceptor", str(relocated / "bin/acceptor-example"),
                    "--initiator", str(relocated / "bin/initiator-example"), cwd=base)
            if not args.core_only:
                run(str(external_build / "schema-consumer"), cwd=base)
                generated = external_build / "generated/venue_schema/venue.hpp"
                original = generated.read_bytes()
                original_mtime = generated.stat().st_mtime_ns
                run("cmake", "--build", str(external_build), "--parallel", "1" if args.features else "2")
                if generated.stat().st_mtime_ns != original_mtime:
                    raise RuntimeError("no-op installed helper build rewrote its generated header")
                # Verify both incrementality and that changing XML actually
                # changes generated metadata, without a manual codegen step.
                xml = consumer / "venue.xml"
                xml.write_text(xml.read_text().replace('type="STRING"/>',
                    'type="STRING"><value enum="external" description="Changed"/></field>'))
                stamp = max(xml.stat().st_mtime_ns, generated.stat().st_mtime_ns) + 2_000_000_000
                os.utime(xml, ns=(stamp, stamp))
                run("cmake", "--build", str(external_build), "-j", "2")
                if generated.read_bytes() == original:
                    raise RuntimeError("XML edit did not regenerate the installed helper's header")
                run(str(external_build / "schema-consumer"), cwd=base)
                if args.features:
                    run(str(external_build / "features"), cwd=base)
                    run(str(external_build / "xml-runtime"), str(consumer / "venue.xml"), cwd=base)
                    schema_summary = subprocess.run([str(cli), "schema", "check", "--xml",
                        str(consumer / "venue.xml"), "--json"], cwd=base,
                        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
                    if not json.loads(schema_summary.stdout).get("valid"):
                        raise RuntimeError("installed CLI did not load the runtime XML schema")
                    cli_header = base / "cli generated" / "venue.hpp"
                    cli_header.parent.mkdir(parents=True)
                    run(str(cli), "schema", "generate", "--xml", str(consumer / "venue.xml"),
                        "--namespace", "installed::venue", "--output", str(cli_header), cwd=base)
                    if not cli_header.is_file() or b"installed::venue" not in cli_header.read_bytes():
                        raise RuntimeError("installed CLI schema generator did not produce the requested header")
                    # The installed xmake helper must find its sibling generator
                    # after relocation, with both the copied source and build hidden.
                    run(sys.executable, str(ROOT / "tests/xmake_schema_test.py"),
                        "--rule", str(relocated / "share/nimblefix/xmake/nimblefix-schema.lua"),
                        "--generator", str(relocated / "share/nimblefix/schema/generate.py"),
                        "--include-dir", str(relocated / "include"), cwd=base)
            # Unknown requested components must fail during find_package.
            cmakelists = consumer / "CMakeLists.txt"
            cmakelists.write_text(cmakelists.read_text().replace("COMPONENTS core", "COMPONENTS nonexistent"))
            failed = subprocess.run(["cmake", "-S", str(consumer), "-B", str(base / "unknown-component"),
                f"-DCMAKE_PREFIX_PATH={relocated}", f"-DCMAKE_CXX_COMPILER={args.cxx}",
                f"-DCONSUMER_FEATURES={'ON' if args.features else 'OFF'}"],
                text=True, capture_output=True)
            failure_text = failed.stdout + failed.stderr
            if failed.returncode == 0 or "NimbleFIX" not in failure_text:
                raise RuntimeError(f"unknown required component was not rejected clearly:\n{failure_text}")
        finally:
            # TemporaryDirectory cleanup also needs write permissions on the copy.
            for candidate in (source, base / "hidden-source"):
                if candidate.exists():
                    candidate.chmod(0o755)
                    for item in candidate.rglob("*"):
                        if item.is_dir(): item.chmod(0o755)
    mode = "core only" if args.core_only else "TLS/XML/standard features" if args.features else "sample XML"
    print(f"read-only build, relocated install, external consumer, and installed CLI passed ({mode})")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build from a read-only copy; install and consume without the source/build tree."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*command, cwd=None):
    result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"{' '.join(map(str, command))}\n{result.stdout}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--core-only", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nimblefix-install-") as temporary:
        base = Path(temporary)
        source, build, prefix = base / "source", base / "build", base / "prefix"
        source.mkdir()
        for directory in ("include", "src", "cmake", "tools/schema", "schemas"):
            shutil.copytree(ROOT / directory, source / directory)
        for filename in ("CMakeLists.txt", "LICENSE"):
            shutil.copyfile(ROOT / filename, source / filename)
        for item in source.rglob("*"):
            item.chmod(0o555 if item.is_dir() else 0o444)
        source.chmod(0o555)
        try:
            run("cmake", "-S", str(source), "-B", str(build),
                "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_CXX_COMPILER={args.cxx}",
                "-DNIMBLEFIX_BUILD_TESTS=OFF", "-DNIMBLEFIX_BUILD_EXAMPLES=OFF",
                f"-DNIMBLEFIX_BUILD_SAMPLE_SCHEMA={'OFF' if args.core_only else 'ON'}")
            run("cmake", "--build", str(build), "-j", "2")
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
                f"-DCONSUMER_SCHEMA={'OFF' if args.core_only else 'ON'}")
            run("cmake", "--build", str(external_build), "-j", "2")
            run(str(external_build / "consumer"), cwd=base)
            if not args.core_only:
                run(str(external_build / "schema-consumer"), cwd=base)
                generated = external_build / "generated/venue_schema/venue.hpp"
                original = generated.read_bytes()
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
            # Unknown requested components must fail during find_package.
            cmakelists = consumer / "CMakeLists.txt"
            cmakelists.write_text(cmakelists.read_text().replace("COMPONENTS core", "COMPONENTS nonexistent"))
            failed = subprocess.run(["cmake", "-S", str(consumer), "-B", str(base / "unknown-component"),
                f"-DCMAKE_PREFIX_PATH={relocated}"], text=True, capture_output=True)
            if failed.returncode == 0:
                raise RuntimeError("unknown required component was accepted")
        finally:
            # TemporaryDirectory cleanup also needs write permissions on the copy.
            for candidate in (source, base / "hidden-source"):
                if candidate.exists():
                    candidate.chmod(0o755)
                    for item in candidate.rglob("*"):
                        if item.is_dir(): item.chmod(0o755)
    print("read-only build, relocated install, and external consumer passed" + (" (core only)" if args.core_only else " (with XML regeneration)"))


if __name__ == "__main__":
    main()

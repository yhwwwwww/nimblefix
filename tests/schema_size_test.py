#!/usr/bin/env python3
"""Hold the actual shape fixed while inflating unused schema and sparse tag IDs."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def run(command):
    return subprocess.run(command, check=True, text=True, capture_output=True).stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    results = []
    with tempfile.TemporaryDirectory(prefix="nimblefix-schema-size-") as temporary:
        base = Path(temporary)
        for optional_count, tag_stride in ((0, 1), (1000, 1), (1000, 1000)):
            tag = 900001
            fields = '<field number="11" name="ClOrdID" type="STRING"/>'
            fields += f'<field number="{tag}" name="CustomValue" type="STRING"/>'
            members = '<field name="ClOrdID" required="Y"/><field name="CustomValue" required="N"/>'
            for index in range(optional_count):
                fields += f'<field number="{1000 + index * tag_stride}" name="Extra{index}" type="STRING"/>'
                members += f'<field name="Extra{index}" required="N"/>'
            xml = base / "schema.xml"
            xml.write_text(f'<fix type="FIX" major="4" minor="4"><fields>{fields}</fields>'
                           f'<messages><message name="Sample" msgtype="Z" msgcat="app">{members}</message></messages></fix>')
            header = base / "sample.hpp"
            start = time.monotonic()
            run(["python3", str(ROOT / "tools/schema/generate.py"), "--namespace", "sample", "--output", str(header), str(xml)])
            generated_seconds = time.monotonic() - start
            source = base / "main.cpp"
            source.write_text('''#include <iostream>
#include "sample.hpp"
int main() {
  using Message = decltype(sample::Sample::make().cl_ord_id("x").custom_value("y"));
  static_assert(Message::slot_count == 2);
  static_assert(Message::slot_storage_bytes == 32);
  std::cout << sizeof(Message) << " " << Message::slot_storage_bytes;
}
''')
            binary = base / "probe"
            start = time.monotonic()
            run([args.cxx, "-std=c++20", "-O2", "-I", str(ROOT / "include"), str(source), "-o", str(binary)])
            compile_seconds = time.monotonic() - start
            builder_bytes, slot_bytes = map(int, run([str(binary)]).split())
            results.append({"unused_optional_fields": optional_count, "tag_stride": tag_stride,
                "builder_bytes": builder_bytes, "slot_bytes": slot_bytes,
                "generated_header_bytes": header.stat().st_size, "binary_bytes": binary.stat().st_size,
                "generation_seconds": generated_seconds, "compile_seconds": compile_seconds})
        if len({(row["builder_bytes"], row["slot_bytes"]) for row in results}) != 1:
            raise RuntimeError("unused schema fields inflated the actual message payload")
    report = json.dumps({"compiler": args.cxx, "measurements": results}, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report + "\n")
    print(report)


if __name__ == "__main__":
    main()

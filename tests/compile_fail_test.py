#!/usr/bin/env python3
"""Check rejected public API uses and the diagnostic that explains each one."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

CASES = {
    "managed_tag": ('auto x = nf::message<"D">().set<34>(1);', "engine managed"),
    "managed_possdup": ('auto x = nf::message<"D">().set<43>(true);', "engine managed"),
    "managed_origtime": ('auto x = nf::message<"D">().set<122>("20261031-00:00:00");', "engine managed"),
    "managed_processed": ('auto x = nf::message<"D">().set<369>(1);', "engine managed"),
    "invalid_tag": ('auto x = nf::message<"D">().set<0>(1);', "FIX tags must"),
    "duplicate_flat_tag": ('auto x = nf::message<"D">().set<11>("a").set<11>("b");', "same tag twice"),
    "runtime_tag": ('int tag = 11; auto x = nf::message<"D">().set(tag, "a");', "set"),
    "binary_float": ('auto x = nf::message<"D">().set<44>(1.25);', "unsupported static FIX"),
    "invalid_decimal_literal": ('constexpr auto x = "not-a-price"_dec;', "constant expression"),
    "unshaped_set": ('nf::FixedMessage<"D", nf::Fields<nf::Field<11, nf::FieldKind::string>>> x; (void)x.set<55>("a");', "must name a field"),
    "wrong_fixed_type": ('nf::FixedMessage<"D", nf::Fields<nf::Field<11, nf::FieldKind::string>>> x; (void)x.set<11>(4);', "value type does not match"),
}

GENERATED_CASES = {
    "quantity_string": ('auto x = nf::fix44_sample::NewOrderSingle::make().order_qty("bad");', "generated FIX field type"),
    "tagged_price_string": ('auto x = nf::fix44_sample::NewOrderSingle::make().set<44>("bad");', "generated FIX field type"),
    "side_integer": ('auto x = nf::fix44_sample::NewOrderSingle::make().side(1);', "generated FIX field type"),
    "typed_generic_callback": ('nf::Engine<nf::fix44_sample::Schema>& e=*static_cast<nf::Engine<nf::fix44_sample::Schema>*>(nullptr); (void)e.on_message([](nf::SessionContext&, const nf::FlatMessageView&) { return nf::Status::success(); });', "on_message"),
    "typed_wrong_view": ('nf::Engine<nf::fix44_sample::Schema>& e=*static_cast<nf::Engine<nf::fix44_sample::Schema>*>(nullptr); (void)e.on<nf::fix44_sample::NewOrderSingle>([](nf::SessionContext&, int) { return nf::Status::success(); });', "on"),
    "unknown_generated_field": ('auto x = nf::fix44_sample::NewOrderSingle::make().set<900001>("x");', "not allowed"),
}

ENGINE_CASES = {
    "unsupported_engine_schema": ('(void)sizeof(nf::Engine<int>);', "typed Engine Schema must expose descriptor()"),
    "wrong_message_callback": ('nf::Engine<>& engine = *static_cast<nf::Engine<>*>(nullptr); (void)engine.on_message([](int) {});', "on_message"),
    "invalid_execution_policy": ('(void)sizeof(nf::Engine<nf::FlatSchema, int>);', "Engine Policy must"),
    "retained_session_context": ('void save(nf::SessionContext&); auto f=[](nf::SessionContext& context) { auto retained = context; };', "deleted"),
    "wrong_active_callback": ('nf::Engine<>& engine = *static_cast<nf::Engine<>*>(nullptr); (void)engine.on_session_active([]() {});', "on_session_active"),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nimblefix-compile-") as temp:
        source = Path(temp) / "case.cpp"
        command = [args.cxx, "-std=c++20", "-fsyntax-only", "-I", str(ROOT / "include"), str(source)]
        # A working positive control prevents missing compilers/headers from
        # accidentally making every expected-failure case appear to pass.
        prefix = '#include <nimblefix/message.hpp>\nnamespace nf=nimblefix;\nusing namespace nf::literals;\n'
        source.write_text(prefix + 'int main() { auto x = nf::message<"D">().set<11>("ok"); }\n')
        positive = subprocess.run(command, text=True, capture_output=True)
        if positive.returncode:
            raise RuntimeError("positive control failed:\n" + positive.stderr)
        generated = Path(temp) / "nimblefix/fix44_sample.hpp"
        subprocess.run([sys.executable, str(ROOT / "tools/schema/generate.py"),
            "--namespace", "nimblefix::fix44_sample", "--output", str(generated),
            str(ROOT / "schemas/fix44-sample.xml")], check=True)
        command.extend(["-I", temp])
        for name, (body, diagnostic) in (CASES | GENERATED_CASES | ENGINE_CASES).items():
            includes = '#include <nimblefix/fix44_sample.hpp>\n#include <nimblefix/engine.hpp>\n' if name in GENERATED_CASES else ""
            if name in ENGINE_CASES:
                includes = '#include <nimblefix/engine.hpp>\n'
            source.write_text(prefix + includes + "int main() { " + body + " }\n")
            result = subprocess.run(command, text=True, capture_output=True)
            if result.returncode == 0 or diagnostic not in result.stderr:
                raise RuntimeError(f"{name}: expected compile failure containing {diagnostic!r}\n{result.stderr}")
    print(f"{len(CASES) + len(GENERATED_CASES) + len(ENGINE_CASES)} compile-fail contracts passed ({args.cxx})")


if __name__ == "__main__":
    main()

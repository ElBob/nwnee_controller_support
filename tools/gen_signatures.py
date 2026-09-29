#!/usr/bin/env python3
"""Compile signatures/ee.yaml into a C header for the library (run by CMake).

Usage: tools/gen_signatures.py <ee.yaml> <out.h> [--release]

--release leaves out entries marked `debug: true` (control-socket surfaces).

Validates entry shape the same way at build time that sigcheck does offline, so
a malformed entry fails the build instead of silently disabling a feature.
"""
import re
import sys

try:
    import yaml
except ImportError:
    sys.exit("gen_signatures needs PyYAML: pip install pyyaml")

KEY_RE = re.compile(r"^[a-z][a-z0-9_]*$")


def c_str(v):
    if v is None:
        return "NULL"
    return '"' + str(v).replace("\\", "\\\\").replace('"', '\\"') + '"'


def load(path):
    doc = yaml.safe_load(open(path)) or {}
    sigs = doc.get("signatures") or {}
    errors = []
    for key, sig in sigs.items():
        if not KEY_RE.match(key):
            errors.append(f"{key}: key must be lower_snake_case")
        if not isinstance(sig, dict):
            errors.append(f"{key}: entry must be a mapping")
            continue
        if not sig.get("note"):
            errors.append(f"{key}: missing 'note' (re-notes reference)")
        if ("symbol" in sig) == ("pattern" in sig):
            errors.append(f"{key}: needs exactly one of 'symbol' or 'pattern'")
        if "symbol" in sig and ("offset" in sig or "expect" in sig):
            errors.append(f"{key}: 'offset'/'expect' apply to patterns only")
        if "debug" in sig and not isinstance(sig["debug"], bool):
            errors.append(f"{key}: 'debug' must be true or false")
    if errors:
        sys.exit("signatures/ee.yaml:\n  " + "\n  ".join(errors))
    return doc.get("binary_sha256"), sigs


def main():
    src, out = sys.argv[1], sys.argv[2]
    release = "--release" in sys.argv[3:]
    sha, sigs = load(src)
    if release:
        sigs = {k: v for k, v in sigs.items() if not v.get("debug")}
    lines = [
        "/* Generated from signatures/ee.yaml by tools/gen_signatures.py. Do not edit. */",
        "#ifndef NWPAD_SIGNATURES_GEN_H",
        "#define NWPAD_SIGNATURES_GEN_H",
        "",
        f"#define NWPAD_SIG_BINARY_SHA256 {c_str(sha)}",
        "",
        "enum {",
    ]
    lines += [f"    NWPAD_SIG_{key.upper()}," for key in sigs]
    lines += [
        "    NWPAD_SIG_COUNT",
        "};",
        "",
        "static const nwpad_sig_def nwpad_sig_defs[] = {",
    ]
    for key, sig in sigs.items():
        lines.append(
            f"    {{{c_str(key)}, {c_str(sig['note'])}, {c_str(sig.get('symbol'))}, "
            f"{c_str(sig.get('pattern'))}, {int(sig.get('offset', 0))}, {int(sig.get('expect', 1))}}},"
        )
    if not sigs:
        lines.append("    {NULL, NULL, NULL, NULL, 0, 0}, /* placeholder: no signatures yet */")
    lines += ["};", "", "#endif", ""]
    text = "\n".join(lines)
    try:
        if open(out).read() == text:
            return  # unchanged: don't touch the mtime
    except OSError:
        pass
    open(out, "w").write(text)


if __name__ == "__main__":
    main()

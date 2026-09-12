# -*- coding: utf-8 -*-
"""Regenerate pipl_data.bin for this plugin template.

Usage:
    python make_pipl.py NewPluginName
"""
import io
import sys
from pathlib import Path


def pstring(s):
    b = bytes([len(s)]) + s.encode("ascii")
    while len(b) % 4:
        b += b"\x00"
    return b


def cstring(s):
    b = s.encode("ascii") + b"\x00"
    while len(b) % 4:
        b += b"\x00"
    return b


def build_pipl(name):
    out = bytearray()
    out += b"\x01\x00"
    out += b"\x00\x00\x00\x00"
    out += b"\x05\x00\x00\x00"

    def prop(key, size, payload):
        nonlocal out
        out += b"MIB8" + key + b"\x00\x00\x00\x00"
        out += size.to_bytes(4, "little")
        out += payload

    prop(b"dnik", 4, b"xgEA")
    prop(b"eman", len(pstring(name)), pstring(name))
    prop(b"gtac", len(pstring("General Plugin")), pstring("General Plugin"))
    prop(b"srev", 4, b"\x00\x00\x01\x00")
    prop(b"4668", len(cstring("EntryPointFunc")), cstring("EntryPointFunc"))
    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: python make_pipl.py PluginName")
        sys.exit(1)
    data = build_pipl(sys.argv[1])
    output_path = Path(__file__).resolve().parent / 'DalimaoCurves' / 'pipl_data.bin'
    with io.open(output_path, "wb") as f:
        f.write(data)
    print("wrote {0} ({1} bytes) for {2}".format(output_path, len(data), sys.argv[1]))

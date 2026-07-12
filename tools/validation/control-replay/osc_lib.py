"""Minimal OSC 1.0 codec for the XOA control-replay harness.

Dependency-free (stdlib only): just enough of OSC to build /xoa messages and
parse the app's replies. Types supported: int32 'i', float32 'f', string 's' -
the only types XOA's router uses.
"""

import struct


def _pad(b: bytes) -> bytes:
    """Pad to the next 4-byte boundary (OSC alignment)."""
    return b + b"\x00" * ((4 - (len(b) % 4)) % 4)


def _pad_string(s: str) -> bytes:
    """OSC string: null-terminated, 4-byte padded."""
    return _pad(s.encode("utf-8") + b"\x00")


def encode(address: str, args=None) -> bytes:
    """Encode an OSC message. args is a list of (type, value) pairs where type
    is one of 'i', 'f', 's'."""
    args = args or []
    tags = "," + "".join(t for t, _ in args)
    out = _pad_string(address) + _pad_string(tags)
    for t, v in args:
        if t == "i":
            out += struct.pack(">i", int(v))
        elif t == "f":
            out += struct.pack(">f", float(v))
        elif t == "s":
            out += _pad_string(str(v))
        else:
            raise ValueError(f"unsupported OSC type {t!r}")
    return out


def decode(data: bytes):
    """Decode an OSC message -> (address, [values]). Raises on malformed input."""
    pos = 0

    def read_string():
        nonlocal pos
        end = data.index(b"\x00", pos)
        s = data[pos:end].decode("utf-8")
        pos = end + 1
        pos += (4 - (pos % 4)) % 4
        return s

    address = read_string()
    if pos >= len(data):
        return address, []
    tags = read_string()
    if not tags.startswith(","):
        return address, []

    values = []
    for t in tags[1:]:
        if t == "i":
            (v,) = struct.unpack_from(">i", data, pos)
            pos += 4
        elif t == "f":
            (v,) = struct.unpack_from(">f", data, pos)
            pos += 4
        elif t == "s":
            v = read_string()
        else:
            raise ValueError(f"unsupported OSC type tag {t!r}")
        values.append(v)
    return address, values

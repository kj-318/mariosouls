"""Export Dark Souls Remastered breakable map objects for Mario's attacks.

Reads the owner's installed MSBs (object part placement) and GameParam ObjectParam (hp > 0
means breakable). Output per map, next to the collision cache in
%LOCALAPPDATA%\\MarioSoulsProbe\\collision: <map_id>.brk = b"MSB1" + uint32 count + count
records of (float32[3] world position, int32 object model number, int16 hp, 2 pad bytes).

The game keeps each ObjIns's placement (the MSB position) in its matrix at +0x80; the host
matches live objects to these records by that position. No game-derived data goes into Git.

Standard library only.
"""

from pathlib import Path
import argparse
import struct
import sys
import zlib


def read_gameparam(path):
    data = path.read_bytes()
    raw = zlib.decompress(data[data.find(b"\x78", data.find(b"DCA\0")):])
    fmt, count = raw[0xC], struct.unpack_from("<I", raw, 0x10)[0]
    esz = 0x18 if fmt & 0x20 else 0x14
    params = {}
    for k in range(count):
        _, size, off, _, noff = struct.unpack_from("<5I", raw, 0x20 + k * esz)
        name = raw[noff:raw.index(b"\0", noff)].decode("latin1").split("\\")[-1]
        params[name] = raw[off:off + size]
    return params


def object_hp(params):
    b = params["ObjectParam.param"]
    rows = struct.unpack_from("<H", b, 0xA)[0]
    hp = {}
    for j in range(rows):
        rid, off, _ = struct.unpack_from("<iII", b, 0x30 + j * 12)
        hp[rid] = struct.unpack_from("<h", b, off)[0]
    return hp


def msb_lists(d):
    off, out = 0, {}
    while True:
        _, name_off, cnt = struct.unpack_from("<iii", d, off)
        offs = struct.unpack_from("<%di" % cnt, d, off + 12)
        out[d[name_off:d.index(b"\0", name_off)].decode()] = offs[:-1]
        if offs[-1] == 0:
            return out
        off = offs[-1]


def cstr(d, o):
    return d[o:d.index(b"\0", o)].decode("shift_jis", "replace")


def export_map(msb_path, hp, output):
    d = msb_path.read_bytes()
    lists = msb_lists(d)
    models = [cstr(d, o + struct.unpack_from("<i", d, o)[0]) for o in lists["MODEL_PARAM_ST"]]
    records = []
    for o in lists["PARTS_PARAM_ST"]:
        _, part_type, _, model_index = struct.unpack_from("<iiii", d, o)
        if part_type != 1 or not (0 <= model_index < len(models)):
            continue  # objects only
        model = models[model_index]
        if not model.startswith("o") or not model[1:5].isdigit():
            continue
        number = int(model[1:5])
        if hp.get(number, -1) <= 0:
            continue
        pos = struct.unpack_from("<3f", d, o + 0x14)
        records.append(struct.pack("<3fih2x", *pos, number, hp[number]))
    target = output / (msb_path.stem + ".brk")
    target.write_bytes(b"MSB1" + struct.pack("<I", len(records)) + b"".join(records))
    return len(records)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--game", required=True, help="DARK SOULS REMASTERED install directory")
    p.add_argument("--output", required=True)
    a = p.parse_args()
    game, output = Path(a.game), Path(a.output)
    output.mkdir(parents=True, exist_ok=True)
    hp = object_hp(read_gameparam(game / "param/GameParam/GameParam.parambnd.dcx"))
    for msb in sorted((game / "map/MapStudio").glob("m1*.msb")):
        print(f"{msb.stem}: {export_map(msb, hp, output)} breakable objects", flush=True)


if __name__ == "__main__":
    sys.exit(main())

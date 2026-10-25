"""Export Dark Souls Remastered static map collision for the SM64 guest.

Remastered counterpart of hosts/ds1ptde/prepare_collision.py. Reads the
owner's installed MSB (collision part placement), map Havok binders
(h*.hkxbhd/.hkxbdt) and EMEVD scripts. Collision parts that a script can switch
(EMEVD 2011[01]/[02], directly or through an argument) stay native-owned and
are not exported. No game-derived data goes into Git: write the output under
%LOCALAPPDATA%\\MarioSoulsProbe\\collision.

Output per map: <map_id>.bin = b"MSC2" + uint32 count + count records of
(uint32 hit filter, float32[3][3] triangle), world metres; manifest.json.

Needs Soulstruct with soulstruct-havok (Grimrukh) on PYTHONPATH.
"""

from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import sys

MAP_RE = re.compile(r"m\d{2}_\d{2}_\d{2}_\d{2}")


def controlled_collisions(events, possible_ids=None):
    ids = set()
    for event in events.values():
        for instruction in event.instructions:
            if instruction.category == 2011 and instruction.index in (1, 2):
                if any(r.arg_index == 0 for r in instruction.event_arg_replacements):
                    # A script argument can address any collision: keep them all native.
                    if possible_ids is not None:
                        ids.update(i for i in possible_ids if i >= 0)
                    continue
                ids.add(int(instruction.args_list[0]))
    return ids


def export_map(game, map_id, output, common_events):
    import numpy as np
    from soulstruct.containers import Binder
    from soulstruct.darksouls1r.maps import MSB
    from soulstruct.darksouls1r.events.emevd import EMEVD
    from soulstruct.utilities.maths import Matrix3
    from soulstruct.havok.fromsoft.darksouls1r import MapCollisionModel

    msb = MSB.from_path(game / "map/MapStudio" / f"{map_id}.msb")
    event_path = game / "event" / f"{map_id}.emevd.dcx"
    events = EMEVD.from_path(event_path).events if event_path.exists() else {}
    ids = [p.entity_id for p in msb.collisions]
    controlled = controlled_collisions(events, ids) | controlled_collisions(common_events, ids)
    stem = game / "map" / map_id / f"h{map_id[1:]}"
    binder = Binder.from_path(stem.with_suffix(".hkxbhd"), stem.with_suffix(".hkxbdt"))
    entries = {Path(e.name.replace("\\", "/")).name.lower(): e for e in binder.entries}
    triangles, filters, native, failed = [], [], [], []
    for part in msb.collisions:
        if part.entity_id in controlled:
            native.append(dict(name=part.name, entity_id=part.entity_id))
            continue
        name = part.model.name.lower() + f"a{map_id[1:3]}.hkx.dcx"
        if name not in entries:
            candidates = [n for n in entries if n.startswith(part.model.name.lower())]
            if len(candidates) != 1:
                failed.append(dict(name=part.name, reason="model-not-found"))
                continue
            name = candidates[0]
        try:
            model = MapCollisionModel.from_bytes(entries[name].data)
        except Exception as error:  # unknown shape: stays native-only, never guessed
            failed.append(dict(name=part.name, reason=type(error).__name__, detail=str(error)[:200]))
            continue
        rot = np.array(Matrix3.from_euler_angles_deg(part.rotate).data)
        offset = np.array(list(part.translate))
        for mesh in model.meshes:
            tri = (mesh.vertices3D @ rot.T + offset)[mesh.face_vertex_indices].astype("<f4")
            triangles.append(tri)
            filters.extend([int(part.hit_filter_id)] * len(tri))
    if not triangles:
        return dict(triangles=0, native_owned=native, failed=failed)
    data = np.concatenate(triangles)
    if not np.isfinite(data).all():
        raise ValueError(f"{map_id}: non-finite collision geometry")
    records = np.empty(len(data), dtype=[("filter", "<u4"), ("triangle", "<f4", (3, 3))])
    records["filter"] = filters
    records["triangle"] = data
    target = output / (map_id + ".bin")
    target.write_bytes(b"MSC2" + struct.pack("<I", len(data)) + records.tobytes())
    lo, hi = data.reshape(-1, 3).min(axis=0), data.reshape(-1, 3).max(axis=0)
    return dict(triangles=int(len(data)), bounds=[lo.tolist(), hi.tolist()], native_owned=native, failed=failed,
                sha256=hashlib.sha256(target.read_bytes()).hexdigest())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--game", required=True, help="DARK SOULS REMASTERED install directory")
    p.add_argument("--output", required=True)
    p.add_argument("--map", action="append", help="map ID (default: every m1x map)")
    a = p.parse_args()
    from soulstruct.darksouls1r.events.emevd import EMEVD

    game, output = Path(a.game), Path(a.output)
    output.mkdir(parents=True, exist_ok=True)
    maps = a.map or sorted(d.name for d in (game / "map").iterdir()
                           if d.is_dir() and MAP_RE.fullmatch(d.name) and d.name.startswith("m1"))
    common = EMEVD.from_path(game / "event" / "common.emevd.dcx").events
    manifest = dict(schema=1, policy="event-colliders-native", maps={})
    for map_id in maps:
        info = export_map(game, map_id, output, common)
        manifest["maps"][map_id] = info
        print(f"{map_id}: {info['triangles']} triangles, {len(info['native_owned'])} native-owned, "
              f"{len(info['failed'])} unsupported", flush=True)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=1))


if __name__ == "__main__":
    sys.exit(main())

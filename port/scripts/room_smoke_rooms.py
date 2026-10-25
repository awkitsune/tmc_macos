"""List the rooms the room smoke test warps into, from the game's room tables.

usage: room_smoke_rooms.py VALID_OUT SKIPPED_OUT

Reads data/map/{room,entity,map,tileset}_headers.s: gAreaRoomHeaders (one
header per room, ending at 0xFFFF), gAreaTable (room properties: entity lists
and callbacks), gAreaRoomMaps and gAreaTileSets. A room is valid when it has a
header with a tile set, non-NULL properties, a map entry, and a tile set entry
for its header's tileSet_id. Every other header is a leftover the game never
loads: the GBA would read its properties from the BIOS (NULL) or its map or
tile set from past the end of the area's table, so no exit leads there.

VALID_OUT gets "AA:RR" lines; SKIPPED_OUT gets "AA:RR reason" lines.
"""

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


def labels():
    """label -> list of the label's lines, up to the next label (lists can sit in another table's file)."""
    out = {}
    for path in ("room_headers.s", "entity_headers.s", "map_headers.s", "tileset_headers.s"):
        current = None
        for line in open(os.path.join(ROOT, "data", "map", path)):
            line = line.split("@")[0].strip()
            m = re.match(r"(\w+)::", line)
            if m:
                current = m.group(1)
                out[current] = []
            elif current and line:
                out[current].append(line)
    return out


SYMS = labels()


def words(lines):
    """The .4byte values of a pointer list."""
    return [l.split()[1] for l in lines if l.startswith(".4byte")]


def table(name):
    """area -> the area's list of .4byte entries (None for a NULL area)."""
    return [None if s == "0x0" else words(SYMS[s]) for s in words(SYMS[name])]


def main():
    syms = SYMS
    headers = []
    for sym in words(syms["gAreaRoomHeaders"]):
        rooms = []
        for line in syms.get(sym, []):
            if line.startswith(".2byte") and int(line.split()[1], 0) == 0xFFFF:
                break
            m = re.match(r"room_header\s+(.*)", line)
            if m:
                rooms.append(int(m.group(1).split(",")[4], 0))
        headers.append(rooms)
    props = table("gAreaTable")
    maps = table("gAreaRoomMaps")
    tilesets = table("gAreaTileSets")

    valid, skipped = [], []
    for area, rooms in enumerate(headers):
        for room, tileset in enumerate(rooms):
            spec = f"{area:02x}:{room:02x}"
            area_props = props[area] if area < len(props) else None
            area_maps = maps[area] if area < len(maps) else None
            area_tilesets = tilesets[area] if area < len(tilesets) else None
            if tileset == 0xFFFF:
                why = "no tile set in its header"
            elif area_props is None or room >= len(area_props) or area_props[room] == "0x0":
                why = "no room properties (gAreaTable entry NULL)"
            elif area_maps is None or room >= len(area_maps) or area_maps[room] == "0x0":
                why = "no map (gAreaRoomMaps entry NULL or past the end)"
            elif area_tilesets is None or tileset >= len(area_tilesets) or area_tilesets[tileset] == "0x0":
                why = f"tile set {tileset} past the end of gAreaTileSets"
            else:
                valid.append(spec)
                continue
            skipped.append(f"{spec} {why}")
    open(sys.argv[1], "w").write("".join(v + "\n" for v in valid))
    open(sys.argv[2], "w").write("".join(s + "\n" for s in skipped))
    print(f"rooms: {len(valid)} valid, {len(skipped)} skipped of {len(valid) + len(skipped)} headers")


if __name__ == "__main__":
    main()

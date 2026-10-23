"""Entity lists (EntityData) that only room data leads to, and a check of every list.

Native EntityData entries are 24 bytes (spritePtr is a 64-bit pointer), GBA
entries 16. A list carries over only if its label is typed, and many lists are
.incbin blobs without a single pointer in them, so no relocation shows their
type. Besides C declarations, hints and typed pointers (emit.py), lists are
reached through rooms:

- room property 0, 1 and 2 are entity lists (LoadRoom, sub_0804B058);
- some entities in a room load another property of that room as an entity
  list, the index being their type or type2 (CONSUMERS);
- lists that room code loads by name (src/roomInit.c) can hold such entities
  too; `^<regex> <room property table>` hints say which room they belong to.

check() then walks every EntityData label the way LoadRoomEntityList does and
fails the conversion if a native list would not end where the GBA one does.
"""

import re

GBA_ENTRY = 16
TERMINATOR = 0xFF
KIND_OBJECT, KIND_MANAGER = 6, 9

# (kind, id) -> the EntityData fields holding the room property index that the
# entity loads as an entity list.
CONSUMERS = {
    (KIND_MANAGER, 0x0B): ("type2",),  # FightManager_Init
    (KIND_MANAGER, 0x0D): ("type",),  # TileChangeObserveManager_Action2 (0: none)
    (KIND_MANAGER, 0x0E): ("type",),  # EntitySpawnManager
    (KIND_MANAGER, 0x28): ("type2",),  # EnemyInteractionManager_FindMatchingEntities
    (KIND_MANAGER, 0x38): ("type",),  # FloatingPlatformManager_Init
    (KIND_OBJECT, 0x89): ("type", "type2"),  # MazaalBossObject sub_0809AD68
}
ROOM_LIST_PROPERTIES = (0, 1, 2)


def gba_entries(label):
    """The 16-byte GBA entries within a label."""
    data = label.section.data
    return [data[o : o + GBA_ENTRY] for o in range(label.offset, label.end - GBA_ENTRY + 1, GBA_ENTRY)]


def list_shaped(label) -> bool:
    """Whole 16-byte entries of valid kinds, the last one the 0xFF terminator."""
    if label.size < GBA_ENTRY or label.size % GBA_ENTRY:
        return False
    entries = gba_entries(label)
    return entries[-1][0] == TERMINATOR and all(1 <= (e[0] & 0xF) <= 9 and e[0] != TERMINATOR for e in entries[:-1])


def consumed_properties(entry) -> list:
    """Room property indices that this entity loads as entity lists."""
    fields = CONSUMERS.get((entry[0] & 0xF, entry[2]))
    if not fields:
        return []
    out = []
    for f in fields:
        if f == "type":
            index = entry[3]
        else:
            # LoadRoomEntity leaves type2 alone for flags 0x1_
            index = 0 if (entry[1] & 0xF0) == 0x10 else entry[4]
        if index == 0 and entry[2] == 0x0D and entry[0] & 0xF == KIND_MANAGER:
            continue
        out.append(index)
    return out


class RoomLists:
    def __init__(self, model, list_type, room_hints):
        self.model = model
        self.list_type = list_type
        self.room_hints = room_hints  # [(regex, room property table label)]
        self.rooms = self._rooms()

    def _pointers(self, info):
        sec = info.label.section
        out = []
        for off in range(info.label.offset, info.label.end, 4):
            reloc = sec.relocs.get(off)
            target = self.model._resolve(sec.obj, reloc) if reloc is not None else None
            out.append(target[0] if target is not None and target[1] == 0 else None)
        return out

    def _rooms(self) -> dict:
        """Room property table label -> its properties (LabelInfo or None), via gAreaTable."""
        rooms = {}
        area_table = self.model.globals.get("gAreaTable")
        if area_table is None:
            return rooms
        for area in self._pointers(area_table):
            if area is None:
                continue
            for room in self._pointers(area):
                if room is not None:
                    rooms[room.label.name] = self._pointers(room)
        return rooms

    def derive(self):
        """Type the room properties that room lists' entities load as entity lists."""
        model = self.model
        # Lists at any property index belong to that room (room code loads some by name).
        work = [
            (p, name)
            for name, props in self.rooms.items()
            for i, p in enumerate(props)
            if p and (i in ROOM_LIST_PROPERTIES or (p.mode == "typed" and p.type == self.list_type))
        ]
        for pattern, room in self.room_hints:
            if room not in self.rooms:
                raise SystemExit(f"hints.txt: ^{pattern.pattern}: no room property table {room}")
            matched = [info for info in model.labels.values() if pattern.fullmatch(info.label.name)]
            if not matched:
                raise SystemExit(f"hints.txt: ^{pattern.pattern}: no such label")
            work += [(info, room) for info in matched]
        seen = set()
        self.walked = set()  # entity lists whose room is known
        derived = []
        while work:
            info, room = work.pop()
            if (info.label.name, room) in seen:
                continue
            seen.add((info.label.name, room))
            if info.mode == "raw" and list_shaped(info.label):
                model._assign(info, self.list_type)
            if info.mode != "typed" or info.type != self.list_type:
                continue  # reported by check() if something loads it as a list
            self.walked.add(info.label.name)
            for entry in self._walk(info.label)[0]:
                for index in consumed_properties(entry):
                    props = self.rooms[room]
                    target = props[index] if index < len(props) else None
                    if target is None:
                        continue  # GetCurrentRoomProperty gives NULL; the consumers check it
                    if target.mode == "raw" and not list_shaped(target.label):
                        model._problem(target, f"loaded as an entity list (property {index} of {room}) but not shaped like one")
                        continue
                    if target.mode == "raw":
                        derived.append(target.label.name)
                    elif target.mode != "typed" or target.type != self.list_type:
                        model._problem(target, f"loaded as an entity list (property {index} of {room}) but typed {target.type}")
                        continue
                    work.append((target, room))
        return derived

    def _walk(self, label):
        """GBA entries from label to the terminator: (entries, labels walked through)."""
        sec = label.section
        entries, labels = [], [label]
        off = label.offset
        while off + GBA_ENTRY <= len(sec.data):
            entry = sec.data[off : off + GBA_ENTRY]
            if entry[0] == TERMINATOR:
                break
            entries.append(entry)
            off += GBA_ENTRY
            if off >= labels[-1].end and off < len(sec.data):
                nxt = sec.label_at(off)
                labels.append(nxt if nxt is not None and nxt.offset == off else None)
                if labels[-1] is None:
                    break
        return entries, labels

    def check(self) -> list:
        """Errors for EntityData labels whose native list would not end at the GBA terminator,
        and warnings for list-shaped room properties that nothing types."""
        model = self.model
        errors = []
        for info in model.labels.values():
            if info.mode != "typed" or info.type != self.list_type:
                continue
            label = info.label
            if info.tail:
                errors.append(f"{label.name}: {info.tail} bytes after its {info.count} entries stay in GBA layout")
                continue
            # LoadRoomEntityList steps 24 bytes at a time from here; it must meet the
            # terminator in converted entries, crossing only into converted labels.
            entries, labels = self._walk(label)
            for walked in labels[1:]:
                if walked is None:
                    errors.append(f"{label.name}: list runs on past {len(entries)} entries into unlabelled bytes")
                    break
                nxt = model.labels[(walked.section.obj.rel, walked.name)]
                if nxt.mode != "typed" or nxt.type != self.list_type or nxt.tail:
                    errors.append(
                        f"{label.name}: list runs on past {len(entries)} entries into {walked.name}, which is not converted"
                        " (if it is a single entry, not a list, give the next label a type)"
                    )
                    break
            if label.name not in self.walked and any(consumed_properties(e) for e in entries):
                errors.append(f"{label.name}: holds entities that load room properties as entity lists, but no room is known"
                              " for it (add a ^ hint)")
        for name, props in self.rooms.items():
            for i, p in enumerate(props):
                if i >= 8 and p is not None and p.mode == "raw" and list_shaped(p.label):
                    model.warnings.append(f"{p.label.name}: property {i} of {name} looks like an entity list but nothing loads it as one")
        return errors


def load_room_hints(lines) -> list:
    out = []
    for line in lines:
        line = line.split("#", 1)[0].strip()
        if line.startswith("^"):
            pattern, room = line[1:].split()
            out.append((re.compile(pattern), room))
    return out

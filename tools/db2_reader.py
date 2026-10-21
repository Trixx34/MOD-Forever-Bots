"""A small reader for the client's DB2 files (format WDC5): whole tables of numbers and text.

A copy of the forever-dashboard reader (same author) with three additions for tools/gen_avoid_spells.py: skip_encrypted=True leaves the encrypted sections out
(the client's SpellEffect.db2 has some), array columns are read as lists, and read_table.parents holds the relationship table (record id -> the foreign id, the
SpellID of SpellEffect and SpellMisc rows).

It follows the core's own loader (src/common/DataStores/DB2FileLoader.cpp). Not supported, and refused with a Db2Error: sparse
tables (variable-length records), array columns of anything but 32-bit values (returned as a list), encrypted sections. Used by the offline tools only.
"""
from __future__ import annotations

import struct

HEADER = struct.Struct("<II128s9IHh7I")        # DB2Header
SECTION = struct.Struct("<QIIIIIIII")          # DB2SectionHeader
COL_NONE, COL_IMMEDIATE, COL_COMMON, COL_PALLET, COL_PALLET_ARRAY, COL_SIGNED = range(6)


class Db2Error(Exception):
    pass


def as_float(value: int) -> float:
    return struct.unpack("<f", struct.pack("<I", value & 0xFFFFFFFF))[0]


def read_table(path: str, strings: tuple[int, ...] = (), skip_encrypted: bool = False) -> dict[int, list]:
    """Every record as {record id: [one value per field]}. Fields listed in `strings` come back as text; the others as unsigned
    integers (use as_float for float fields; signed fields wrap, see signed())."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HEADER.size:
        raise Db2Error(f"{path}: file too short")
    h = HEADER.unpack_from(data, 0)
    if h[0] != 0x35434457 or h[1] != 5:
        raise Db2Error(f"{path}: not a WDC5 file")
    field_count, record_size, flags, index_field = h[4], h[5], h[12], h[13]
    total_fields, packed_offset, colmeta_size, section_count = h[14], h[15], h[17], h[20]
    if flags & 1:
        raise Db2Error(f"{path}: sparse tables are not supported")
    if colmeta_size != 24 * total_fields:
        raise Db2Error(f"{path}: unexpected column metadata size")
    pos = HEADER.size
    sections = []
    for _ in range(section_count):
        sections.append(SECTION.unpack_from(data, pos))
        pos += SECTION.size
    if any(sec[0] for sec in sections) and not skip_encrypted:
        raise Db2Error(f"{path}: encrypted section")
    pos += 4 * field_count

    columns = []
    for _ in range(total_fields):
        bit_offset, bit_size, extra, comp = struct.unpack_from("<HHII", data, pos)
        a, b, c = struct.unpack_from("<III", data, pos + 12)
        columns.append({"bit_offset": bit_offset, "bit_size": bit_size, "extra": extra, "comp": comp, "a": a, "b": b, "c": c})
        pos += 24
    for kind in (COL_PALLET, COL_PALLET_ARRAY):
        for col in columns:
            if col["comp"] == kind:
                col["pallet"] = list(struct.unpack_from(f"<{col['extra'] // 4}I", data, pos))
                pos += col["extra"]
    for col in columns:
        if col["comp"] == COL_COMMON:
            col["common"] = dict(struct.unpack_from("<II", data, pos + 8 * i) for i in range(col["extra"] // 8))
            pos += col["extra"]
        if col["comp"] == COL_NONE and col["bit_size"] not in (8, 16, 32, 64) and col["bit_size"] % 32:
            raise Db2Error(f"{path}: odd-sized column")

    # The strings of all sections form one table after all the records; a string field holds an offset counted from the field's own
    # position in the records laid end to end (this is how the core's loader reads it).
    total_records = sum(sec[2] for sec in sections) * record_size
    tables, table_at = [], 0
    for sec in sections:
        start = sec[1] + record_size * sec[2]
        tables.append(data[start:start + sec[3]])
    string_table = b"".join(tables)

    def text(merged_target: int) -> str:
        at = merged_target - total_records
        if not 0 <= at < len(string_table):
            return ""
        return string_table[at:string_table.index(b"\0", at)].decode("utf-8", "replace")

    rows: dict[int, list] = {}
    skipped_sections = [0]
    parents: dict[int, int] = {}
    rec_ids: list[int] = []
    copies: list[tuple[int, int]] = []
    merged_base = 0
    for tact, file_offset, rec_count, str_size, _cat, id_table_size, parent_size, _catcount, copy_count in sections:
        if tact:
            merged_base += rec_count * record_size
            skipped_sections[0] += rec_count
            continue
        ids_at = file_offset + record_size * rec_count + str_size
        if ids_at + id_table_size + 8 * copy_count > len(data):
            raise Db2Error(f"{path}: truncated")
        ids = list(struct.unpack_from(f"<{id_table_size // 4}I", data, ids_at)) if id_table_size else []
        for i in range(rec_count):
            rec = file_offset + i * record_size
            values: list = []
            commons: list[int] = []
            for fi, col in enumerate(columns):
                comp = col["comp"]
                if comp == COL_NONE:
                    off = rec + col["bit_offset"] // 8
                    if col["bit_size"] in (8, 16, 32, 64):
                        v = int.from_bytes(data[off:off + col["bit_size"] // 8], "little")
                        if fi in strings:
                            v = text(merged_base + i * record_size + col["bit_offset"] // 8 + v) if v else ""
                    else:
                        v = list(struct.unpack_from(f"<{col['bit_size'] // 32}I", data, off))
                    values.append(v)
                elif comp == COL_COMMON:
                    values.append(None)
                    commons.append(fi)
                else:
                    bit_offset, width = col["a"], col["b"]
                    off = rec + packed_offset + bit_offset // 8
                    raw = int.from_bytes(data[off:off + 8].ljust(8, b"\0"), "little")
                    v = (raw >> (bit_offset & 7)) & ((1 << width) - 1)
                    if comp == COL_SIGNED:
                        mask = 1 << (width - 1)
                        v = (v ^ mask) - mask
                    elif comp == COL_PALLET:
                        v = col["pallet"][v]
                    elif comp == COL_PALLET_ARRAY:
                        n = col["c"]
                        v = col["pallet"][v * n:(v + 1) * n]
                    values.append(v)
            if ids:
                rid = ids[i]
            elif 0 <= index_field < len(values) and values[index_field] is not None:
                rid = values[index_field]
            else:
                raise Db2Error(f"{path}: cannot find a record id")
            for fi in commons:
                values[fi] = columns[fi]["common"].get(rid, columns[fi]["a"])
            rows[rid] = values
            rec_ids.append(rid)
        at = ids_at + id_table_size
        for j in range(copy_count):
            copies.append(struct.unpack_from("<II", data, at + 8 * j))
        if parent_size:
            rel = at + 8 * copy_count
            n, _mn, _mx = struct.unpack_from("<III", data, rel)
            base_idx = len(rec_ids) - rec_count
            for j in range(n):
                foreign, rindex = struct.unpack_from("<II", data, rel + 12 + 8 * j)
                if rindex < rec_count:
                    parents[rec_ids[base_idx + rindex]] = foreign
        merged_base += rec_count * record_size
    for new_id, src_id in copies:
        if src_id in rows:
            rows[new_id] = list(rows[src_id])
    read_table.parents = parents
    read_table.skipped_records = skipped_sections[0]
    return rows


def signed(value: int, bits: int = 32) -> int:
    return value - (1 << bits) if value >= 1 << (bits - 1) else value

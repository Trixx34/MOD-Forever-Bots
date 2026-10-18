#!/usr/bin/env python3
"""Reads some tables out of a mysqldump file (.sql or .sql.gz) into a SQLite file, so the queries in doc/*.md can be run without a MySQL server.

    python3 -I tools/dump_to_sqlite.py forever_test_world.sql.gz world.sqlite creature creature_template quest_template ...

Only the tables named are read. Column types are not kept (SQLite does not need them); numbers stay numbers and strings stay strings. Backticks around
names work in SQLite, so `Order` and the like are written the same way as in MySQL. Not a full SQL parser: it reads the `CREATE TABLE` column lines and the
`INSERT INTO ... VALUES` lines that mysqldump writes.
"""
import gzip
import re
import sqlite3
import sys

ESCAPES = {"'": "''", "\\": "\\", "n": "\n", "r": "\r", "t": "\t", "0": "", "Z": "", '"': '"', "b": "\b"}


def unescape(match):
    return ESCAPES.get(match.group(1), match.group(1))


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    dump, out, tables = argv[1], argv[2], set(argv[3:])
    opener = gzip.open if dump.endswith(".gz") else open
    db = sqlite3.connect(out)
    db.execute("PRAGMA journal_mode = OFF")
    db.execute("PRAGMA synchronous = OFF")
    create = re.compile(r"^CREATE TABLE `(\w+)`")
    insert = re.compile(r"^INSERT INTO `(\w+)` VALUES ")
    escape = re.compile(r"\\(.)", re.S)
    current = None
    columns = []
    rows = {}
    failed = {}
    with opener(dump, "rt", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("CREATE TABLE"):
                m = create.match(line)
                current = m.group(1) if m and m.group(1) in tables else None
                columns = []
                continue
            if current is not None:
                if line.startswith("  `"):
                    columns.append(line.split("`")[1])
                elif line.startswith(")"):
                    db.execute("DROP TABLE IF EXISTS `%s`" % current)
                    db.execute("CREATE TABLE `%s` (%s)" % (current, ", ".join("`%s`" % c for c in columns)))
                    current = None
                continue
            if not line.startswith("INSERT INTO"):
                continue
            m = insert.match(line)
            if not m or m.group(1) not in tables:
                continue
            table = m.group(1)
            body = escape.sub(unescape, line[m.end():].rstrip().rstrip(";"))
            try:
                db.execute("INSERT INTO `%s` VALUES %s" % (table, body))
                rows[table] = rows.get(table, 0) + 1
            except sqlite3.Error as error:
                failed[table] = failed.get(table, 0) + 1
                if failed[table] <= 2:
                    print("%s: an INSERT line was skipped: %s" % (table, error), file=sys.stderr)
    db.commit()
    for table in sorted(tables):
        count = db.execute("SELECT COUNT(*) FROM `%s`" % table).fetchone()[0] if table in {r[0] for r in db.execute("SELECT name FROM sqlite_master")} else None
        print("%-34s %s rows (%d INSERT lines, %d skipped)" % (table, count, rows.get(table, 0), failed.get(table, 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

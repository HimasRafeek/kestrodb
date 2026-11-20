"""python test_embedded.py DIRECTORY

Embedded mode: this process owns the database through the C library."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kestrodb  # noqa: E402

path = os.path.join(sys.argv[1], "embedded.kdb")


def check(ok, what):
    if not ok:
        print("FAIL", what, file=sys.stderr)
        sys.exit(1)


with kestrodb.open(path, create=True) as db:
    for i in range(500):
        db.set("e", f"k{i}", {"i": i, "text": f"héllo ✓ {i}"})
    check(db.get("e", "k7")["value"] == {"i": 7, "text": "héllo ✓ 7"}, "get")
    check(db.get("e", "missing") is None, "missing")
    v = db.batch([{"type": "set", "collection": "e", "key": "a", "value": 1}, {"type": "delete", "collection": "e", "key": "k0"}])
    many = db.get_many("e", ["a", "k0", "k1"])
    check(many[0]["version"] == v and many[1] is None and many[2]["value"]["i"] == 1, "batch/get_many")
    try:
        db.set("e", "a", 2, expected_version=v - 1)
        check(False, "stale expected_version accepted")
    except kestrodb.KestroDBError as e:
        check(e.kind == "CONFLICT", "conflict kind")
    try:
        kestrodb.open(path)
        check(False, "second owner accepted")
    except kestrodb.KestroDBError as e:
        check(e.kind == "LOCKED", "locked kind " + e.kind)
    check(db.count() == 500, "count")
    db.sql("CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT UNIQUE, age INT)")
    for i in range(10):
        db.sql("INSERT INTO users (email, age) VALUES (?, ?)", [f"u{i}@x", 20 + i])
    check(db.sql("SELECT email FROM users WHERE age >= ? ORDER BY age DESC LIMIT 2", [27])["rows"] == [{"email": "u9@x"}, {"email": "u8@x"}], "embedded sql")
with kestrodb.open(path) as db:
    check(db.get("e", "a")["value"] == 1 and db.sql("SELECT COUNT(*) AS n FROM users")["rows"][0]["n"] == 10, "reopen")
print("ok python embedded")

"""python test_kestrodb.py ADDRESS TOKEN WORKER_ID COUNT [DB_PATH]

DB_PATH: read in-process (read_from) while writing through the server.

Exercises the Python client against a running kestro-server. Run several at once to test
concurrent connections; exits non-zero on any failure."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kestrodb  # noqa: E402

address, token, worker, count = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
read_from = sys.argv[5] if len(sys.argv) > 5 else None


def fail(what):
    print("FAIL", what, file=sys.stderr)
    sys.exit(1)


with kestrodb.connect(address, token=token, read_from=read_from) as db:
    if read_from:
        v = db.set("py", f"fresh-{worker}", "x")
        if db.get("py", f"fresh-{worker}")["version"] != v:
            fail("in-process read missed an acknowledged write")
        db.delete("py", f"fresh-{worker}")
        if db.get("py", f"fresh-{worker}") is not None:
            fail("in-process read missed an acknowledged delete")
    for i in range(count):
        db.set("py", f"w{worker}-{i}", {"worker": int(worker), "i": i, "text": f"héllo ✓ {i}"})
    for i in range(count):
        r = db.get("py", f"w{worker}-{i}")
        if r is None or r["value"]["i"] != i or r["value"]["text"] != f"héllo ✓ {i}":
            fail(f"read back {i}")
    many = db.get_many("py", [f"w{worker}-0", "missing", f"w{worker}-0"])
    if many[1] is not None or many[0]["value"]["i"] != 0 or many[2]["version"] != many[0]["version"]:
        fail("get_many")

    key = f"cas-{worker}"
    v = db.set("py", key, 1, expected_version=0)
    db.set("py", key, 2, expected_version=v)
    try:
        db.set("py", key, 3, expected_version=v)
        fail("stale expected_version accepted")
    except kestrodb.KestroDBError as e:
        if e.kind != "CONFLICT":
            fail("conflict kind " + e.kind)
    if db.get("py", key)["value"] != 2:
        fail("cas value")

    version = db.batch([
        {"type": "set", "collection": "py", "key": f"b{worker}-1", "value": [1, 2, 3]},
        {"type": "set", "collection": "py", "key": f"b{worker}-2", "value": {"nested": {"ok": True}}},
    ])
    if db.get("py", f"b{worker}-1")["version"] != version or db.get("py", f"b{worker}-2")["version"] != version:
        fail("batch version")
    db.delete("py", f"b{worker}-1")
    if db.get("py", f"b{worker}-1") is not None:
        fail("delete")

    for c, k in [("bad name", "k"), ("py", ""), ("py", "a\0b")]:
        try:
            db.set(c, k, 1)
            fail(f"accepted {c}/{k}")
        except kestrodb.KestroDBError as e:
            if e.kind != "INPUT":
                fail("input kind")
    if not db.stats()["server"]:
        fail("stats")

    # SQL from several processes at once: auto ids, unique constraint, parameters.
    db.sql("CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, sku TEXT UNIQUE, qty INT)")
    for i in range(20):
        db.sql("INSERT INTO items (sku, qty) VALUES (?, ?)", [f"py-{worker}-{i}", i])
    r = db.sql("SELECT sku, qty FROM items WHERE sku = ?", [f"py-{worker}-7"])
    if r["rows"] != [{"sku": f"py-{worker}-7", "qty": 7}]:
        fail("sql select " + str(r))
    try:
        db.sql("INSERT INTO items (sku) VALUES (?)", [f"py-{worker}-7"])
        fail("sql unique accepted")
    except kestrodb.KestroDBError as e:
        if e.kind != "CONFLICT":
            fail("sql unique kind " + e.kind)
    if db.sql("UPDATE items SET qty = qty + 100 WHERE sku LIKE ?", [f"py-{worker}-%"])["changes"] != 20:
        fail("sql update")
print(f"ok python worker {worker}")

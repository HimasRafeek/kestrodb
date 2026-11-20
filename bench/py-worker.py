"""python py-worker.py ENGINE TARGET TOKEN MODE WORKER COUNT KEYS

ENGINE  kestrodb  (TARGET = server address, or "address|database path" for in-process reads)
        | embedded (TARGET = database path; this process owns it)
        | sqlite (TARGET = database file; WAL + synchronous=FULL)
MODE    write: COUNT durable single writes | read: COUNT random point reads of KEYS keys
        | seed: write KEYS keys in batches of 1000
Prints {"start", "end", "ops"} measured around the work loop only."""
import json
import os
import random
import sqlite3
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "clients", "python"))
import kestrodb  # noqa: E402

engine, target, token, mode, worker, count, keys = sys.argv[1:8]
count, keys = int(count), int(keys)


def doc(i):
    return {"id": i, "name": f"user-{i}", "email": f"user{i}@example.com", "tags": ["a", "b", "c"], "score": i * 7 % 1000, "bio": "x" * 120}


if engine in ("kestrodb", "embedded"):
    if engine == "embedded":
        db = kestrodb.open(target, create=not os.path.exists(target))
    else:
        address, _, path = target.partition("|")
        db = kestrodb.connect(address, token=None if token == "-" else token, read_from=path or None)

    def put(k, v):
        db.set("c", k, v)

    def get(k):
        return db.get("c", k)["value"]

    def seed(a, b):
        db.batch([{"type": "set", "collection": "c", "key": f"k{i}", "value": doc(i)} for i in range(a, b)])
else:
    con = sqlite3.connect(target, timeout=30, isolation_level=None)
    con.execute("PRAGMA journal_mode=WAL")
    con.execute("PRAGMA synchronous=FULL")
    con.execute("CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT NOT NULL, version INTEGER NOT NULL) WITHOUT ROWID")
    UPSERT = "INSERT INTO kv (k, v, version) VALUES (?, ?, 1) ON CONFLICT(k) DO UPDATE SET v = excluded.v, version = version + 1"

    def put(k, v):
        con.execute(UPSERT, (k, json.dumps(v)))

    def get(k):
        return json.loads(con.execute("SELECT v FROM kv WHERE k = ?", (k,)).fetchone()[0])

    def seed(a, b):
        con.execute("BEGIN")
        con.executemany(UPSERT, [(f"k{i}", json.dumps(doc(i))) for i in range(a, b)])
        con.execute("COMMIT")

start = time.time()
if mode == "seed":
    for i in range(0, keys, 1000):
        seed(i, min(i + 1000, keys))
    ops = keys
elif mode == "write":
    for i in range(count):
        put(f"w{worker}-{i}", doc(i))
    ops = count
else:
    rng = random.Random(int(worker))
    for _ in range(count):
        n = rng.randrange(keys)
        if get(f"k{n}")["id"] != n:
            sys.exit("bad read")
    ops = count
print(json.dumps({"start": start, "end": time.time(), "ops": ops}))

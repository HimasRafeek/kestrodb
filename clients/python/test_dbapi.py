"""python test_dbapi.py ADDRESS TOKEN [EMBEDDED_PATH]

The SQL API and DB-API 2.0 (PEP 249) against a running kestro-server and, with a path, an
embedded database. Exits non-zero on the first failure."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kestrodb  # noqa: E402


def fail(what):
    print("FAIL", what, file=sys.stderr)
    sys.exit(1)


def eq(a, b, what):
    if a != b:
        fail("%s: got %r expected %r" % (what, a, b))


def check(db, name):
    db.execute("CREATE TABLE IF NOT EXISTS people (id INTEGER PRIMARY KEY, email TEXT UNIQUE, name TEXT, age INTEGER)")
    db.execute("DELETE FROM people")
    # Named, positional and numbered parameters; last_insert_id.
    eq(db.execute("INSERT INTO people (email, name, age) VALUES (:email, :name, :age)", {"email": "a@x.org", ":name": "Ann", "age": 31}), 1, name + " insert named")
    a = db.last_insert_id
    db.execute("INSERT INTO people (email, name, age) VALUES (?, ?, ?)", ("b@x.org", "Bob", 25))
    db.execute("INSERT INTO people (email, name, age) VALUES ($2, $1, $3)", ["Cy", "c@x.org", 41])
    eq(db.query_value("SELECT name FROM people WHERE id = ?", [a]), "Ann", name + " last_insert_id")
    eq([r["name"] for r in db.query("SELECT name FROM people WHERE age > :min ORDER BY age", {"min": 26})], ["Ann", "Cy"], name + " query")
    eq(db.query_one("SELECT name, age FROM people WHERE email = @e", {"e": "b@x.org"}), {"name": "Bob", "age": 25}, name + " query_one")
    eq(db.query_one("SELECT * FROM people WHERE id = ?", [999]), None, name + " query_one none")
    db.execute("INSERT INTO people (email, name, age) VALUES (?, ?, ?)", ["d@x.org", "Dévi ✓ 日本", None])
    eq(db.query_one("SELECT name, age FROM people WHERE email = ?", ["d@x.org"]), {"name": "Dévi ✓ 日本", "age": None}, name + " unicode/null")
    # PEP 249 exceptions.
    for sql, params, cls in [("INSERT INTO people (email) VALUES (?)", ["a@x.org"], kestrodb.IntegrityError),
                             ("SELEC nonsense", [], kestrodb.ProgrammingError),
                             ("SELECT * FROM people WHERE id = :id", {}, kestrodb.ProgrammingError),
                             ("SELECT * FROM people WHERE id = :id AND age = ?", [1, 2], kestrodb.ProgrammingError)]:
        try:
            db.execute(sql, params)
            fail(name + " accepted " + sql)
        except cls as e:
            if not isinstance(e, kestrodb.KestroDBError) or not isinstance(e, kestrodb.DatabaseError):
                fail(name + " hierarchy")
    # Cursor.
    cur = db.cursor()
    cur.execute("SELECT name, age FROM people WHERE age >= ? ORDER BY age", [30])
    eq([d[0] for d in cur.description], ["name", "age"], name + " description")
    eq(cur.rowcount, 2, name + " rowcount select")
    eq(cur.fetchone(), ("Ann", 31), name + " fetchone")
    eq(cur.fetchall(), [("Cy", 41)], name + " fetchall")
    eq(cur.fetchone(), None, name + " fetchone end")
    cur.execute("INSERT INTO people (email, name, age) VALUES (?, ?, ?)", ["e@x.org", "Eve", 22])
    eq(cur.rowcount, 1, name + " rowcount insert")
    eq(db.query_value("SELECT email FROM people WHERE id = ?", [cur.lastrowid]), "e@x.org", name + " lastrowid")
    cur.executemany("INSERT INTO people (email, name, age) VALUES (?, ?, ?)", [("f%d@x.org" % i, "F", i) for i in range(10)])
    eq(cur.rowcount, 10, name + " executemany")
    eq(db.query_value("SELECT COUNT(*) FROM people"), 15, name + " count after executemany")
    try:
        cur.executemany("INSERT INTO people (email, name) VALUES (?, ?)", [("g@x.org", "G"), ("a@x.org", "dup")])
        fail(name + " executemany duplicate accepted")
    except kestrodb.IntegrityError:
        pass
    eq(db.query_value("SELECT COUNT(*) FROM people WHERE email = ?", ["g@x.org"]), 0, name + " executemany is atomic")
    cur.execute("SELECT id FROM people ORDER BY id")
    eq(len(list(cur)), 15, name + " iterate")
    eq(cur.fetchmany(3), [], name + " fetchmany end")
    cur.close()
    # Transactions: context manager, explicit, rollback on exception.
    with db.transaction():
        db.execute("UPDATE people SET age = 50 WHERE email = ?", ["b@x.org"])
    eq(db.query_value("SELECT age FROM people WHERE email = ?", ["b@x.org"]), 50, name + " transaction commit")
    db.begin()
    eq(db.in_transaction, True, name + " in_transaction")
    db.execute("DELETE FROM people")
    eq(db.query_value("SELECT COUNT(*) FROM people"), 0, name + " read your writes")
    db.rollback()
    eq(db.query_value("SELECT COUNT(*) FROM people"), 15, name + " rollback")
    try:
        with db.transaction():
            db.execute("DELETE FROM people")
            raise RuntimeError("boom")
    except RuntimeError:
        pass
    eq(db.in_transaction, False, name + " no transaction after rollback")
    eq(db.query_value("SELECT COUNT(*) FROM people"), 15, name + " rolled back on exception")
    db.commit()  # nothing open: a no-op, as PEP 249 expects
    # PEP 249 mode: the first statement opens a transaction that commit() ends.
    db.autocommit = False
    c2 = db.cursor()
    c2.execute("DELETE FROM people WHERE age < ?", [10])
    eq(db.in_transaction, True, name + " implicit begin")
    db.rollback()
    eq(db.query_value("SELECT COUNT(*) FROM people"), 15, name + " implicit rollback")
    db.rollback()  # leave the read's implicit transaction
    c2.execute("DELETE FROM people WHERE age < ?", [10])
    db.commit()
    db.autocommit = True
    eq(db.query_value("SELECT COUNT(*) FROM people"), 5, name + " implicit commit")
    # Key/value next to SQL.
    v = db.set("kv", "k1", {"n": 1})
    eq(db.get("kv", "k1"), {"version": v, "value": {"n": 1}}, name + " kv")
    try:
        db.set("kv", "k1", 2, expected_version=0)
        fail(name + " cas accepted")
    except kestrodb.IntegrityError:
        pass
    db.delete("kv", "k1")
    eq(db.get("kv", "k1"), None, name + " delete")


address, token = sys.argv[1], sys.argv[2]
db = kestrodb.connect(address, token=token)
check(db, "server")
try:
    kestrodb.connect("tcp://127.0.0.1:1", timeout=1.0)
    fail("connect to a closed port")
except (OSError, kestrodb.KestroDBError):
    pass
try:
    kestrodb.connect(address, token="wrong-token-0123456789")
    fail("wrong token accepted")
except kestrodb.OperationalError as e:
    eq(e.kind, "AUTH", "auth kind")
c = db.compact()
eq(c["after"] <= c["before"], True, "server compact")
eq(db.query_value("SELECT COUNT(*) FROM people"), 5, "rows after compact")
db.close()
try:
    db.query("SELECT COUNT(*) FROM people")
    fail("closed connection used")
except kestrodb.InterfaceError:
    pass
if len(sys.argv) > 3:
    e = kestrodb.open(sys.argv[3], create=True)
    check(e, "embedded")
    e.close()
    e2 = kestrodb.open(sys.argv[3])
    eq(e2.query_value("SELECT COUNT(*) FROM people"), 5, "embedded reopen")
    d = os.path.dirname(sys.argv[3])
    eq(e2.compact()["after"] > 0, True, "embedded compact")
    tx1 = e2.archive(os.path.join(d, "py-archive"))
    e2.execute("DELETE FROM people")
    tx2 = e2.archive(os.path.join(d, "py-archive"))
    eq(tx2 > tx1, True, "archive advanced")
    e2.backup(os.path.join(d, "py-backup.kdb"))
    e2.close()
    eq(kestrodb.restore(os.path.join(d, "py-archive", "archive.kdb"), os.path.join(d, "py-restored.kdb"), txid=tx1), tx1, "restore txid")
    r = kestrodb.open(os.path.join(d, "py-restored.kdb"))
    eq(r.query_value("SELECT COUNT(*) FROM people"), 5, "restored before the delete")
    r.close()
    b = kestrodb.open(os.path.join(d, "py-backup.kdb"))
    eq(b.query_value("SELECT COUNT(*) FROM people"), 0, "backup after the delete")
    b.close()
print("ok python dbapi")

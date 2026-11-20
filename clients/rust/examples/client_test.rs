//! The Rust client against a real kestro-server; tests/rust-client.test.mjs starts the
//! servers and runs this. Prints `ok rust MODE (N checks)`, or `FAIL ...` and exits 1.
//!
//! ```text
//! client_test api ADDRESS TOKEN
//! client_test reconnect ADDRESS TOKEN        survives server restarts (waits for lines on stdin)
//! client_test busy ADDRESS TOKEN MAX         the server was started with --max-connections MAX
//! client_test tls ADDRESS TOKEN CA WRONG_CA  ADDRESS is tls://localhost:PORT (feature tls)
//! client_test cluster LIST TOKEN FOLLOWER    follows the leader; waits for it to be killed
//! client_test cluster-new LIST TOKEN         a new client whose first member is dead
//! ```

use kestrodb::serde_json::{json, Map, Value};
use kestrodb::{named_params, params, Client, Error, Op, Options, Params, Versioned};
use serde::Serialize;
use std::collections::{BTreeMap, HashMap};
use std::fmt::Debug;
use std::panic::{self, AssertUnwindSafe};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

static CHECKS: AtomicUsize = AtomicUsize::new(0);

fn fail(what: &str) -> ! {
    eprintln!("FAIL {what}");
    std::process::exit(1)
}

fn ok(cond: bool, what: &str) {
    CHECKS.fetch_add(1, Ordering::Relaxed);
    if !cond {
        fail(what)
    }
}

fn eq<A: PartialEq<B> + Debug, B: Debug>(actual: A, expected: B, what: &str) {
    CHECKS.fetch_add(1, Ordering::Relaxed);
    if actual != expected {
        fail(&format!("{what}: got {actual:?} expected {expected:?}"))
    }
}

/// The error `r` must be, by code.
fn fails<T: Debug>(r: Result<T, Error>, code: &str, what: &str) -> Error {
    CHECKS.fetch_add(1, Ordering::Relaxed);
    match r {
        Ok(v) => fail(&format!("{what}: no error (got {v:?}), expected {code}")),
        Err(e) if e.code() == code => e,
        Err(e) => fail(&format!("{what}: {e} ({e:?}), expected {code}")),
    }
}

/// Unwraps, or fails with the expression and its line.
macro_rules! t {
    ($e:expr) => {
        match $e {
            Ok(v) => v,
            Err(e) => fail(&format!("line {}: {}: {}", line!(), stringify!($e), e)),
        }
    };
}

fn sleep_ms(ms: u64) {
    thread::sleep(Duration::from_millis(ms));
}

fn read_line() {
    let mut s = String::new();
    let _ = std::io::stdin().read_line(&mut s);
}

/// The first column of the first row (null when none).
fn value(db: &mut Client, sql: &str, p: impl Into<Params>) -> Value {
    match db.query_value(sql, p) {
        Ok(v) => v.unwrap_or(Value::Null),
        Err(e) => fail(&format!("{sql}: {e}")),
    }
}

/// A type of the program's own, serialized as text.
struct Date(u16, u8, u8);

impl Serialize for Date {
    fn serialize<S: serde::Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.collect_str(&format_args!("{:04}-{:02}-{:02}", self.0, self.1, self.2))
    }
}

#[derive(Debug)]
enum AppError {
    Db(Error),
    Boom,
}

impl From<Error> for AppError {
    fn from(e: Error) -> AppError {
        AppError::Db(e)
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let arg = |i: usize| {
        args.get(i)
            .map(String::as_str)
            .unwrap_or_else(|| fail("missing argument (see the file's comment)"))
    };
    let mode = arg(0);
    match mode {
        "api" => api(arg(1), arg(2)),
        "reconnect" => reconnect(arg(1), arg(2)),
        "busy" => busy(
            arg(1),
            arg(2),
            arg(3)
                .parse()
                .unwrap_or_else(|_| fail("MAX must be a number")),
        ),
        "tls" => tls(arg(1), arg(2), arg(3), arg(4)),
        "cluster" => cluster(arg(1), arg(2), arg(3)),
        "cluster-new" => cluster_new(arg(1), arg(2)),
        _ => fail(&format!("unknown mode {mode}")),
    }
    println!("ok rust {mode} ({} checks)", CHECKS.load(Ordering::Relaxed));
}

// ---------------------------------------------------------------------- client API

fn api(address: &str, token: &str) {
    let opts = || Options::new().token(token);

    // Connecting: a wrong token, bad addresses.
    let auth = fails(
        Client::connect(address, Options::new().token("wrong-token-0123456789")),
        "AUTH",
        "wrong token",
    );
    ok(matches!(auth, Error::Auth(_)), "Error::Auth");
    fails(
        Client::connect("tcp://127.0.0.1", opts()),
        "INPUT",
        "address without a port",
    );
    fails(Client::connect(" , ", opts()), "INPUT", "no address");
    let refused = fails(
        Client::connect("tcp://127.0.0.1:1", opts().timeout(Duration::from_secs(5))),
        "IO",
        "nothing listening",
    );
    ok(
        refused
            .message()
            .starts_with("IO cannot connect to tcp://127.0.0.1:1"),
        &format!("IO message: {refused}"),
    );
    #[cfg(not(feature = "tls"))]
    fails(
        Client::connect("tls://127.0.0.1:1", opts()),
        "INPUT",
        "tls:// without the tls feature",
    );

    let mut db = t!(Client::connect(address, opts()));
    eq(db.address(), address, "address()");
    let st = t!(db.stats());
    eq(&st["server"], &json!(true), "stats server");
    eq(&st["tls"], &json!(false), "stats tls");
    ok(st["keys"].is_u64(), "stats keys is an integer");

    // ---- SQL with every parameter type.
    t!(db.execute("CREATE TABLE IF NOT EXISTS typed (id INTEGER PRIMARY KEY, s TEXT, i INTEGER, r REAL, b BOOLEAN, j JSON, d DATE, u TEXT, n TEXT)", ()));
    t!(db.execute("DELETE FROM typed", ()));
    let text = "quote\" back\\slash\nnew\ttab \u{1} é 日本 😀 ' -- ;";
    let doc = json!({"list": [1, "two", 3.5, null, true], "obj": {"k": "v"}, "empty": {}});
    let uuid = "123e4567-e89b-12d3-a456-426614174000";
    let ins = t!(db.sql(
        "INSERT INTO typed (s, i, r, b, j, d, u, n) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        params![
            text,
            42,
            3.25,
            true,
            doc,
            Date(2026, 11, 20),
            uuid,
            None::<i64>
        ]
    ));
    eq(ins.changes, 1, "insert changes");
    ok(!ins.has_rows() && ins.rows.is_none(), "a write has no rows");
    let id1 = ins
        .last_insert_id
        .clone()
        .unwrap_or_else(|| fail("no lastInsertId"));
    ok(id1.is_i64(), "lastInsertId is an integer");
    eq(db.last_insert_id(), Some(&id1), "client last_insert_id");
    let row = t!(db.query_one("SELECT * FROM typed WHERE id = ?", params![id1]))
        .unwrap_or_else(|| fail("the row"));
    eq(&row["s"], &json!(text), "text round trip");
    eq(&row["i"], &json!(42), "integer");
    eq(&row["r"], &json!(3.25), "real");
    eq(&row["b"], &json!(true), "boolean");
    eq(&row["j"], &doc, "JSON column round trip");
    eq(
        &row["d"],
        &json!("2026-11-20"),
        "own Serialize type (a date) as text",
    );
    eq(&row["u"], &json!(uuid), "uuid text");
    ok(row.get("n").is_none_or(Value::is_null), "null");
    eq(
        row.keys().take(2).map(String::as_str).collect::<Vec<_>>(),
        vec!["id", "s"],
        "columns in table order",
    );
    // Other Rust types.
    t!(db.execute(
        "INSERT INTO typed (s, i, r, b) VALUES (?, ?, ?, ?)",
        params!["long", 9007199254740991u64, 1.5f32, false]
    ));
    t!(db.execute(
        "INSERT INTO typed (s, i, r) VALUES (?, ?, ?)",
        params!["short", -7i16, 12.5f64]
    ));
    t!(db.execute(
        "INSERT INTO typed (s, i, r) VALUES (?, ?, ?)",
        params!["byte", 8u8, 0.1]
    ));
    t!(db.execute(
        "INSERT INTO typed (s, i, j) VALUES (?, ?, ?)",
        params!["bigint", -123456789012i64, ("a", 'c', [1u8, 2])]
    ));
    let mut map = HashMap::new();
    map.insert("k", vec![1, 2]);
    t!(db.execute(
        "INSERT INTO typed (s, j, n) VALUES (?, ?, ?)",
        params![String::from("map"), map, Some("some")]
    ));
    eq(
        value(&mut db, "SELECT i FROM typed WHERE s = ?", params!["long"]),
        json!(9007199254740991u64),
        "2^53-1 exact",
    );
    eq(
        value(&mut db, "SELECT r FROM typed WHERE s = ?", params!["long"]),
        json!(1.5),
        "f32",
    );
    eq(
        value(&mut db, "SELECT b FROM typed WHERE s = ?", params!["long"]),
        json!(false),
        "false",
    );
    eq(
        value(&mut db, "SELECT i FROM typed WHERE s = ?", params!["short"]),
        json!(-7),
        "i16",
    );
    eq(
        value(&mut db, "SELECT r FROM typed WHERE s = ?", params!["short"]),
        json!(12.5),
        "f64",
    );
    eq(
        value(&mut db, "SELECT r FROM typed WHERE s = ?", params!["byte"]),
        json!(0.1),
        "0.1 exactly",
    );
    eq(
        value(
            &mut db,
            "SELECT i FROM typed WHERE s = ?",
            params!["bigint"],
        ),
        json!(-123456789012i64),
        "i64",
    );
    eq(
        value(
            &mut db,
            "SELECT j FROM typed WHERE s = ?",
            params!["bigint"],
        ),
        json!(["a", "c", [1, 2]]),
        "tuple, char and array as JSON",
    );
    eq(
        value(&mut db, "SELECT j FROM typed WHERE s = ?", params!["map"]),
        json!({"k": [1, 2]}),
        "HashMap as JSON",
    );
    eq(
        value(&mut db, "SELECT n FROM typed WHERE s = ?", params!["map"]),
        json!("some"),
        "Some(&str)",
    );
    eq(
        value(
            &mut db,
            "SELECT COUNT(*) FROM typed WHERE b = ?",
            params![true],
        ),
        json!(1),
        "boolean parameter",
    );
    eq(
        value(
            &mut db,
            "SELECT COUNT(*) FROM typed WHERE n IS NULL AND s = ?",
            params![text],
        ),
        json!(1),
        "null column",
    );
    t!(db.execute(
        "INSERT INTO typed (s, r) VALUES (?, ?)",
        params!["nan", f64::NAN]
    ));
    eq(
        value(&mut db, "SELECT r FROM typed WHERE s = 'nan'", ()),
        Value::Null,
        "NaN is sent as null",
    );
    fails(
        db.execute("INSERT INTO typed (i) VALUES (?)", params!["not a number"]),
        "INPUT",
        "declared type enforced",
    );
    let mut bad_key = BTreeMap::new();
    bad_key.insert(vec![1u8], 1);
    fails(
        db.execute("INSERT INTO typed (j) VALUES (?)", params![bad_key]),
        "INPUT",
        "a map with non-text keys",
    );
    // Rows as your own types.
    let typed: Vec<BTreeMap<String, Value>> =
        t!(db.query_as("SELECT s, i FROM typed WHERE s = ?", params!["short"]));
    eq(&typed[0]["i"], &json!(-7), "query_as");
    let ints: Vec<HashMap<String, i64>> = t!(db.query_as(
        "SELECT i FROM typed WHERE s IN ('short', 'byte') ORDER BY i",
        ()
    ));
    eq(
        ints.iter().map(|m| m["i"]).collect::<Vec<_>>(),
        vec![-7, 8],
        "query_as numbers",
    );
    fails(
        db.query_as::<HashMap<String, i64>>("SELECT s FROM typed", ()),
        "INPUT",
        "query_as into the wrong type",
    );

    // ---- Placeholders: ?, ?N, $N, :name, @name; lists and maps.
    t!(db.execute("CREATE TABLE IF NOT EXISTS people (id INTEGER PRIMARY KEY, email TEXT UNIQUE, name TEXT, age INTEGER, city TEXT)", ()));
    t!(db.execute("DELETE FROM people", ()));
    eq(
        t!(db.execute(
            "INSERT INTO people (email, name, age, city) VALUES (:email, :name, :age, @city)",
            named_params! { "email" => "a@x.org", ":name" => "Ann", "@age" => 31, "city" => "Kandy" }
        )),
        1,
        "named insert",
    );
    let ann = db.last_insert_id().cloned();
    t!(db.execute(
        "INSERT INTO people (email, name, age, city) VALUES (?, ?, ?, ?)",
        vec![json!("b@x.org"), json!("Bob"), json!(25), json!("Galle")]
    ));
    t!(db.execute(
        "INSERT INTO people (email, name, age, city) VALUES ($2, $1, $3, $4)",
        json!(["Cy", "c@x.org", 41, "Kandy"])
    ));
    t!(db.execute(
        "INSERT INTO people (email, name, age, city) VALUES (?2, ?1, ?3, ?3)",
        params!["Dee", "d@x.org", Value::Null]
    ));
    eq(
        value(
            &mut db,
            "SELECT name FROM people WHERE id = ?",
            params![ann],
        ),
        json!("Ann"),
        "last_insert_id row",
    );
    let older = t!(db.query(
        "SELECT name FROM people WHERE age > :min ORDER BY age",
        Params::named([("min", 26)])
    ));
    eq(older.len(), 2, "query size");
    eq(
        Value::Object(older[0].clone()),
        json!({"name": "Ann"}),
        "query rows",
    );
    let bob = t!(db.query_one(
        "SELECT name, age FROM people WHERE email = @e",
        json!({"e": "b@x.org"})
    ));
    eq(
        bob.map(Value::Object),
        Some(json!({"name": "Bob", "age": 25})),
        "query_one with a JSON object",
    );
    ok(
        t!(db.query_one("SELECT * FROM people WHERE id = ?", params![999])).is_none(),
        "query_one none",
    );
    ok(
        t!(db.query_value("SELECT name FROM people WHERE id = ?", params![999])).is_none(),
        "query_value none",
    );
    eq(
        value(&mut db, "SELECT COUNT(*) FROM people", ()),
        json!(4),
        "query_value",
    );
    eq(
        value(
            &mut db,
            "SELECT COUNT(*) FROM people WHERE city = $1 OR name = $1",
            params!["Kandy"],
        ),
        json!(2),
        "$1 twice",
    );
    eq(
        t!(db.query("SELECT * FROM people WHERE id = ?", [json!(-1)])).len(),
        0,
        "empty result",
    );
    ok(
        t!(db.sql("SELECT * FROM people WHERE id = -1", ())).has_rows(),
        "an empty result still has rows",
    );
    eq(
        t!(db.sql("SELECT name, age FROM people ORDER BY id", ())).columns(),
        vec!["name", "age"],
        "columns()",
    );
    eq(
        t!(db.execute(
            "UPDATE people SET age = age + 1 WHERE city = ?",
            params!["Kandy"]
        )),
        2,
        "update changes",
    );
    eq(
        t!(db.execute("DELETE FROM people WHERE email = ?", params!["zzz"])),
        0,
        "delete none",
    );
    let by_city = Params::positional(["Kandy"]);
    eq(
        t!(db.query(
            "SELECT name FROM people WHERE city = ? ORDER BY name",
            &by_city
        ))
        .len(),
        2,
        "reused Params",
    );
    eq(
        t!(db.query(
            "SELECT name FROM people WHERE city = ? ORDER BY name",
            &by_city
        ))
        .len(),
        2,
        "reused Params again",
    );
    fails(
        db.query("UPDATE people SET age = 1 WHERE id = -1", ()),
        "INPUT",
        "query() of a write",
    );

    // ---- Errors by code.
    let dup = fails(
        db.execute("INSERT INTO people (email) VALUES (?)", params!["a@x.org"]),
        "CONFLICT",
        "duplicate",
    );
    ok(matches!(dup, Error::Conflict(_)), "Error::Conflict");
    let boxed: Box<dyn std::error::Error + Send + Sync> = Box::new(dup.clone());
    ok(
        boxed.to_string().starts_with("CONFLICT"),
        "std::error::Error",
    );
    let bad = fails(db.query("SELEC nonsense", ()), "INPUT", "bad SQL");
    ok(matches!(bad, Error::Input(_)), "Error::Input");
    fails(
        db.query("SELECT * FROM people WHERE id = :id", Map::new()),
        "INPUT",
        "missing named param",
    );
    fails(
        db.query("SELECT * FROM people WHERE id = ?", ()),
        "INPUT",
        "missing positional param",
    );
    fails(
        db.query(
            "SELECT * FROM people WHERE id = :id AND age = ?",
            params![1, 2],
        ),
        "INPUT",
        "mixed params",
    );
    let mut mixed = params![1];
    mixed.insert("a", 2);
    fails(db.query("SELECT ?", mixed), "INPUT", "mixed Params");
    fails(
        db.query("SELECT * FROM no_such_table", ()),
        "INPUT",
        "missing table",
    );
    ok(
        matches!(Error::from_message("STALE something"), Error::Other(_)),
        "other codes: Other",
    );
    eq(
        Error::from_message("STALE something").code(),
        "STALE",
        "other codes: code",
    );
    ok(
        matches!(Error::from_message("UNAVAILABLE x"), Error::Unavailable(_)),
        "UNAVAILABLE",
    );
    ok(
        matches!(Error::from_message("READONLY x"), Error::ReadOnly(_)),
        "READONLY",
    );
    ok(
        matches!(Error::from_message("BUSY x"), Error::Busy(_)),
        "BUSY",
    );
    ok(
        matches!(Error::from_message("CORRUPT x"), Error::Corrupt(_)),
        "CORRUPT",
    );
    ok(
        matches!(Error::from_message("LIMIT x"), Error::Limit(_)),
        "LIMIT",
    );
    ok(
        matches!(Error::from_message("CLOSED x"), Error::Closed(_)),
        "CLOSED",
    );
    eq(
        Error::from_message("weird message").code(),
        "ERROR",
        "no code",
    );
    eq(
        value(&mut db, "SELECT COUNT(*) FROM people", ()),
        json!(4),
        "usable after errors",
    );

    // ---- Transactions.
    t!(db.execute(
        "CREATE TABLE IF NOT EXISTS acct (id INTEGER PRIMARY KEY, bal INTEGER)",
        ()
    ));
    t!(db.execute("DELETE FROM acct", ()));
    t!(db.execute("INSERT INTO acct (id, bal) VALUES (1, 100), (2, 0)", ()));
    let mut other = t!(Client::connect(address, opts()));
    t!(db.begin());
    ok(db.in_transaction(), "begin() opens");
    fails(db.begin(), "INPUT", "nested begin");
    t!(db.execute("UPDATE acct SET bal = bal - 10 WHERE id = 1", ()));
    eq(
        value(&mut db, "SELECT bal FROM acct WHERE id = 1", ()),
        json!(90),
        "reads its own write",
    );
    eq(
        value(&mut other, "SELECT bal FROM acct WHERE id = 1", ()),
        json!(100),
        "others do not see it yet",
    );
    fails(
        db.set("kv", "inside", 1),
        "INPUT",
        "key/value write inside a SQL transaction",
    );
    ok(db.in_transaction(), "still open after a refused statement");
    t!(db.commit());
    ok(!db.in_transaction(), "commit() closes");
    eq(
        value(&mut other, "SELECT bal FROM acct WHERE id = 1", ()),
        json!(90),
        "committed",
    );
    t!(db.commit()); // nothing open: nothing to do
    t!(db.rollback());
    t!(db.begin());
    t!(db.execute("UPDATE acct SET bal = 0 WHERE id = 1", ()));
    t!(db.rollback());
    ok(!db.in_transaction(), "rollback() closes");
    eq(
        value(&mut db, "SELECT bal FROM acct WHERE id = 1", ()),
        json!(90),
        "rolled back",
    );
    // As SQL text.
    t!(db.sql("BEGIN", ()));
    ok(db.in_transaction(), "BEGIN as SQL");
    t!(db.execute("UPDATE acct SET bal = 1 WHERE id = 2", ()));
    t!(db.sql("ROLLBACK", ()));
    ok(!db.in_transaction(), "ROLLBACK as SQL");
    t!(db.sql("start transaction", ()));
    ok(db.in_transaction(), "START TRANSACTION");
    t!(db.sql("  commit;", ()));
    ok(!db.in_transaction(), "commit; with spaces");
    // transaction(): commit, or roll back and return the error.
    t!(db.transaction(|tx| {
        tx.execute("UPDATE acct SET bal = bal - 30 WHERE id = 1", ())?;
        tx.execute("UPDATE acct SET bal = bal + 30 WHERE id = 2", ())?;
        Ok::<_, Error>(())
    }));
    eq(
        value(&mut db, "SELECT SUM(bal) FROM acct", ()),
        json!(90),
        "transaction() kept the total",
    );
    eq(
        value(&mut db, "SELECT bal FROM acct WHERE id = 2", ()),
        json!(30),
        "transaction() committed",
    );
    let r = db.transaction(|tx| {
        tx.execute("UPDATE acct SET bal = 0", ())?;
        Err::<(), _>(AppError::Boom)
    });
    ok(
        matches!(r, Err(AppError::Boom)),
        &format!("transaction() returns the body's error: {r:?}"),
    );
    ok(!db.in_transaction(), "rolled back after the error");
    eq(
        value(&mut db, "SELECT bal FROM acct WHERE id = 2", ()),
        json!(30),
        "transaction() rolled back",
    );
    let r = db.transaction(|tx| -> Result<(), AppError> {
        tx.execute("SELEC broken", ())?;
        Ok(())
    });
    ok(
        matches!(&r, Err(AppError::Db(e)) if e.code() == "INPUT"),
        "a kestrodb error converted with From",
    );
    let quiet = panic::take_hook();
    panic::set_hook(Box::new(|_| {}));
    let caught = panic::catch_unwind(AssertUnwindSafe(|| {
        let _ = db.transaction(|tx| -> Result<(), Error> {
            tx.execute("UPDATE acct SET bal = 0", ())?;
            panic!("boom")
        });
    }));
    panic::set_hook(quiet);
    ok(caught.is_err(), "a panic in transaction() continues");
    ok(!db.in_transaction(), "rolled back after the panic");
    eq(
        value(&mut db, "SELECT bal FROM acct WHERE id = 2", ()),
        json!(30),
        "nothing of the panicking body kept",
    );
    eq(
        t!(db.transaction(|tx| tx.query_value("SELECT bal FROM acct WHERE id = 1", ()))),
        Some(json!(60)),
        "transaction() returns a value",
    );
    let r = db.transaction(|tx| {
        tx.execute("INSERT INTO people (email) VALUES (?)", params!["zz@x.org"])?;
        tx.execute("INSERT INTO people (email) VALUES (?)", params!["zz@x.org"])?;
        Ok::<_, Error>(())
    });
    fails(r, "CONFLICT", "conflict inside transaction()");
    ok(!db.in_transaction(), "closed after a conflict");
    eq(
        value(
            &mut db,
            "SELECT COUNT(*) FROM people WHERE email = 'zz@x.org'",
            (),
        ),
        json!(0),
        "nothing of it kept",
    );
    // Savepoints: ROLLBACK TO keeps the transaction; a SAVEPOINT outside one begins it.
    t!(db.begin());
    t!(db.execute("UPDATE acct SET bal = 1000 WHERE id = 1", ()));
    t!(db.sql("SAVEPOINT sp1", ()));
    t!(db.execute("UPDATE acct SET bal = 2000 WHERE id = 1", ()));
    t!(db.sql("ROLLBACK TO SAVEPOINT sp1", ()));
    ok(
        db.in_transaction(),
        "ROLLBACK TO keeps the transaction open",
    );
    eq(
        value(&mut db, "SELECT bal FROM acct WHERE id = 1", ()),
        json!(1000),
        "ROLLBACK TO undid the later write",
    );
    t!(db.rollback());
    eq(
        value(&mut db, "SELECT bal FROM acct WHERE id = 1", ()),
        json!(60),
        "whole rollback",
    );
    t!(db.sql("SAVEPOINT outer_sp", ()));
    ok(
        db.in_transaction(),
        "SAVEPOINT outside a transaction begins one",
    );
    t!(db.execute("UPDATE acct SET bal = 61 WHERE id = 1", ()));
    t!(db.sql("RELEASE SAVEPOINT outer_sp", ()));
    ok(!db.in_transaction(), "releasing it commits");
    eq(
        value(&mut other, "SELECT bal FROM acct WHERE id = 1", ()),
        json!(61),
        "released savepoint committed",
    );
    // BEGIN CONCURRENT: first committer wins; the loser's COMMIT fails and ends it.
    t!(db.sql("BEGIN CONCURRENT", ()));
    t!(other.sql("BEGIN CONCURRENT", ()));
    t!(db.execute("UPDATE acct SET bal = 1 WHERE id = 2", ()));
    t!(other.execute("UPDATE acct SET bal = 2 WHERE id = 2", ()));
    t!(other.commit());
    let lost = fails(db.commit(), "CONFLICT", "second committer loses");
    ok(
        matches!(lost, Error::Conflict(_)),
        "Error::Conflict from COMMIT",
    );
    ok(!db.in_transaction(), "a failed COMMIT ends the transaction");
    eq(
        value(&mut db, "SELECT bal FROM acct WHERE id = 2", ()),
        json!(2),
        "first committer's value",
    );

    // ---- Key/value with versions.
    t!(db.delete("kv", "k1"));
    let v1 = t!(db.set("kv", "k1", json!({"n": 1})));
    ok(v1 > 0, "set returns a version");
    let got = t!(db.get("kv", "k1")).unwrap_or_else(|| fail("k1"));
    eq(got.version, v1, "get version");
    eq(&got.value, &json!({"n": 1}), "get value");
    ok(t!(db.get("kv", "missing")).is_none(), "get missing");
    let v2 = t!(db.set_if("kv", "k1", json!({"n": 2}), v1));
    ok(v2 > v1, "conditional set at the right version");
    let stale = fails(
        db.set_if("kv", "k1", json!({"n": 3}), v1),
        "CONFLICT",
        "stale expected_version",
    );
    ok(
        matches!(stale, Error::Conflict(_)),
        "stale is Error::Conflict",
    );
    eq(
        t!(db.get("kv", "k1")).map(|v| v.value),
        Some(json!({"n": 2})),
        "not overwritten",
    );
    fails(
        db.set_if("kv", "k1", 1, 0),
        "CONFLICT",
        "expected_version 0 on an existing key",
    );
    t!(db.delete("kv", "fresh"));
    let vf = t!(db.set_if("kv", "fresh", "new", 0));
    eq(
        t!(db.get("kv", "fresh")),
        Some(Versioned {
            version: vf,
            value: json!("new"),
        }),
        "expected_version 0 creates",
    );
    fails(
        db.delete_if("kv", "fresh", vf + 1000),
        "CONFLICT",
        "conditional delete, wrong version",
    );
    t!(db.delete_if("kv", "fresh", vf));
    ok(t!(db.get("kv", "fresh")).is_none(), "conditional delete");
    // Values of every JSON type, and typed values.
    let values = [
        Value::Null,
        json!(true),
        json!(false),
        json!(0),
        json!(-1),
        json!(i64::MAX),
        json!(i64::MIN),
        json!(3.5),
        json!(-1.25e-10),
        json!(""),
        json!("text 😀 \"q\" \\ \n"),
        json!([]),
        json!({}),
        json!({"deep": [{"x": [1, null, "y"]}]}),
    ];
    for (i, v) in values.iter().enumerate() {
        t!(db.set("kv", &format!("type{i}"), v));
        eq(
            t!(db.get("kv", &format!("type{i}"))).map(|x| x.value),
            Some(v.clone()),
            &format!("value round trip {i}"),
        );
    }
    let vt = t!(db.set("kv", "typed", vec![1u32, 2, 3]));
    eq(
        t!(db.get_as::<Vec<u32>>("kv", "typed")),
        Some(Versioned {
            version: vt,
            value: vec![1, 2, 3],
        }),
        "get_as",
    );
    fails(
        db.get_as::<String>("kv", "typed"),
        "INPUT",
        "get_as into the wrong type",
    );
    // Unicode keys; key and collection rules.
    let ukey = "ключ/日本/😀 with spaces";
    t!(db.set("kv", ukey, "unicode key"));
    eq(
        t!(db.get("kv", ukey)).map(|v| v.value),
        Some(json!("unicode key")),
        "unicode key",
    );
    fails(db.get("bad-name", "k"), "INPUT", "invalid collection");
    fails(db.get("kv", ""), "INPUT", "empty key");
    fails(db.get("kv", "a\0b"), "INPUT", "NUL in key");
    fails(
        db.get("kv", &"x".repeat(513)),
        "INPUT",
        "key over 512 bytes",
    );
    t!(db.set("kv", &"y".repeat(512), 1));
    fails(
        db.set("kv", "huge", "z".repeat(300 * 1024)),
        "LIMIT",
        "value over 256 KiB",
    );
    let big = "b".repeat(200 * 1024);
    t!(db.set("kv", "big", &big));
    eq(
        t!(db.get("kv", "big")).map(|v| v.value),
        Some(json!(big)),
        "200 KiB value",
    );
    // get_many: one snapshot, in order, None where missing.
    let many = t!(db.get_many("kv", &["k1", "missing", "type3", ukey]));
    eq(many.len(), 4, "get_many size");
    eq(
        many[0].as_ref().map(|v| &v.value),
        Some(&json!({"n": 2})),
        "get_many 0",
    );
    ok(many[1].is_none(), "get_many missing");
    eq(
        many[2].as_ref().map(|v| &v.value),
        Some(&json!(0)),
        "get_many 2",
    );
    eq(
        many[3].as_ref().map(|v| &v.value),
        Some(&json!("unicode key")),
        "get_many unicode",
    );
    eq(t!(db.get_many::<&str>("kv", &[])).len(), 0, "get_many none");
    let too_many: Vec<String> = (0..4097).map(|i| format!("k{i}")).collect();
    fails(db.get_many("kv", &too_many), "LIMIT", "get_many over 4096");
    // batch: all or nothing, one version.
    let vb = t!(db.batch(&[
        Op::set("kv", "b1", 1),
        Op::set("kv", "b2", 2),
        Op::delete("kv", "type0")
    ]));
    eq(
        t!(db.get("kv", "b1")).map(|v| v.version),
        Some(vb),
        "batch version b1",
    );
    eq(
        t!(db.get("kv", "b2")).map(|v| v.version),
        Some(vb),
        "batch version b2",
    );
    ok(t!(db.get("kv", "type0")).is_none(), "batch delete");
    fails(
        db.batch(&[
            Op::set("kv", "b1", 10),
            Op::set("kv", "b2", 20).if_version(vb - 1),
        ]),
        "CONFLICT",
        "batch with a stale version",
    );
    eq(
        t!(db.get("kv", "b1")).map(|v| v.value),
        Some(json!(1)),
        "nothing of a failed batch applied",
    );
    fails(
        db.batch(&[Op::set("kv", "d", 1), Op::delete("kv", "d")]),
        "INPUT",
        "duplicate key in a batch",
    );
    fails(db.batch(&[]), "INPUT", "empty batch");
    let ops: Vec<Op> = (0..1000)
        .map(|i| Op::set("bulk", format!("k{i}"), json!({"i": i})))
        .collect();
    let vbulk = t!(db.batch(&ops));
    eq(
        t!(db.get("bulk", "k999")),
        Some(Versioned {
            version: vbulk,
            value: json!({"i": 999}),
        }),
        "1000-op batch",
    );
    // Server commands.
    ok(
        t!(db.stats())["keys"].as_u64().unwrap_or(0) > 1000,
        "stats keys",
    );
    t!(db.verify());
    fails(db.backup("x.kdb"), "INPUT", "backup without --backup-dir");
    let compacted = t!(db.compact());
    ok(
        compacted.contains_key("before") && compacted.contains_key("after"),
        "compact()",
    );
    eq(
        t!(db.get("bulk", "k5")).map(|v| v.value),
        Some(json!({"i": 5})),
        "reads after compaction",
    );

    // ---- Threads: one client behind a Mutex; a client moved to another thread.
    let shared = Arc::new(Mutex::new(t!(Client::connect(address, opts()))));
    let workers: Vec<_> = (0..8)
        .map(|n| {
            let shared = Arc::clone(&shared);
            thread::spawn(move || -> Result<(), String> {
                for i in 0..50 {
                    let mut c = shared.lock().map_err(|e| e.to_string())?;
                    let key = format!("t{n}_{i}");
                    c.set("threads", &key, i).map_err(|e| e.to_string())?;
                    let v = c
                        .get("threads", &key)
                        .map_err(|e| e.to_string())?
                        .map(|v| v.value);
                    if v != Some(json!(i)) {
                        return Err(format!("thread read {v:?}"));
                    }
                }
                Ok(())
            })
        })
        .collect();
    for w in workers {
        if let Err(e) = w.join().unwrap_or_else(|_| Err("a thread panicked".into())) {
            fail(&format!("threads: {e}"));
        }
    }
    let (mut other, n) = thread::spawn(move || {
        let n = other.query_value("SELECT COUNT(*) FROM people", ());
        (other, n)
    })
    .join()
    .unwrap_or_else(|_| fail("the moved client's thread panicked"));
    eq(t!(n), Some(json!(4)), "a client moved to another thread");

    // ---- An idle connection is kept (no reconnect when nothing happened).
    let opened = db.connections_opened();
    sleep_ms(1200);
    eq(
        value(&mut db, "SELECT COUNT(*) FROM people", ()),
        json!(4),
        "after an idle second",
    );
    eq(
        db.connections_opened(),
        opened,
        "the idle connection was kept, not reopened",
    );

    // ---- Closed.
    other.close();
    db.close();
    db.close(); // twice is fine
    ok(db.is_closed(), "is_closed");
    let closed = fails(db.get("kv", "k1"), "CLOSED", "use after close");
    ok(matches!(closed, Error::Closed(_)), "Error::Closed");
    {
        let mut scoped = t!(Client::connect(address, opts()));
        eq(
            t!(scoped.get("kv", "k1")).map(|v| v.value),
            Some(json!({"n": 2})),
            "a client closed by Drop",
        );
    }
}

// ---------------------------------------------------------------------- reconnect

fn reconnect(address: &str, token: &str) {
    let opts = || Options::new().token(token);
    let mut db = t!(Client::connect(address, opts()));
    t!(db.execute(
        "CREATE TABLE IF NOT EXISTS rc (id INTEGER PRIMARY KEY, v TEXT)",
        ()
    ));
    t!(db.execute("INSERT INTO rc (v) VALUES (?)", params!["before"]));
    t!(db.set("rkv", "k", "before"));
    let mut tx = t!(Client::connect(address, opts()));
    let mut off = t!(Client::connect(address, opts().reconnect(false)));
    t!(off.stats());
    println!("READY");
    read_line(); // the server restarts meanwhile
    sleep_ms(1500); // idle long enough for the check before the next request
    let opened = db.connections_opened();
    t!(db.execute("INSERT INTO rc (v) VALUES (?)", params!["after"]));
    eq(db.connections_opened(), opened + 1, "reopened once");
    eq(
        value(&mut db, "SELECT COUNT(*) FROM rc", ()),
        json!(2),
        "rows after reconnect",
    );
    eq(
        t!(db.get("rkv", "k")).map(|v| v.value),
        Some(json!("before")),
        "key/value after reconnect",
    );
    // A transaction cannot survive its connection: the next call says so, the one after reconnects.
    t!(tx.begin());
    ok(tx.in_transaction(), "transaction on the new server");
    t!(tx.execute("INSERT INTO rc (v) VALUES (?)", params!["in tx"]));
    println!("READY2");
    read_line(); // restarted again, with tx's transaction open
    let lost = fails(
        tx.execute("INSERT INTO rc (v) VALUES (?)", params!["x"]),
        "IO",
        "call inside a lost transaction",
    );
    ok(
        lost.message()
            .starts_with("IO connection lost inside a transaction; it was rolled back"),
        &format!("lost transaction message: {lost}"),
    );
    ok(lost.is_unknown_outcome(), "IO is an unknown outcome");
    ok(!tx.in_transaction(), "no longer in a transaction");
    eq(
        value(&mut tx, "SELECT COUNT(*) FROM rc", ()),
        json!(2),
        "the next call reconnects; the transaction's row is gone",
    );
    // reconnect(false): the dead connection fails with IO, and stays failed.
    let off_err = fails(off.stats(), "IO", "reconnect off");
    ok(matches!(off_err, Error::Io(_)), "Error::Io");
    fails(off.stats(), "IO", "reconnect off, again");
}

// ---------------------------------------------------------------------- busy

fn busy(address: &str, token: &str, max: usize) {
    let opts = || Options::new().token(token);
    let mut held = Vec::new();
    for _ in 0..max {
        let mut c = t!(Client::connect(address, opts()));
        t!(c.stats());
        held.push(c);
    }
    // The server answers BUSY and closes at once; the client's login frame, still unread, can
    // make that close a reset that discards the BUSY reply (then: IO). Every attempt must
    // fail, and BUSY must be seen.
    let mut busy = None;
    for _ in 0..20 {
        match Client::connect(address, opts()).and_then(|mut c| c.stats()) {
            Ok(_) => fail("a connection over --max-connections was accepted"),
            Err(e @ Error::Busy(_)) => {
                busy = Some(e);
                break;
            }
            Err(e @ Error::Io(_)) => ok(
                e.message().contains("before login") || e.message().contains("connection lost"),
                &format!("over the limit: {e}"),
            ),
            Err(e) => fail(&format!("over the limit: {e}")),
        }
    }
    // Deterministic: a client that has sent nothing yet reads the server's BUSY before its
    // first request (a client without a token sends nothing when it connects).
    let mut quiet = t!(Client::connect(address, Options::new()));
    sleep_ms(300);
    let waited = fails(
        quiet.stats(),
        "BUSY",
        "BUSY waiting before the first request",
    );
    drop(quiet);
    let busy = busy.unwrap_or(waited);
    ok(matches!(busy, Error::Busy(_)), "Error::Busy");
    ok(
        busy.message().starts_with("BUSY too many connections"),
        &format!("BUSY message: {busy}"),
    );
    drop(held.remove(0)); // Drop closes: a slot frees
    let mut again = None;
    for _ in 0..50 {
        match Client::connect(address, opts()).and_then(|mut c| c.stats().map(|_| c)) {
            Ok(c) => {
                again = Some(c);
                break;
            }
            Err(_) => sleep_ms(100),
        }
    }
    ok(again.is_some(), "a slot freed by dropping a client");
}

// ---------------------------------------------------------------------- TLS

#[cfg(not(feature = "tls"))]
fn tls(_address: &str, _token: &str, _ca: &str, _wrong_ca: &str) {
    fail("built without the tls feature (cargo build --features tls)")
}

#[cfg(feature = "tls")]
fn tls(address: &str, token: &str, ca: &str, wrong_ca: &str) {
    let port = address
        .rsplit(':')
        .next()
        .unwrap_or_else(|| fail("no port"));
    let opts = || Options::new().token(token).tls_ca_file(ca);
    let mut db = t!(Client::connect(address, opts()));
    eq(&t!(db.stats())["tls"], &json!(true), "stats tls");
    t!(db.execute(
        "CREATE TABLE IF NOT EXISTS tl (id INTEGER PRIMARY KEY, body TEXT)",
        ()
    ));
    let big = "t".repeat(300_000); // many TLS records each way
    t!(db.execute("INSERT INTO tl (body) VALUES (?)", params![big]));
    let id = db.last_insert_id().cloned();
    eq(
        value(&mut db, "SELECT body FROM tl WHERE id = ?", params![id])
            .as_str()
            .map(str::len),
        Some(300_000),
        "300 KB over TLS",
    );
    let v = t!(db.set("kv", "tls", json!({"ok": true})));
    eq(
        t!(db.get("kv", "tls")),
        Some(Versioned {
            version: v,
            value: json!({"ok": true}),
        }),
        "key/value over TLS",
    );
    let opened = db.connections_opened();
    sleep_ms(1200);
    eq(
        t!(db.get("kv", "tls")).map(|x| x.version),
        Some(v),
        "after an idle second",
    );
    eq(
        db.connections_opened(),
        opened,
        "an idle TLS connection is kept",
    );
    // The certificate names localhost and 127.0.0.1.
    {
        let mut ip = t!(Client::connect(format!("tls://127.0.0.1:{port}"), opts()));
        eq(&t!(ip.stats())["tls"], &json!(true), "IP address SAN");
    }
    let wrong = fails(
        Client::connect(address, Options::new().token(token).tls_ca_file(wrong_ca)),
        "IO",
        "wrong CA",
    );
    ok(
        wrong.message().contains("TLS handshake failed"),
        &format!("handshake message: {wrong}"),
    );
    fails(
        Client::connect(address, Options::new().token(token)),
        "IO",
        "the system CAs do not know the test CA",
    );
    fails(
        Client::connect(address, opts().tls_server_name("other.example")),
        "IO",
        "wrong host name",
    );
    fails(
        Client::connect(address, opts().tls_ca_file("/nonexistent/ca.pem")),
        "IO",
        "missing CA file",
    );
    {
        let mut insecure = t!(Client::connect(
            address,
            Options::new().token(token).tls_verify(false)
        ));
        eq(
            &t!(insecure.stats())["tls"],
            &json!(true),
            "tls_verify(false)",
        );
    }
    fails(
        Client::connect(
            format!("tcp://localhost:{port}"),
            Options::new().token(token).timeout(Duration::from_secs(5)),
        )
        .and_then(|mut c| c.stats()),
        "IO",
        "plain TCP to a TLS port",
    );
    println!("READY");
    read_line(); // the TLS server restarts
    sleep_ms(1500);
    eq(
        t!(db.get("kv", "tls")).map(|x| x.value),
        Some(json!({"ok": true})),
        "TLS reconnect after a restart",
    );
    eq(db.connections_opened(), opened + 1, "reopened once");
}

// ---------------------------------------------------------------------- cluster

fn cluster_options(token: &str) -> Options {
    Options::new()
        .token(token)
        .timeout(Duration::from_secs(3))
        .failover_timeout(Duration::from_secs(25))
}

fn cluster(list: &str, token: &str, follower: &str) {
    // Only the follower: a write is refused with READONLY naming the leader; reads work.
    {
        let mut f = t!(Client::connect(follower, Options::new().token(token)));
        let ro = fails(
            f.execute("INSERT INTO t (v) VALUES ('x')", ()),
            "READONLY",
            "a follower refuses writes",
        );
        ok(matches!(ro, Error::ReadOnly(_)), "Error::ReadOnly");
        ok(
            ro.message().contains("the leader is"),
            &format!("names the leader: {ro}"),
        );
        t!(f.query_value("SELECT COUNT(*) FROM t", ()));
    }
    let mut db = t!(Client::connect(list, cluster_options(token)));
    eq(db.address(), follower, "starts at the first member");
    for i in 0..5 {
        t!(db.execute("INSERT INTO t (v) VALUES (?)", params![format!("a{i}")]));
    }
    let v = t!(db.set("ckv", "k", "v1"));
    eq(
        t!(db.get("ckv", "k")).map(|x| x.version),
        Some(v),
        "key/value at the leader",
    );
    println!("ADDRESS {}", db.address());
    // The members as a Vec.
    {
        let members: Vec<&str> = list.split(',').collect();
        let mut c = t!(Client::connect(members, cluster_options(token)));
        eq(
            t!(c.execute("INSERT INTO t (v) VALUES ('vec')", ())),
            1,
            "a Vec of members follows the leader",
        );
        eq(c.address(), db.address(), "at the same leader");
    }
    println!("READY");
    read_line(); // the leader is killed and a new one elected
    sleep_ms(1500);
    for i in 0..5 {
        t!(db.execute("INSERT INTO t (v) VALUES (?)", params![format!("b{i}")]));
    }
    eq(
        t!(db.get("ckv", "k")).map(|x| x.value),
        Some(json!("v1")),
        "key/value survived the failover",
    );
    println!("ADDRESS {}", db.address());
}

fn cluster_new(list: &str, token: &str) {
    let mut db = t!(Client::connect(list, cluster_options(token)));
    for i in 0..5 {
        t!(db.execute("INSERT INTO t (v) VALUES (?)", params![format!("c{i}")]));
    }
    println!("ADDRESS {}", db.address());
}

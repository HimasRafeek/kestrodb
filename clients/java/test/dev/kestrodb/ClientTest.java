package dev.kestrodb;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.sql.BatchUpdateException;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.SQLIntegrityConstraintViolationException;
import java.sql.SQLInvalidAuthorizationSpecException;
import java.sql.SQLSyntaxErrorException;
import java.sql.Statement;
import java.sql.Types;
import java.time.Duration;
import java.time.LocalDate;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicReference;

/**
 * The Java client against a real kestro-server. Exits non-zero on the first failure.
 *
 * <pre>
 * java -cp kestrodb.jar;build/test dev.kestrodb.ClientTest api ADDRESS TOKEN
 * ... reconnect ADDRESS TOKEN          survives a server restart (waits for a line on stdin)
 * ... busy ADDRESS TOKEN MAX            the server was started with --max-connections MAX
 * ... tls ADDRESS TOKEN CA WRONG_CA     ADDRESS is tls://localhost:PORT (waits for a restart too)
 * ... cluster LIST TOKEN FOLLOWER       follows the leader; waits for the leader to be killed
 * ... cluster-new LIST TOKEN            a new client whose first member is dead
 * </pre>
 */
public final class ClientTest {
    private static int checks;

    static void fail(String what) {
        System.err.println("FAIL " + what);
        System.exit(1);
    }

    static void eq(Object actual, Object expected, String what) {
        checks++;
        boolean same = actual == null ? expected == null : actual.equals(expected);
        if (!same) fail(what + ": got " + show(actual) + " expected " + show(expected));
    }

    static void ok(boolean cond, String what) {
        checks++;
        if (!cond) fail(what);
    }

    static String show(Object v) {
        if (v == null) return "null";
        try { return Json.stringify(v) + " (" + v.getClass().getSimpleName() + ")"; } catch (RuntimeException e) { return String.valueOf(v); }
    }

    interface Body { void run() throws Exception; }

    @SuppressWarnings("unchecked")
    static <E extends Throwable> E throwsA(Class<E> type, Body body, String what) {
        checks++;
        try {
            body.run();
        } catch (Throwable e) {
            if (type.isInstance(e)) return (E) e;
            e.printStackTrace();
            fail(what + ": threw " + e.getClass().getName() + ": " + e.getMessage() + ", expected " + type.getSimpleName());
        }
        fail(what + ": no exception, expected " + type.getSimpleName());
        return null;
    }

    static Map<String, Object> map(Object... kv) {
        Map<String, Object> m = new LinkedHashMap<>();
        for (int i = 0; i < kv.length; i += 2) m.put((String) kv[i], kv[i + 1]);
        return m;
    }

    static void sleep(long ms) {
        try { Thread.sleep(ms); } catch (InterruptedException e) { throw new RuntimeException(e); }
    }

    private static final BufferedReader STDIN = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));

    static String readLine() throws Exception {
        return STDIN.readLine();
    }

    enum Color { RED, GREEN }

    public static void main(String[] args) throws Exception {
        String mode = args.length > 0 ? args[0] : "";
        switch (mode) {
            case "json": json(); break;
            case "api": json(); api(args[1], args[2]); jdbc(args[1], args[2]); break;
            case "reconnect": reconnect(args[1], args[2]); break;
            case "busy": busy(args[1], args[2], Integer.parseInt(args[3])); break;
            case "tls": tls(args[1], args[2], args[3], args[4]); break;
            case "cluster": cluster(args[1], args[2], args[3]); break;
            case "cluster-new": clusterNew(args[1], args[2]); break;
            default: fail("unknown mode " + mode);
        }
        System.out.println("ok java " + mode + " (" + checks + " checks)");
        System.exit(0);
    }

    // ------------------------------------------------------------------ JSON

    static void json() {
        eq(Json.parse("{\"a\":1,\"b\":[true,false,null],\"c\":-2.5e3,\"d\":\"x\"}"), map("a", 1L, "b", Arrays.asList(true, false, null), "c", -2500.0, "d", "x"), "parse object");
        eq(Json.parse("9223372036854775807"), Long.MAX_VALUE, "long max");
        eq(Json.parse("-9223372036854775808"), Long.MIN_VALUE, "long min");
        eq(Json.parse("9223372036854775808"), new BigInteger("9223372036854775808"), "beyond long");
        eq(Json.parse("0"), 0L, "zero");
        eq(Json.parse("-0.0"), -0.0, "negative zero");
        eq(Json.parse("1E2"), 100.0, "exponent is a double");
        eq(Json.parse(" \t\n[ ]\r\n"), new ArrayList<>(), "whitespace and empty array");
        eq(Json.parse("\"\\ud83d\\ude00 \\u00e9\\n\\t\\\"\\\\\\/\\b\\f\\r\""), "\uD83D\uDE00 \u00e9\n\t\"\\/\b\f\r", "escapes and a surrogate pair");
        eq(Json.parse("\"日本 \uD83D\uDE00\""), "日本 \uD83D\uDE00", "raw non-ASCII");
        for (String bad : new String[] {"", "{", "[1,]", "{\"a\" 1}", "01", "1.", "-", "tru", "\"\\x\"", "\"a", "\"\u0001\"", "[1] 2", "{a:1}", "NaN", "\"\\u12\""}) {
            throwsA(InputException.class, () -> Json.parse(bad), "invalid JSON " + bad);
        }
        StringBuilder deep = new StringBuilder();
        for (int i = 0; i < 2000; i++) deep.append('[');
        throwsA(InputException.class, () -> Json.parse(deep.toString()), "too deep");

        eq(Json.stringify(map("s", "q\"b\\n\n\u0001\u001f\u2028\uD83D\uDE00/", "n", null, "t", true)), "{\"s\":\"q\\\"b\\\\n\\n\\u0001\\u001f\u2028\uD83D\uDE00/\",\"n\":null,\"t\":true}", "stringify escapes");
        eq(Json.stringify(Arrays.asList(1, 2L, (short) 3, (byte) 4, 1.5, 0.1f, new BigDecimal("12.50"), new BigInteger("123456789012345678901234567890"))), "[1,2,3,4,1.5,0.1,12.50,123456789012345678901234567890]", "stringify numbers");
        eq(Json.stringify(new int[] {1, 2}), "[1,2]", "primitive array");
        eq(Json.stringify(new Object[] {"a", null}), "[\"a\",null]", "object array");
        eq(Json.stringify(LocalDate.of(2026, 11, 20)), "\"2026-11-20\"", "LocalDate as text");
        eq(Json.stringify(Color.GREEN), "\"GREEN\"", "enum as its name");
        eq(Json.stringify(java.sql.Date.valueOf("2026-11-20")), "\"2026-11-20\"", "java.sql.Date");
        throwsA(InputException.class, () -> Json.stringify(Double.NaN), "NaN");
        throwsA(InputException.class, () -> Json.stringify(Double.POSITIVE_INFINITY), "Infinity");
        throwsA(InputException.class, () -> Json.stringify("\uD83D"), "lone high surrogate");
        throwsA(InputException.class, () -> Json.stringify("x\uDE00"), "lone low surrogate");
        throwsA(InputException.class, () -> Json.stringify(new byte[] {1}), "byte[]");
        throwsA(InputException.class, () -> Json.stringify(new Object()), "unknown object");
        List<Object> cycle = new ArrayList<>();
        cycle.add(cycle);
        throwsA(InputException.class, () -> Json.stringify(cycle), "cycle");
        // Round trip, UTF-8 bytes included.
        Object v = map("emoji", "\uD83D\uDE00", "nested", map("a", Arrays.asList(1L, 2.5, "x", map())), "big", Long.MIN_VALUE);
        byte[] bytes = Json.toBytes(v);
        eq(Json.parse(bytes, 0, bytes.length), v, "round trip");
        eq(new String(Json.toBytes("é\uD83D\uDE00"), StandardCharsets.UTF_8), "\"é\uD83D\uDE00\"", "UTF-8 bytes");
    }

    // ------------------------------------------------------------------ client API

    static void api(String address, String token) {
        // Connecting: a wrong token, bad addresses.
        AuthException auth = throwsA(AuthException.class, () -> KestroClient.connect(address, "wrong-token-0123456789"), "wrong token");
        eq(auth.kind(), "AUTH", "AUTH kind");
        throwsA(InputException.class, () -> KestroClient.connect("tcp://127.0.0.1", token), "address without a port");
        throwsA(InputException.class, () -> KestroClient.connect(" , ", token), "no address");
        ConnectionException refused = throwsA(ConnectionException.class,
                () -> KestroClient.connect("tcp://127.0.0.1:1", token, new KestroClient.Options().timeout(Duration.ofSeconds(5))), "nothing listening");
        ok(refused.getMessage().startsWith("IO cannot connect to tcp://127.0.0.1:1"), "IO message: " + refused.getMessage());

        KestroClient db = KestroClient.connect(address, token);
        eq(db.address(), address, "address()");
        Map<String, Object> st = db.stats();
        eq(st.get("server"), true, "stats().server");
        eq(st.get("tls"), false, "stats().tls");
        ok(st.get("keys") instanceof Long, "stats().keys is a Long");

        // ---- SQL with every parameter type.
        db.execute("CREATE TABLE IF NOT EXISTS typed (id INTEGER PRIMARY KEY, s TEXT, i INTEGER, r REAL, b BOOLEAN, j JSON, d DATE, u TEXT, n TEXT)");
        db.execute("DELETE FROM typed");
        String text = "quote\" back\\slash\nnew\ttab \u0001 é 日本 \uD83D\uDE00 ' -- ;";
        Map<String, Object> doc = map("list", Arrays.asList(1L, "two", 3.5, null, true), "obj", map("k", "v"), "empty", map());
        UUID uuid = UUID.fromString("123e4567-e89b-12d3-a456-426614174000");
        SqlResult ins = db.sql("INSERT INTO typed (s, i, r, b, j, d, u, n) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                text, 42, 3.25, true, doc, LocalDate.of(2026, 11, 20), uuid, null);
        eq(ins.changes(), 1L, "insert changes");
        ok(!ins.hasRows() && ins.rows() == null, "a write has no rows");
        Object id1 = ins.lastInsertId();
        ok(id1 instanceof Long, "lastInsertId is a Long");
        eq(db.lastInsertId(), id1, "client lastInsertId");
        Map<String, Object> row = db.queryOne("SELECT * FROM typed WHERE id = ?", id1);
        eq(row.get("s"), text, "text round trip");
        eq(row.get("i"), 42L, "integer as Long");
        eq(row.get("r"), 3.25, "real as Double");
        eq(row.get("b"), true, "boolean");
        eq(row.get("j"), doc, "JSON column round trip");
        eq(row.get("d"), "2026-11-20", "LocalDate as ISO text");
        eq(row.get("u"), uuid.toString(), "UUID as text");
        ok(row.containsKey("n") ? row.get("n") == null : true, "null");
        eq(new ArrayList<>(row.keySet()).subList(0, 2), Arrays.asList("id", "s"), "columns in table order");
        // Other numeric Java types.
        db.execute("INSERT INTO typed (s, i, r, b) VALUES (?, ?, ?, ?)", "long", Long.valueOf(9007199254740991L), 1.5f, false);
        db.execute("INSERT INTO typed (s, i, r) VALUES (?, ?, ?)", "short", (short) -7, new BigDecimal("12.5"));
        db.execute("INSERT INTO typed (s, i, r) VALUES (?, ?, ?)", "byte", (byte) 8, 0.1);
        db.execute("INSERT INTO typed (s, i, j) VALUES (?, ?, ?)", "bigint", BigInteger.valueOf(-123456789012L), Arrays.asList("a", Color.RED, new int[] {1, 2}));
        eq(db.queryValue("SELECT i FROM typed WHERE s = ?", "long"), 9007199254740991L, "2^53-1 exact");
        eq(db.queryValue("SELECT r FROM typed WHERE s = ?", "long"), 1.5, "float");
        eq(db.queryValue("SELECT b FROM typed WHERE s = ?", "long"), false, "false");
        eq(db.queryValue("SELECT i FROM typed WHERE s = ?", "short"), -7L, "short");
        eq(db.queryValue("SELECT r FROM typed WHERE s = ?", "short"), 12.5, "BigDecimal");
        eq(db.queryValue("SELECT r FROM typed WHERE s = ?", "byte"), 0.1, "0.1 exactly");
        eq(db.queryValue("SELECT i FROM typed WHERE s = ?", "bigint"), -123456789012L, "BigInteger");
        eq(db.queryValue("SELECT j FROM typed WHERE s = ?", "bigint"), Arrays.asList("a", "RED", Arrays.asList(1L, 2L)), "list with enum and array");
        eq(db.queryValue("SELECT COUNT(*) FROM typed WHERE b = ?", true), 1L, "boolean parameter");
        eq(db.queryValue("SELECT COUNT(*) FROM typed WHERE n IS NULL AND s = ?", text), 1L, "null column");
        throwsA(InputException.class, () -> db.execute("INSERT INTO typed (i) VALUES (?)", "not a number"), "declared type enforced");
        throwsA(InputException.class, () -> db.execute("INSERT INTO typed (s) VALUES (?)", Double.NaN), "NaN parameter");
        throwsA(InputException.class, () -> db.execute("INSERT INTO typed (s) VALUES (?)", "\uD800"), "lone surrogate parameter");
        throwsA(InputException.class, () -> db.execute("INSERT INTO typed (s) VALUES (?)", new Object()), "unknown parameter type");

        // ---- Placeholders: ?, ?N, $N, :name, @name; lists and maps.
        db.execute("CREATE TABLE IF NOT EXISTS people (id INTEGER PRIMARY KEY, email TEXT UNIQUE, name TEXT, age INTEGER, city TEXT)");
        db.execute("DELETE FROM people");
        eq(db.execute("INSERT INTO people (email, name, age, city) VALUES (:email, :name, :age, @city)",
                map("email", "a@x.org", ":name", "Ann", "@age", 31, "city", "Kandy")), 1L, "named insert");
        Object ann = db.lastInsertId();
        db.execute("INSERT INTO people (email, name, age, city) VALUES (?, ?, ?, ?)", Arrays.asList("b@x.org", "Bob", 25, "Galle"));
        db.execute("INSERT INTO people (email, name, age, city) VALUES ($2, $1, $3, $4)", "Cy", "c@x.org", 41, "Kandy");
        db.execute("INSERT INTO people (email, name, age, city) VALUES (?2, ?1, ?3, ?3)", "Dee", "d@x.org", null);
        eq(db.queryValue("SELECT name FROM people WHERE id = ?", ann), "Ann", "lastInsertId row");
        List<Map<String, Object>> older = db.query("SELECT name FROM people WHERE age > :min ORDER BY age", map("min", 26));
        eq(older.size(), 2, "query size");
        eq(older.get(0), map("name", "Ann"), "query rows");
        eq(db.queryOne("SELECT name, age FROM people WHERE email = @e", map("e", "b@x.org")), map("name", "Bob", "age", 25L), "queryOne");
        eq(db.queryOne("SELECT * FROM people WHERE id = ?", 999), null, "queryOne none");
        eq(db.queryValue("SELECT COUNT(*) FROM people"), 4L, "queryValue");
        eq(db.queryValue("SELECT COUNT(*) FROM people WHERE city = $1 OR name = $1", "Kandy"), 2L, "$1 twice");
        eq(db.query("SELECT * FROM people WHERE id = ?", Arrays.asList(-1)).size(), 0, "empty result");
        ok(db.sql("SELECT * FROM people WHERE id = -1").hasRows(), "empty result still has rows");
        eq(db.execute("UPDATE people SET age = age + 1 WHERE city = ?", "Kandy"), 2L, "update changes");
        eq(db.execute("DELETE FROM people WHERE email = ?", "zzz"), 0L, "delete none");
        KestroClient.Prepared byCity = db.prepare("SELECT name FROM people WHERE city = ? ORDER BY name");
        eq(byCity.all("Kandy").size(), 2, "prepared all");
        eq(byCity.value("Galle"), "Bob", "prepared value");
        eq(byCity.one("Nowhere"), null, "prepared one");
        eq(db.prepare("SELECT COUNT(*) AS n FROM people WHERE age >= :a").value(map("a", 0)), 3L, "prepared named");
        throwsA(InputException.class, () -> db.query("UPDATE people SET age = 1 WHERE id = -1"), "query() of a write");

        // ---- Errors map to classes.
        ConflictException dup = throwsA(ConflictException.class, () -> db.execute("INSERT INTO people (email) VALUES (?)", "a@x.org"), "duplicate");
        eq(dup.kind(), "CONFLICT", "conflict kind");
        ok(dup instanceof KestroException, "KestroException base");
        InputException bad = throwsA(InputException.class, () -> db.query("SELEC nonsense"), "bad SQL");
        eq(bad.code(), "INPUT", "input code");
        throwsA(InputException.class, () -> db.query("SELECT * FROM people WHERE id = :id", map()), "missing named param");
        throwsA(InputException.class, () -> db.query("SELECT * FROM people WHERE id = ?"), "missing positional param");
        throwsA(InputException.class, () -> db.query("SELECT * FROM people WHERE id = :id AND age = ?", 1, 2), "mixed params");
        throwsA(InputException.class, () -> db.query("SELECT * FROM no_such_table"), "missing table");
        eq(KestroException.of("STALE something").getClass(), KestroException.class, "other codes: base class");
        eq(KestroException.of("STALE something").kind(), "STALE", "other codes: kind");
        eq(KestroException.of("UNAVAILABLE x").getClass(), UnavailableException.class, "UNAVAILABLE class");
        eq(KestroException.of("READONLY x").getClass(), ReadOnlyException.class, "READONLY class");
        eq(KestroException.of("BUSY x").getClass(), BusyException.class, "BUSY class");
        eq(KestroException.of("CORRUPT x").getClass(), CorruptException.class, "CORRUPT class");
        eq(KestroException.of("LIMIT x").getClass(), LimitException.class, "LIMIT class");
        eq(KestroException.of("CLOSED x").getClass(), ClosedException.class, "CLOSED class");
        ok(KestroException.of("CLOSED x") instanceof ConnectionException, "CLOSED is a ConnectionException");
        eq(KestroException.of("weird message").kind(), "ERROR", "no code");
        // The connection still works after errors.
        eq(db.queryValue("SELECT COUNT(*) FROM people"), 4L, "usable after errors");

        // ---- Transactions.
        db.execute("CREATE TABLE IF NOT EXISTS acct (id INTEGER PRIMARY KEY, bal INTEGER)");
        db.execute("DELETE FROM acct");
        db.execute("INSERT INTO acct (id, bal) VALUES (1, 100), (2, 0)");
        KestroClient other = KestroClient.connect(address, token);
        db.begin();
        ok(db.inTransaction(), "begin() opens");
        throwsA(InputException.class, db::begin, "nested begin");
        db.execute("UPDATE acct SET bal = bal - 10 WHERE id = 1");
        eq(db.queryValue("SELECT bal FROM acct WHERE id = 1"), 90L, "reads its own write");
        eq(other.queryValue("SELECT bal FROM acct WHERE id = 1"), 100L, "others do not see it yet");
        throwsA(InputException.class, () -> db.set("kv", "inside", 1), "key/value write inside a SQL transaction");
        ok(db.inTransaction(), "still open after a refused statement");
        db.commit();
        ok(!db.inTransaction(), "commit() closes");
        eq(other.queryValue("SELECT bal FROM acct WHERE id = 1"), 90L, "committed");
        db.commit();   // nothing open: nothing to do
        db.rollback();
        db.begin();
        db.execute("UPDATE acct SET bal = 0 WHERE id = 1");
        db.rollback();
        ok(!db.inTransaction(), "rollback() closes");
        eq(db.queryValue("SELECT bal FROM acct WHERE id = 1"), 90L, "rolled back");
        // As SQL text.
        db.sql("BEGIN");
        ok(db.inTransaction(), "BEGIN as SQL");
        db.execute("UPDATE acct SET bal = 1 WHERE id = 2");
        db.sql("ROLLBACK");
        ok(!db.inTransaction(), "ROLLBACK as SQL");
        db.sql("start transaction");
        ok(db.inTransaction(), "START TRANSACTION");
        db.sql("  commit;");
        ok(!db.inTransaction(), "commit; with spaces");
        // transaction(fn): commit, or rollback and rethrow.
        db.transaction(tx -> {
            tx.execute("UPDATE acct SET bal = bal - 30 WHERE id = 1");
            tx.execute("UPDATE acct SET bal = bal + 30 WHERE id = 2");
        });
        eq(db.queryValue("SELECT SUM(bal) FROM acct"), 90L, "transaction() kept the total");
        eq(db.queryValue("SELECT bal FROM acct WHERE id = 2"), 30L, "transaction() committed");
        RuntimeException boom = throwsA(IllegalStateException.class, () -> db.transaction(tx -> {
            tx.execute("UPDATE acct SET bal = 0");
            throw new IllegalStateException("boom");
        }), "transaction() rethrows");
        eq(boom.getMessage(), "boom", "the body's exception");
        ok(!db.inTransaction(), "rolled back after the exception");
        eq(db.queryValue("SELECT bal FROM acct WHERE id = 2"), 30L, "transaction() rolled back");
        eq(db.transactionResult(tx -> tx.queryValue("SELECT bal FROM acct WHERE id = 1")), 60L, "transactionResult");
        throwsA(ConflictException.class, () -> db.transaction(tx -> {
            tx.execute("INSERT INTO people (email) VALUES (?)", "zz@x.org");
            tx.execute("INSERT INTO people (email) VALUES (?)", "zz@x.org");
        }), "conflict inside transaction()");
        ok(!db.inTransaction(), "closed after a conflict");
        eq(db.queryValue("SELECT COUNT(*) FROM people WHERE email = 'zz@x.org'"), 0L, "nothing of it kept");
        // Savepoints: ROLLBACK TO keeps the transaction; a SAVEPOINT outside one begins it.
        db.begin();
        db.execute("UPDATE acct SET bal = 1000 WHERE id = 1");
        db.sql("SAVEPOINT sp1");
        db.execute("UPDATE acct SET bal = 2000 WHERE id = 1");
        db.sql("ROLLBACK TO SAVEPOINT sp1");
        ok(db.inTransaction(), "ROLLBACK TO keeps the transaction open");
        eq(db.queryValue("SELECT bal FROM acct WHERE id = 1"), 1000L, "ROLLBACK TO undid the later write");
        db.rollback();
        eq(db.queryValue("SELECT bal FROM acct WHERE id = 1"), 60L, "whole rollback");
        db.sql("SAVEPOINT outer_sp");
        ok(db.inTransaction(), "SAVEPOINT outside a transaction begins one");
        db.execute("UPDATE acct SET bal = 61 WHERE id = 1");
        db.sql("RELEASE SAVEPOINT outer_sp");
        ok(!db.inTransaction(), "releasing it commits");
        eq(other.queryValue("SELECT bal FROM acct WHERE id = 1"), 61L, "released savepoint committed");
        // BEGIN CONCURRENT: first committer wins; the loser's COMMIT fails and ends it.
        db.sql("BEGIN CONCURRENT");
        other.sql("BEGIN CONCURRENT");
        db.execute("UPDATE acct SET bal = 1 WHERE id = 2");
        other.execute("UPDATE acct SET bal = 2 WHERE id = 2");
        other.commit();
        ConflictException lost = throwsA(ConflictException.class, db::commit, "second committer loses");
        ok(lost.getMessage().contains("CONFLICT"), "conflict message");
        ok(!db.inTransaction(), "a failed COMMIT ends the transaction");
        eq(db.queryValue("SELECT bal FROM acct WHERE id = 2"), 2L, "first committer's value");

        // ---- Key/value with versions.
        db.delete("kv", "k1");
        long v1 = db.set("kv", "k1", map("n", 1L));
        ok(v1 > 0, "set returns a version");
        Versioned got = db.get("kv", "k1");
        eq(got.version(), v1, "get version");
        eq(got.value(), map("n", 1L), "get value");
        eq(db.get("kv", "missing"), null, "get missing");
        long v2 = db.set("kv", "k1", map("n", 2L), v1);
        ok(v2 > v1, "conditional set at the right version");
        ConflictException stale = throwsA(ConflictException.class, () -> db.set("kv", "k1", map("n", 3L), v1), "stale expectedVersion");
        eq(stale.kind(), "CONFLICT", "stale kind");
        eq(db.get("kv", "k1").value(), map("n", 2L), "not overwritten");
        throwsA(ConflictException.class, () -> db.set("kv", "k1", 1, 0), "expectedVersion 0 on an existing key");
        db.delete("kv", "fresh");
        long vf = db.set("kv", "fresh", "new", 0);
        eq(db.get("kv", "fresh"), new Versioned(vf, "new"), "expectedVersion 0 creates");
        throwsA(ConflictException.class, () -> db.delete("kv", "fresh", vf + 1000), "conditional delete, wrong version");
        db.delete("kv", "fresh", vf);
        eq(db.get("kv", "fresh"), null, "conditional delete");
        // Values of every JSON type.
        Object[] values = {null, true, false, 0L, -1L, Long.MAX_VALUE, Long.MIN_VALUE, 3.5, -1.25e-10, "", "text \uD83D\uDE00 \"q\" \\ \n",
                Arrays.asList(), map(), map("deep", Arrays.asList(map("x", Arrays.asList(1L, null, "y"))))};
        for (int i = 0; i < values.length; i++) {
            db.set("kv", "type" + i, values[i]);
            eq(db.get("kv", "type" + i).value(), values[i], "value round trip " + i);
        }
        // Unicode keys; key and collection rules.
        String ukey = "ключ/日本/\uD83D\uDE00 with spaces";
        db.set("kv", ukey, "unicode key");
        eq(db.get("kv", ukey).value(), "unicode key", "unicode key");
        throwsA(InputException.class, () -> db.get("bad-name", "k"), "invalid collection");
        throwsA(InputException.class, () -> db.get("kv", ""), "empty key");
        throwsA(InputException.class, () -> db.get("kv", "a\0b"), "NUL in key");
        throwsA(InputException.class, () -> db.get("kv", "x".repeat(513)), "key over 512 bytes");
        throwsA(InputException.class, () -> db.get("kv", "\uD800"), "lone surrogate key");
        db.set("kv", "y".repeat(512), 1);
        throwsA(LimitException.class, () -> db.set("kv", "huge", "z".repeat(300 * 1024)), "value over 256 KiB");
        String big = "b".repeat(200 * 1024);
        db.set("kv", "big", big);
        eq(db.get("kv", "big").value(), big, "200 KiB value");
        // getMany: one snapshot, in order, null where missing.
        List<Versioned> many = db.getMany("kv", "k1", "missing", "type3", ukey);
        eq(many.size(), 4, "getMany size");
        eq(many.get(0).value(), map("n", 2L), "getMany 0");
        eq(many.get(1), null, "getMany missing");
        eq(many.get(2).value(), 0L, "getMany 2");
        eq(many.get(3).value(), "unicode key", "getMany unicode");
        eq(db.getMany("kv", new ArrayList<>()).size(), 0, "getMany none");
        List<String> tooMany = new ArrayList<>();
        for (int i = 0; i < 4097; i++) tooMany.add("k" + i);
        throwsA(LimitException.class, () -> db.getMany("kv", tooMany), "getMany over 4096");
        // batch: all or nothing, one version.
        long vb = db.batch(Op.set("kv", "b1", 1), Op.set("kv", "b2", 2), Op.delete("kv", "type0"));
        eq(db.get("kv", "b1").version(), vb, "batch version b1");
        eq(db.get("kv", "b2").version(), vb, "batch version b2");
        eq(db.get("kv", "type0"), null, "batch delete");
        throwsA(ConflictException.class, () -> db.batch(Op.set("kv", "b1", 10), Op.set("kv", "b2", 20, vb - 1)), "batch with a stale version");
        eq(db.get("kv", "b1").value(), 1L, "nothing of a failed batch applied");
        throwsA(InputException.class, () -> db.batch(Op.set("kv", "d", 1), Op.delete("kv", "d")), "duplicate key in a batch");
        throwsA(InputException.class, () -> db.batch(new ArrayList<>()), "empty batch");
        throwsA(InputException.class, () -> Op.set("kv", "x", 1, -5), "negative expectedVersion");
        List<Op> ops = new ArrayList<>();
        for (int i = 0; i < 1000; i++) ops.add(Op.set("bulk", "k" + i, map("i", (long) i)));
        long vbulk = db.batch(ops);
        eq(db.get("bulk", "k999"), new Versioned(vbulk, map("i", 999L)), "1000-op batch");
        // A SQL table's rows through key/value: admin may.
        ok(((Number) db.stats().get("keys")).longValue() > 1000, "stats keys");
        db.verify();
        throwsA(InputException.class, () -> db.backup("x.kdb"), "backup without --backup-dir");
        Map<String, Object> compacted = db.compact();
        ok(compacted.containsKey("before") && compacted.containsKey("after"), "compact()");
        eq(db.get("bulk", "k5").value(), map("i", 5L), "reads after compaction");

        // ---- Threads share one client (calls are serialized).
        AtomicReference<Throwable> error = new AtomicReference<>();
        List<Thread> threads = new ArrayList<>();
        for (int t = 0; t < 8; t++) {
            final int n = t;
            Thread th = new Thread(() -> {
                try {
                    for (int i = 0; i < 50; i++) {
                        db.set("threads", "t" + n + "_" + i, i);
                        Object v = db.get("threads", "t" + n + "_" + i).value();
                        if (!Long.valueOf(i).equals(v)) throw new AssertionError("thread read " + v);
                        db.queryValue("SELECT COUNT(*) FROM people");
                    }
                } catch (Throwable e) {
                    error.compareAndSet(null, e);
                }
            });
            threads.add(th);
            th.start();
        }
        for (Thread th : threads) {
            try { th.join(); } catch (InterruptedException e) { throw new RuntimeException(e); }
        }
        if (error.get() != null) { error.get().printStackTrace(); fail("threads: " + error.get()); }

        // ---- An idle connection is kept (no reconnect when nothing happened).
        int opened = db.opens;
        sleep(1200);
        eq(db.queryValue("SELECT COUNT(*) FROM people"), 4L, "after an idle second");
        eq(db.opens, opened, "the idle connection was kept, not reopened");

        // ---- Closed.
        other.close();
        db.close();
        db.close();   // twice is fine
        ok(db.isClosed(), "isClosed");
        ClosedException closed = throwsA(ClosedException.class, () -> db.get("kv", "k1"), "use after close");
        eq(closed.kind(), "CLOSED", "CLOSED kind");
        try (KestroClient auto = KestroClient.connect(address, token)) {
            eq(auto.get("kv", "k1").value(), map("n", 2L), "try-with-resources");
        }
    }

    // ------------------------------------------------------------------ JDBC

    static void jdbc(String address, String token) throws Exception {
        String hostPort = address.substring("tcp://".length());
        String url = "jdbc:kestrodb://" + hostPort;
        // The driver registers itself through META-INF/services: no Class.forName.
        ok(DriverManager.getDriver(url) != null, "driver found by DriverManager");
        throwsA(SQLInvalidAuthorizationSpecException.class, () -> DriverManager.getConnection(url + "?token=wrong-token-0123456789"), "JDBC wrong token");
        Properties props = new Properties();
        props.setProperty("user", "ignored");
        props.setProperty("password", token);
        try (Connection c = DriverManager.getConnection(url, props)) {
            ok(c.isValid(2), "isValid");
            ok(c.isWrapperFor(KestroClient.class), "isWrapperFor");
            eq(c.unwrap(KestroClient.class).address(), "tcp://" + hostPort, "unwrap to KestroClient");
            eq(c.getMetaData().getDatabaseProductName(), "KestroDB", "metadata product");
            eq(c.getMetaData().getUserName(), "admin", "metadata user");
            try (Statement s = c.createStatement()) {
                s.execute("CREATE TABLE IF NOT EXISTS jt (id INTEGER PRIMARY KEY, name TEXT, n INTEGER, r REAL, ok BOOLEAN, d DATE, ts TIMESTAMP, j JSON)");
                s.executeUpdate("DELETE FROM jt");
                eq(s.executeUpdate("INSERT INTO jt (name, n) VALUES ('plain', 1)"), 1, "Statement.executeUpdate");
                try (ResultSet keys = s.getGeneratedKeys()) {
                    ok(keys.next(), "generated key row");
                    ok(keys.getLong(1) > 0, "generated key");
                    eq(keys.getLong("id"), keys.getLong(1), "generated key by name");
                }
                ok(!s.execute("UPDATE jt SET n = 2"), "execute() of a write is false");
                eq(s.getUpdateCount(), 1, "getUpdateCount");
                ok(s.execute("SELECT * FROM jt"), "execute() of a SELECT is true");
                try (ResultSet rs = s.getResultSet()) { ok(rs.next(), "getResultSet"); }
                throwsA(SQLException.class, () -> s.executeQuery("DELETE FROM jt WHERE id = -1"), "executeQuery of a write");
            }
            try (PreparedStatement p = c.prepareStatement("INSERT INTO jt (name, n, r, ok, d, ts, j) VALUES (?, ?, ?, ?, ?, ?, ?)", Statement.RETURN_GENERATED_KEYS)) {
                p.setString(1, "prep \uD83D\uDE00");
                p.setInt(2, 7);
                p.setDouble(3, 2.5);
                p.setBoolean(4, true);
                p.setDate(5, java.sql.Date.valueOf("2026-11-20"));
                p.setTimestamp(6, java.sql.Timestamp.valueOf("2026-11-20 10:15:30.5"));
                p.setObject(7, map("a", Arrays.asList(1L, 2L)));
                eq(p.executeUpdate(), 1, "prepared insert");
                try (ResultSet keys = p.getGeneratedKeys()) { ok(keys.next() && keys.getLong(1) > 0, "prepared generated key"); }
                p.clearParameters();
                p.setString(1, "nulls");
                p.setNull(2, Types.INTEGER);
                p.setNull(3, Types.DOUBLE);
                p.setNull(4, Types.BOOLEAN);
                p.setNull(5, Types.DATE);
                p.setNull(6, Types.TIMESTAMP);
                throwsA(SQLException.class, p::executeUpdate, "a parameter not set");
                p.setNull(7, Types.OTHER);
                eq(p.executeUpdate(), 1, "nulls insert");
                p.setLong(2, 1L << 40);
                p.setFloat(3, 0.5f);
                p.setBigDecimal(3, new BigDecimal("9.75"));
                p.setShort(2, (short) 3);
                p.setByte(2, (byte) 4);
            }
            try (PreparedStatement q = c.prepareStatement("SELECT id, name, n, r, ok, d, ts, j FROM jt WHERE name = ?")) {
                q.setString(1, "prep \uD83D\uDE00");
                try (ResultSet rs = q.executeQuery()) {
                    ResultSetMetaData md = rs.getMetaData();
                    eq(md.getColumnCount(), 8, "column count");
                    eq(md.getColumnName(2), "name", "column name");
                    eq(md.getColumnType(3), Types.BIGINT, "integer column type");
                    eq(md.getColumnType(4), Types.DOUBLE, "real column type");
                    eq(md.getColumnType(5), Types.BOOLEAN, "boolean column type");
                    eq(md.getColumnType(2), Types.VARCHAR, "text column type");
                    ok(rs.isBeforeFirst(), "before first");
                    ok(rs.next(), "a row");
                    eq(rs.getString("name"), "prep \uD83D\uDE00", "getString");
                    eq(rs.getInt("n"), 7, "getInt");
                    eq(rs.getLong(3), 7L, "getLong");
                    eq(rs.getString("n"), "7", "number as string");
                    eq(rs.getDouble("r"), 2.5, "getDouble");
                    eq(rs.getBigDecimal("r"), new BigDecimal("2.5"), "getBigDecimal");
                    ok(rs.getBoolean("ok"), "getBoolean");
                    eq(rs.getDate("d"), java.sql.Date.valueOf("2026-11-20"), "getDate");
                    eq(rs.getTimestamp("ts"), java.sql.Timestamp.valueOf("2026-11-20 10:15:30.5"), "getTimestamp");
                    eq(rs.getObject("d", LocalDate.class), LocalDate.of(2026, 11, 20), "getObject LocalDate");
                    eq(rs.getString("j"), "{\"a\":[1,2]}", "JSON column as text");
                    eq(rs.getObject("j"), map("a", Arrays.asList(1L, 2L)), "JSON column as Map");
                    eq(rs.getObject("n", Integer.class), 7, "getObject Integer");
                    ok(!rs.wasNull(), "wasNull false");
                    throwsA(SQLException.class, () -> rs.getInt("name"), "text as int");
                    throwsA(SQLException.class, () -> rs.getString("nope"), "unknown column");
                    ok(!rs.next(), "one row");
                }
                q.setString(1, "nulls");
                try (ResultSet rs = q.executeQuery()) {
                    ok(rs.next(), "null row");
                    eq(rs.getInt("n"), 0, "null int is 0");
                    ok(rs.wasNull(), "wasNull");
                    eq(rs.getString("d"), null, "null string");
                    ok(rs.wasNull(), "wasNull string");
                }
            }
            // Batches run in one transaction.
            try (PreparedStatement p = c.prepareStatement("INSERT INTO jt (name, n) VALUES (?, ?)")) {
                for (int i = 0; i < 5; i++) { p.setString(1, "batch" + i); p.setInt(2, i); p.addBatch(); }
                eq(Arrays.toString(p.executeBatch()), "[1, 1, 1, 1, 1]", "executeBatch");
            }
            try (Statement s = c.createStatement(ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY)) {
                s.setMaxRows(3);
                try (ResultSet rs = s.executeQuery("SELECT name FROM jt WHERE name LIKE 'batch%' ORDER BY n")) {
                    ok(rs.last(), "last");
                    eq(rs.getRow(), 3, "maxRows");
                    ok(rs.absolute(1), "absolute");
                    eq(rs.getString(1), "batch0", "scroll");
                    ok(rs.previous() == false && rs.isBeforeFirst(), "previous");
                }
            }
            // Auto-commit off: commit and rollback.
            c.setAutoCommit(false);
            KestroClient raw = c.unwrap(KestroClient.class);
            try (PreparedStatement p = c.prepareStatement("UPDATE jt SET n = ? WHERE name = 'plain'")) {
                p.setInt(1, 100);
                p.executeUpdate();
                ok(raw.inTransaction(), "auto-commit off opened a transaction");
                c.rollback();
                p.setInt(1, 200);
                p.executeUpdate();
                c.commit();
            }
            ok(!raw.inTransaction(), "committed");
            c.setAutoCommit(true);
            try (Statement s = c.createStatement(); ResultSet rs = s.executeQuery("SELECT n FROM jt WHERE name = 'plain'")) {
                ok(rs.next(), "row");
                eq(rs.getInt(1), 200, "rollback then commit");
            }
            // Errors as SQLExceptions with SQLState and the KestroException as cause.
            try (Statement s = c.createStatement()) {
                s.execute("CREATE TABLE IF NOT EXISTS ju (id INTEGER PRIMARY KEY, email TEXT UNIQUE)");
                s.executeUpdate("DELETE FROM ju");
                s.executeUpdate("INSERT INTO ju (email) VALUES ('x@y')");
                SQLIntegrityConstraintViolationException dup = throwsA(SQLIntegrityConstraintViolationException.class, () -> s.executeUpdate("INSERT INTO ju (email) VALUES ('x@y')"), "JDBC duplicate");
                eq(dup.getSQLState(), "23000", "SQLState 23000");
                ok(dup.getCause() instanceof ConflictException, "cause is the KestroException");
                throwsA(SQLSyntaxErrorException.class, () -> s.executeQuery("SELEC 1"), "JDBC bad SQL");
                s.addBatch("INSERT INTO ju (email) VALUES ('b1')");
                s.addBatch("INSERT INTO ju (email) VALUES ('x@y')");
                BatchUpdateException be = throwsA(BatchUpdateException.class, s::executeBatch, "failing batch");
                eq(be.getUpdateCounts().length, 1, "counts before the failure");
                try (ResultSet rs = s.executeQuery("SELECT COUNT(*) AS n FROM ju")) { rs.next(); eq(rs.getInt("n"), 1, "a failed batch rolled back"); }
            }
            throwsA(java.sql.SQLFeatureNotSupportedException.class, () -> c.prepareCall("x"), "unsupported call");
        }
    }

    // ------------------------------------------------------------------ reconnect

    static void reconnect(String address, String token) throws Exception {
        KestroClient db = KestroClient.connect(address, token);
        db.execute("CREATE TABLE IF NOT EXISTS rc (id INTEGER PRIMARY KEY, v TEXT)");
        db.execute("INSERT INTO rc (v) VALUES (?)", "before");
        db.set("rkv", "k", "before");
        KestroClient tx = KestroClient.connect(address, token);
        KestroClient off = KestroClient.connect(address, token, new KestroClient.Options().reconnect(false));
        off.stats();
        Connection jdbc = DriverManager.getConnection("jdbc:kestrodb://" + address.substring(6) + "?token=" + token);
        System.out.println("READY");
        System.out.flush();
        readLine();          // the server restarts meanwhile
        sleep(1500);         // idle long enough for the check before the next request
        int opened = db.opens;
        db.execute("INSERT INTO rc (v) VALUES (?)", "after");
        eq(db.opens, opened + 1, "reopened once");
        eq(db.queryValue("SELECT COUNT(*) FROM rc"), 2L, "rows after reconnect");
        eq(db.get("rkv", "k").value(), "before", "key/value after reconnect");
        // A transaction cannot survive its connection: the next call says so, the one after reconnects.
        tx.begin();
        ok(tx.inTransaction(), "transaction on the new server");
        tx.execute("INSERT INTO rc (v) VALUES (?)", "in tx");
        System.out.println("READY2");
        System.out.flush();
        readLine();          // restarted again, with tx's transaction open
        ConnectionException lostTx = throwsA(ConnectionException.class, () -> tx.execute("INSERT INTO rc (v) VALUES (?)", "x"), "call inside a lost transaction");
        ok(lostTx.getMessage().startsWith("IO connection lost inside a transaction; it was rolled back"), "lost transaction message: " + lostTx.getMessage());
        ok(!tx.inTransaction(), "no longer in a transaction");
        eq(tx.queryValue("SELECT COUNT(*) FROM rc"), 2L, "the next call reconnects; the transaction's row is gone");
        // reconnect(false): the dead connection fails with IO, and stays failed.
        ConnectionException offErr = throwsA(ConnectionException.class, off::stats, "reconnect off");
        eq(offErr.kind(), "IO", "IO kind");
        throwsA(ConnectionException.class, off::stats, "reconnect off, again");
        // JDBC reconnects the same way.
        try (Statement s = jdbc.createStatement(); ResultSet rs = s.executeQuery("SELECT COUNT(*) AS n FROM rc")) {
            ok(rs.next(), "JDBC row after restart");
            eq(rs.getLong("n"), 2L, "JDBC count after restart");
        }
        jdbc.close();
        db.close();
        tx.close();
        off.close();
    }

    // ------------------------------------------------------------------ busy

    static void busy(String address, String token, int max) {
        List<KestroClient> held = new ArrayList<>();
        for (int i = 0; i < max; i++) {
            KestroClient c = KestroClient.connect(address, token);
            c.stats();
            held.add(c);
        }
        // The server answers BUSY and closes at once; the client's login frame, still unread,
        // can make that close a reset that discards the BUSY reply (then: IO). Every attempt
        // must fail, and BUSY must be seen.
        BusyException busy = null;
        for (int i = 0; i < 20 && busy == null; i++) {
            try {
                KestroClient.connect(address, token).stats();
                fail("a connection over --max-connections was accepted");
            } catch (BusyException e) {
                busy = e;
            } catch (ConnectionException e) {
                ok(e.getMessage().contains("before login") || e.getMessage().contains("connection lost"), "over the limit: " + e.getMessage());
            }
        }
        // Deterministic: a client that has sent nothing yet reads the server's BUSY before its
        // first request (a client without a token sends nothing when it connects).
        KestroClient quiet = KestroClient.connect(address);
        sleep(300);
        BusyException waited = throwsA(BusyException.class, quiet::stats, "BUSY waiting before the first request");
        quiet.close();
        if (busy == null) busy = waited;
        eq(busy.kind(), "BUSY", "BUSY kind");
        ok(busy.getMessage().startsWith("BUSY too many connections"), "BUSY message: " + busy.getMessage());
        held.remove(0).close();
        KestroClient again = null;
        for (int i = 0; i < 50 && again == null; i++) {
            try { again = KestroClient.connect(address, token); again.stats(); } catch (BusyException | ConnectionException e) { again = null; sleep(100); }
        }
        ok(again != null, "a slot freed by close()");
        again.close();
        for (KestroClient c : held) c.close();
    }

    // ------------------------------------------------------------------ TLS

    static void tls(String address, String token, String ca, String wrongCa) throws Exception {
        int port = Integer.parseInt(address.substring(address.lastIndexOf(':') + 1));
        KestroClient.Options opts = new KestroClient.Options().caFile(ca);
        KestroClient db = KestroClient.connect(address, token, opts);
        eq(db.stats().get("tls"), true, "stats().tls");
        db.execute("CREATE TABLE IF NOT EXISTS tl (id INTEGER PRIMARY KEY, body TEXT)");
        String big = "t".repeat(300_000);   // many TLS records each way
        db.execute("INSERT INTO tl (body) VALUES (?)", big);
        eq(((String) db.queryValue("SELECT body FROM tl WHERE id = ?", db.lastInsertId())).length(), 300_000, "300 KB over TLS");
        long v = db.set("kv", "tls", map("ok", true));
        eq(db.get("kv", "tls"), new Versioned(v, map("ok", true)), "key/value over TLS");
        int opened = db.opens;
        sleep(1200);
        eq(db.get("kv", "tls").version(), v, "after an idle second");
        eq(db.opens, opened, "an idle TLS connection is kept");
        // The certificate names localhost and 127.0.0.1.
        try (KestroClient ip = KestroClient.connect("tls://127.0.0.1:" + port, token, opts)) { eq(ip.stats().get("tls"), true, "IP address SAN"); }
        ConnectionException wrong = throwsA(ConnectionException.class, () -> KestroClient.connect(address, token, new KestroClient.Options().caFile(wrongCa)), "wrong CA");
        ok(wrong.getMessage().contains("TLS handshake failed"), "handshake message: " + wrong.getMessage());
        throwsA(ConnectionException.class, () -> KestroClient.connect(address, token), "JVM trust store does not know the test CA");
        throwsA(ConnectionException.class, () -> KestroClient.connect(address, token, new KestroClient.Options().caFile(ca).serverName("other.example")), "wrong host name");
        try (KestroClient insecure = KestroClient.connect(address, token, new KestroClient.Options().verify(false))) { eq(insecure.stats().get("tls"), true, "verify(false)"); }
        throwsA(ConnectionException.class, () -> KestroClient.connect("tcp://localhost:" + port, token, new KestroClient.Options().timeout(Duration.ofSeconds(5))).stats(), "plain TCP to a TLS port");
        try (Connection c = DriverManager.getConnection("jdbc:kestrodb:tls://localhost:" + port + "?token=" + token + "&caFile=" + java.net.URLEncoder.encode(ca, StandardCharsets.UTF_8));
             Statement s = c.createStatement(); ResultSet rs = s.executeQuery("SELECT COUNT(*) AS n FROM tl")) {
            ok(rs.next() && rs.getLong("n") >= 1, "JDBC over TLS");
        }
        System.out.println("READY");
        System.out.flush();
        readLine();          // the TLS server restarts
        sleep(1500);
        eq(db.get("kv", "tls").value(), map("ok", true), "TLS reconnect after a restart");
        eq(db.opens, opened + 1, "reopened once");
        db.close();
    }

    // ------------------------------------------------------------------ cluster

    static void cluster(String list, String token, String follower) throws Exception {
        // Only the follower: a write is refused with READONLY naming the leader; reads work.
        try (KestroClient f = KestroClient.connect(follower, token)) {
            ReadOnlyException ro = throwsA(ReadOnlyException.class, () -> f.execute("INSERT INTO t (v) VALUES ('x')"), "a follower refuses writes");
            eq(ro.kind(), "READONLY", "READONLY kind");
            ok(ro.getMessage().contains("the leader is"), "names the leader: " + ro.getMessage());
            f.queryValue("SELECT COUNT(*) FROM t");
        }
        KestroClient db = KestroClient.connect(list, token, new KestroClient.Options().timeout(Duration.ofSeconds(3)).failoverTimeout(Duration.ofSeconds(25)));
        eq(db.address(), follower, "starts at the first member");
        for (int i = 0; i < 5; i++) db.execute("INSERT INTO t (v) VALUES (?)", "a" + i);
        long v = db.set("ckv", "k", "v1");
        eq(db.get("ckv", "k").version(), v, "key/value at the leader");
        System.out.println("ADDRESS " + db.address());
        try (Connection c = DriverManager.getConnection("jdbc:kestrodb://" + list.replace("tcp://", "") + "?token=" + token + "&timeout=3&failoverTimeout=25");
             Statement s = c.createStatement()) {
            eq(s.executeUpdate("INSERT INTO t (v) VALUES ('jdbc')"), 1, "JDBC follows the leader");
            eq(c.unwrap(KestroClient.class).address(), db.address(), "JDBC at the leader");
        }
        System.out.println("READY");
        System.out.flush();
        readLine();          // the leader is killed and a new one elected
        sleep(1500);
        for (int i = 0; i < 5; i++) db.execute("INSERT INTO t (v) VALUES (?)", "b" + i);
        eq(db.get("ckv", "k").value(), "v1", "key/value survived the failover");
        System.out.println("ADDRESS " + db.address());
        db.close();
    }

    static void clusterNew(String list, String token) {
        KestroClient db = KestroClient.connect(list, token, new KestroClient.Options().timeout(Duration.ofSeconds(3)).failoverTimeout(Duration.ofSeconds(25)));
        for (int i = 0; i < 5; i++) db.execute("INSERT INTO t (v) VALUES (?)", "c" + i);
        System.out.println("ADDRESS " + db.address());
        db.close();
    }

}

// The .NET client against a real kestro-server. Exits non-zero on the first failure.
//
//   dotnet KestroDB.Tests.dll api ADDRESS TOKEN
//   ... reconnect ADDRESS TOKEN          survives server restarts (waits for lines on stdin)
//   ... busy ADDRESS TOKEN MAX            the server was started with --max-connections MAX
//   ... tls ADDRESS TOKEN CA WRONG_CA     ADDRESS is tls://localhost:PORT (waits for a restart too)
//   ... cluster LIST TOKEN FOLLOWER       follows the leader; waits for the leader to be killed
//   ... cluster-new LIST TOKEN            a new client whose first member is dead
using System;
using System.Collections.Generic;
using System.Data;
using System.Data.Common;
using System.Dynamic;
using System.Linq;
using System.Numerics;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Dapper;
using KestroDB;

namespace KestroDB.Tests;

public sealed record Person(long Id, string Email, string? Name, int? Age, string? City);

public sealed class JtRow
{
    public long Id { get; set; }
    public string? Name { get; set; }
    public int? N { get; set; }
    public double? R { get; set; }
    public bool? Ok { get; set; }
    public DateTime? D { get; set; }
    public string? J { get; set; }
}

public sealed class Profile
{
    public string Name { get; set; } = "";
    public List<string> Tags { get; set; } = new();
    public DateOnly At { get; set; }
}

public static class Program
{
    private static int s_checks;

    private static void Fail(string what)
    {
        Console.Out.Flush();
        Console.Error.WriteLine("FAIL " + what);
        Console.Error.Flush();
        Environment.Exit(1);
    }

    private static void Eq(object? actual, object? expected, string what)
    {
        Interlocked.Increment(ref s_checks);
        if (!Same(actual, expected)) Fail(what + ": got " + Show(actual) + " expected " + Show(expected));
    }

    private static bool Same(object? a, object? b)
    {
        if (a is System.Collections.IEnumerable && b is System.Collections.IEnumerable && a is not string && b is not string
            && a is not Row && b is not Row && a is not System.Collections.IDictionary && b is not System.Collections.IDictionary)
            return ListEquals(a, b);
        return Row.DeepEquals(a, b);
    }

    private static bool ListEquals(object a, object b)
    {
        var la = ((System.Collections.IEnumerable)a).Cast<object?>().ToList();
        var lb = ((System.Collections.IEnumerable)b).Cast<object?>().ToList();
        return la.Count == lb.Count && la.Zip(lb).All(p => Same(p.First, p.Second));
    }

    private static void Ok(bool cond, string what)
    {
        Interlocked.Increment(ref s_checks);
        if (!cond) Fail(what);
    }

    private static string Show(object? v)
    {
        if (v is null) return "null";
        try { return Json.Stringify(v) + " (" + v.GetType().Name + ")"; }
        catch (Exception) { return v + " (" + v.GetType().Name + ")"; }
    }

    private static E Throws<E>(Action body, string what) where E : Exception
    {
        Interlocked.Increment(ref s_checks);
        try
        {
            body();
        }
        catch (Exception e)
        {
            if (e is E match) return match;
            Fail(what + ": threw " + e.GetType().FullName + ": " + e.Message + ", expected " + typeof(E).Name + "\n" + e);
        }
        Fail(what + ": no exception, expected " + typeof(E).Name);
        return null!;
    }

    private static async Task<E> ThrowsAsync<E>(Func<Task> body, string what) where E : Exception
    {
        Interlocked.Increment(ref s_checks);
        try
        {
            await body();
        }
        catch (Exception e)
        {
            if (e is E match) return match;
            Fail(what + ": threw " + e.GetType().FullName + ": " + e.Message + ", expected " + typeof(E).Name + "\n" + e);
        }
        Fail(what + ": no exception, expected " + typeof(E).Name);
        return null!;
    }

    private static Row M(params object?[] kv)
    {
        var m = new Row();
        for (int i = 0; i < kv.Length; i += 2) m[(string)kv[i]!] = kv[i + 1];
        return m;
    }

    private static List<object?> L(params object?[] items) => new(items);

    private static string? ReadLine() => Console.In.ReadLine();

    private static void Say(string line)
    {
        Console.WriteLine(line);
        Console.Out.Flush();
    }

    public static async Task<int> Main(string[] args)
    {
        string mode = args.Length > 0 ? args[0] : "";
        try
        {
            switch (mode)
            {
                case "json": JsonTests(); break;
                case "api": JsonTests(); await Api(args[1], args[2]); await Ado(args[1], args[2]); break;
                case "reconnect": await Reconnect(args[1], args[2]); break;
                case "busy": await Busy(args[1], args[2], int.Parse(args[3])); break;
                case "tls": await Tls(args[1], args[2], args[3], args[4]); break;
                case "cluster": await Cluster(args[1], args[2], args[3]); break;
                case "cluster-new": await ClusterNew(args[1], args[2]); break;
                default: Fail("unknown mode " + mode); break;
            }
        }
        catch (Exception e)
        {
            Fail("unexpected exception: " + e);
        }
        Say("ok dotnet " + mode + " (" + s_checks + " checks)");
        return 0;
    }

    // ------------------------------------------------------------------ JSON

    private enum Color { Red, Green }

    private static void JsonTests()
    {
        Eq(Json.Parse("{\"a\":1,\"b\":[true,false,null],\"c\":-2.5e3,\"d\":\"x\"}"), M("a", 1L, "b", L(true, false, null), "c", -2500.0, "d", "x"), "parse object");
        Eq(Json.Parse("9223372036854775807"), long.MaxValue, "long max");
        Eq(Json.Parse("-9223372036854775808"), long.MinValue, "long min");
        Eq(Json.Parse("9223372036854775808"), BigInteger.Parse("9223372036854775808"), "beyond long");
        Eq(Json.Parse("0"), 0L, "zero");
        Ok(Json.Parse("-0.0") is double z && z == 0 && double.IsNegative(z), "negative zero");
        Eq(Json.Parse("1E2"), 100.0, "exponent is a double");
        Eq(Json.Parse(" \t\n[ ]\r\n"), L(), "whitespace and empty array");
        Eq(Json.Parse("\"\\ud83d\\ude00 \\u00e9\\n\\t\\\"\\\\\\/\\b\\f\\r\""), "\uD83D\uDE00 \u00e9\n\t\"\\/\b\f\r", "escapes and a surrogate pair");
        Eq(Json.Parse("\"日本 \uD83D\uDE00\""), "日本 \uD83D\uDE00", "raw non-ASCII");
        var order = (Row)Json.Parse("{\"z\":1,\"a\":2,\"m\":3}")!;
        Eq(order.Keys, new[] { "z", "a", "m" }, "object keys keep their order");
        foreach (var bad in new[] { "", "{", "[1,]", "{\"a\" 1}", "01", "1.", "-", "tru", "\"\\x\"", "\"a", "\"\u0001\"", "[1] 2", "{a:1}", "NaN", "\"\\u12\"" })
            Throws<InputException>(() => Json.Parse(bad), "invalid JSON " + bad);
        Throws<InputException>(() => Json.Parse(new string('[', 2000)), "too deep");

        Eq(Json.Stringify(M("s", "q\"b\\n\n\u0001\u001f\u2028\uD83D\uDE00/", "n", null, "t", true)), "{\"s\":\"q\\\"b\\\\n\\n\\u0001\\u001f\u2028\uD83D\uDE00/\",\"n\":null,\"t\":true}", "stringify escapes");
        Eq(Json.Stringify(new object[] { 1, 2L, (short)3, (byte)4, 1.5, 0.1f, 12.50m, BigInteger.Parse("123456789012345678901234567890"), (sbyte)-5, 7u, 8ul, (ushort)9 }), "[1,2,3,4,1.5,0.1,12.50,123456789012345678901234567890,-5,7,8,9]", "stringify numbers");
        Eq(Json.Stringify(new[] { 1, 2 }), "[1,2]", "int array");
        Eq(Json.Stringify(new object?[] { "a", null }), "[\"a\",null]", "object array");
        Eq(Json.Stringify(new DateOnly(2026, 11, 20)), "\"2026-11-20\"", "DateOnly");
        Eq(Json.Stringify(new DateTime(2026, 11, 20, 10, 15, 30, 500)), "\"2026-11-20T10:15:30.5\"", "DateTime");
        Eq(Json.Stringify(new DateTime(2026, 11, 20, 0, 0, 0, DateTimeKind.Utc)), "\"2026-11-20T00:00:00Z\"", "DateTime UTC");
        Eq(Json.Stringify(new DateTimeOffset(2026, 11, 20, 10, 0, 0, TimeSpan.FromHours(5.5))), "\"2026-11-20T10:00:00+05:30\"", "DateTimeOffset");
        Eq(Json.Stringify(new TimeOnly(9, 30)), "\"09:30:00\"", "TimeOnly");
        Eq(Json.Stringify(Color.Green), "\"Green\"", "enum as its name");
        Eq(Json.Stringify(Guid.Parse("123e4567-e89b-12d3-a456-426614174000")), "\"123e4567-e89b-12d3-a456-426614174000\"", "Guid");
        Eq(Json.Stringify(new { a = 1, b = new[] { "x" }, c = (string?)null }), "{\"a\":1,\"b\":[\"x\"],\"c\":null}", "anonymous object");
        Eq(Json.Stringify(new Person(1, "e", "n", null, "c")), "{\"Id\":1,\"Email\":\"e\",\"Name\":\"n\",\"Age\":null,\"City\":\"c\"}", "record");
        Eq(Json.Stringify(new Dictionary<string, int> { ["x"] = 1 }), "{\"x\":1}", "dictionary");
        Eq(Json.Stringify(JsonDocument.Parse("{\"k\": [1, 2]}").RootElement), "{\"k\": [1, 2]}", "JsonElement as it is");
        Eq(Json.Stringify(JsonNode.Parse("{\"k\":true}")), "{\"k\":true}", "JsonNode");
        dynamic expando = new ExpandoObject();
        expando.name = "x";
        Eq(Json.Stringify((object)expando), "{\"name\":\"x\"}", "ExpandoObject");
        Throws<InputException>(() => Json.Stringify(double.NaN), "NaN");
        Throws<InputException>(() => Json.Stringify(double.PositiveInfinity), "Infinity");
        Throws<InputException>(() => Json.Stringify(float.NaN), "float NaN");
        Throws<InputException>(() => Json.Stringify("\uD83D"), "lone high surrogate");
        Throws<InputException>(() => Json.Stringify("x\uDE00"), "lone low surrogate");
        Throws<InputException>(() => Json.Stringify(new byte[] { 1 }), "byte[]");
        Throws<InputException>(() => Json.Stringify(new object()), "unknown object");
        var cycle = new List<object>();
        cycle.Add(cycle);
        Throws<InputException>(() => Json.Stringify(cycle), "cycle");
        // Round trip, UTF-8 bytes included.
        object v = M("emoji", "\uD83D\uDE00", "nested", M("a", L(1L, 2.5, "x", M())), "big", long.MinValue);
        Eq(Json.Parse(Json.ToBytes(v)), v, "round trip");
        Eq(System.Text.Encoding.UTF8.GetString(Json.ToBytes("é\uD83D\uDE00")), "\"é\uD83D\uDE00\"", "UTF-8 bytes");
        // Conversions.
        Eq(Json.Convert<int>(5L), 5, "long to int");
        Eq(Json.Convert<int?>(null), null, "null to int?");
        Eq(Json.Convert<Guid>("123e4567-e89b-12d3-a456-426614174000"), Guid.Parse("123e4567-e89b-12d3-a456-426614174000"), "text to Guid");
        Eq(Json.Convert<Color>("green"), Color.Green, "text to enum");
        Eq(Json.Convert<double>(3L), 3.0, "long to double");
        Throws<InvalidCastException>(() => Json.Convert<int>(5_000_000_000L), "overflow");
        Throws<InvalidCastException>(() => Json.Convert<long>(2.5), "fraction");
        Eq(Json.Convert<Profile>(M("name", "N", "tags", L("a"), "at", "2026-11-20"))!.At, new DateOnly(2026, 11, 20), "row to a class");
    }

    // ------------------------------------------------------------------ client API

    private static async Task Api(string address, string token)
    {
        var opts = new KestroOptions { Token = token };
        // Connecting: a wrong token, bad addresses.
        var auth = await ThrowsAsync<AuthException>(() => KestroClient.ConnectAsync(address, new KestroOptions { Token = "wrong-token-0123456789" }), "wrong token");
        Eq(auth.Kind, "AUTH", "AUTH kind");
        Ok(auth is DbException { SqlState: "28000" }, "a DbException with SQLSTATE 28000");
        Throws<AuthException>(() => KestroClient.Connect(address, new KestroOptions { Token = "wrong-token-0123456789" }), "wrong token (blocking)");
        await ThrowsAsync<InputException>(() => KestroClient.ConnectAsync("tcp://127.0.0.1", opts), "address without a port");
        await ThrowsAsync<InputException>(() => KestroClient.ConnectAsync(" , ", opts), "no address");
        await ThrowsAsync<InputException>(() => KestroClient.ConnectAsync("tcp://127.0.0.1:99999", opts), "port out of range");
        var refused = await ThrowsAsync<KestroIOException>(
            () => KestroClient.ConnectAsync("tcp://127.0.0.1:1", new KestroOptions { Token = token, Timeout = TimeSpan.FromSeconds(5) }), "nothing listening");
        Ok(refused.Message.StartsWith("IO cannot connect to tcp://127.0.0.1:1", StringComparison.Ordinal), "IO message: " + refused.Message);
        Ok(refused.IsTransient, "IO is transient");
        var refusedSync = Throws<KestroIOException>(() => KestroClient.Connect("tcp://127.0.0.1:1", new KestroOptions { Token = token, Timeout = TimeSpan.FromSeconds(5) }), "nothing listening (blocking)");
        Ok(refusedSync.Message.StartsWith("IO cannot connect to tcp://127.0.0.1:1", StringComparison.Ordinal), "IO message (blocking): " + refusedSync.Message);

        await using var db = await KestroClient.ConnectAsync(address, opts);
        Eq(db.Address, address, "Address");
        var st = await db.StatsAsync();
        Eq(st["server"], true, "stats server");
        Eq(st["tls"], false, "stats tls");
        Ok(st["keys"] is long, "stats keys is a long");

        // ---- SQL with every parameter type.
        await db.ExecuteAsync("CREATE TABLE IF NOT EXISTS typed (id INTEGER PRIMARY KEY, s TEXT, i INTEGER, r REAL, b BOOLEAN, j JSON, d DATE, u TEXT, n TEXT, ts TIMESTAMP, e TEXT)");
        await db.ExecuteAsync("DELETE FROM typed");
        string text = "quote\" back\\slash\nnew\ttab \u0001 é 日本 \uD83D\uDE00 ' -- ;";
        Row doc = M("list", L(1L, "two", 3.5, null, true), "obj", M("k", "v"), "empty", M());
        var uuid = Guid.Parse("123e4567-e89b-12d3-a456-426614174000");
        var ins = await db.SqlAsync("INSERT INTO typed (s, i, r, b, j, d, u, n, ts, e) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            text, 42, 3.25, true, doc, new DateOnly(2026, 11, 20), uuid, null, new DateTime(2026, 11, 20, 10, 15, 30, 500), DayOfWeek.Friday);
        Eq(ins.Changes, 1L, "insert changes");
        Ok(!ins.HasRows && ins.Rows is null, "a write has no rows");
        object? id1 = ins.LastInsertId;
        Ok(id1 is long, "LastInsertId is a long");
        Eq(db.LastInsertId, id1, "client LastInsertId");
        Row row = (await db.QueryOneAsync("SELECT * FROM typed WHERE id = ?", id1))!;
        Eq(row["s"], text, "text round trip");
        Eq(row["i"], 42L, "integer as long");
        Eq(row["r"], 3.25, "real as double");
        Eq(row["b"], true, "boolean");
        Eq(row["j"], doc, "JSON column round trip");
        Eq(row["d"], "2026-11-20", "DateOnly as ISO text");
        Eq(row["u"], uuid.ToString(), "Guid as text");
        Eq(row["n"], null, "null");
        Eq(row["ts"], "2026-11-20T10:15:30.5", "DateTime as ISO text");
        Eq(row["e"], "Friday", "enum as its name");
        Eq(row.Keys.Take(2).ToList(), new[] { "id", "s" }, "columns in table order");
        Eq(row[0], id1, "value by position");
        Eq(row.Get<Guid>("u"), uuid, "Get<Guid>");
        Eq(row.Get<DateOnly>("d"), new DateOnly(2026, 11, 20), "Get<DateOnly>");
        Eq(row.Get<DateTime>("ts"), new DateTime(2026, 11, 20, 10, 15, 30, 500), "Get<DateTime>");
        Eq(row.Get<int>("i"), 42, "Get<int>");
        Eq(row.Get<DayOfWeek>("e"), DayOfWeek.Friday, "Get<enum>");
        // Other numeric .NET types.
        await db.ExecuteAsync("INSERT INTO typed (s, i, r, b) VALUES (?, ?, ?, ?)", "long", 9007199254740991L, 1.5f, false);
        await db.ExecuteAsync("INSERT INTO typed (s, i, r) VALUES (?, ?, ?)", "short", (short)-7, 12.5m);
        await db.ExecuteAsync("INSERT INTO typed (s, i, r) VALUES (?, ?, ?)", "byte", (byte)8, 0.1);
        await db.ExecuteAsync("INSERT INTO typed (s, i, j) VALUES (?, ?, ?)", "bigint", new BigInteger(-123456789012L), new object[] { "a", Color.Red, new[] { 1, 2 } });
        await db.ExecuteAsync("INSERT INTO typed (s, i, r) VALUES (?, ?, ?)", "unsigned", 4000000000u, (Half)0.5);
        Eq(await db.QueryValueAsync("SELECT i FROM typed WHERE s = ?", "long"), 9007199254740991L, "2^53-1 exact");
        Eq(await db.QueryValueAsync("SELECT r FROM typed WHERE s = ?", "long"), 1.5, "float");
        Eq(await db.QueryValueAsync("SELECT b FROM typed WHERE s = ?", "long"), false, "false");
        Eq(await db.QueryValueAsync("SELECT i FROM typed WHERE s = ?", "short"), -7L, "short");
        Eq(await db.QueryValueAsync("SELECT r FROM typed WHERE s = ?", "short"), 12.5, "decimal");
        Eq(await db.QueryValueAsync("SELECT r FROM typed WHERE s = ?", "byte"), 0.1, "0.1 exactly");
        Eq(await db.QueryValueAsync("SELECT i FROM typed WHERE s = ?", "bigint"), -123456789012L, "BigInteger");
        Eq(await db.QueryValueAsync("SELECT j FROM typed WHERE s = ?", "bigint"), L("a", "Red", L(1L, 2L)), "list with enum and array");
        Eq(await db.QueryValueAsync("SELECT i FROM typed WHERE s = ?", "unsigned"), 4000000000L, "uint");
        Eq(await db.QueryValueAsync("SELECT r FROM typed WHERE s = ?", "unsigned"), 0.5, "Half");
        Eq(await db.QueryValueAsync("SELECT COUNT(*) FROM typed WHERE b = ?", true), 1L, "boolean parameter");
        Eq(await db.QueryValueAsync("SELECT COUNT(*) FROM typed WHERE n IS NULL AND s = ?", text), 1L, "null column");
        Eq(await db.QueryValueAsync<int>("SELECT COUNT(*) FROM typed"), 6, "QueryValueAsync<int>");
        Eq(await db.QueryValueAsync("SELECT i FROM typed WHERE s = ?", new object?[] { "short" }), -7L, "an object[] is the positional list");
        await ThrowsAsync<InputException>(() => db.ExecuteAsync("INSERT INTO typed (i) VALUES (?)", "not a number"), "declared type enforced");
        await ThrowsAsync<InputException>(() => db.ExecuteAsync("INSERT INTO typed (s) VALUES (?)", double.NaN), "NaN parameter");
        await ThrowsAsync<InputException>(() => db.ExecuteAsync("INSERT INTO typed (s) VALUES (?)", "\uD800"), "lone surrogate parameter");
        await ThrowsAsync<InputException>(() => db.ExecuteAsync("INSERT INTO typed (s) VALUES (?)", new object()), "unknown parameter type");
        await ThrowsAsync<InputException>(() => db.ExecuteAsync("INSERT INTO typed (s) VALUES (?)", new byte[] { 1, 2 }), "byte[] parameter");
        await ThrowsAsync<InputException>(() => db.ExecuteAsync("SELECT 1 -- \uDC00"), "lone surrogate in SQL text");

        // ---- Placeholders: ?, ?N, $N, :name, @name; lists, dictionaries, anonymous objects.
        await db.ExecuteAsync("CREATE TABLE IF NOT EXISTS people (id INTEGER PRIMARY KEY, email TEXT UNIQUE, name TEXT, age INTEGER, city TEXT)");
        await db.ExecuteAsync("DELETE FROM people");
        Eq(await db.ExecuteAsync("INSERT INTO people (email, name, age, city) VALUES (:email, :name, :age, @city)",
            new Dictionary<string, object?> { ["email"] = "a@x.org", [":name"] = "Ann", ["@age"] = 31, ["city"] = "Kandy" }), 1L, "named insert (dictionary)");
        object? ann = db.LastInsertId;
        await db.ExecuteAsync("INSERT INTO people (email, name, age, city) VALUES (?, ?, ?, ?)", new List<object> { "b@x.org", "Bob", 25, "Galle" });
        await db.ExecuteAsync("INSERT INTO people (email, name, age, city) VALUES ($2, $1, $3, $4)", "Cy", "c@x.org", 41, "Kandy");
        await db.ExecuteAsync("INSERT INTO people (email, name, age, city) VALUES (?2, ?1, ?3, ?3)", "Dee", "d@x.org", null);
        Eq(await db.QueryValueAsync("SELECT name FROM people WHERE id = ?", ann), "Ann", "LastInsertId row");
        var older = await db.QueryAsync("SELECT name FROM people WHERE age > :min ORDER BY age", new { min = 26 });
        Eq(older.Count, 2, "query size");
        Eq(older[0], M("name", "Ann"), "query rows");
        Eq(await db.QueryOneAsync("SELECT name, age FROM people WHERE email = @e", new { e = "b@x.org" }), M("name", "Bob", "age", 25L), "QueryOne (anonymous object)");
        Eq(await db.QueryOneAsync("SELECT * FROM people WHERE id = ?", 999), null, "QueryOne none");
        Eq(await db.QueryValueAsync("SELECT COUNT(*) FROM people"), 4L, "QueryValue");
        Eq(await db.QueryValueAsync("SELECT COUNT(*) FROM people WHERE city = $1 OR name = $1", "Kandy"), 2L, "$1 twice");
        Eq((await db.QueryAsync("SELECT * FROM people WHERE id = ?", new[] { -1 })).Count, 0, "empty result");
        Ok((await db.SqlAsync("SELECT * FROM people WHERE id = -1")).HasRows, "an empty result still has rows");
        var cols = await db.SqlAsync("SELECT name, age FROM people ORDER BY id");
        Eq(cols.Columns, new[] { "name", "age" }, "Columns");
        Eq(await db.ExecuteAsync("UPDATE people SET age = age + 1 WHERE city = ?", "Kandy"), 2L, "update changes");
        Eq(await db.ExecuteAsync("DELETE FROM people WHERE email = ?", "zzz"), 0L, "delete none");
        // Typed rows.
        var people = await db.QueryAsync<Person>("SELECT * FROM people ORDER BY id");
        Eq(people.Count, 4, "typed rows");
        Eq(people[0], new Person((long)ann!, "a@x.org", "Ann", 32, "Kandy"), "a record from a row");
        Eq(people[3].Age, null, "null into int?");
        Eq(await db.QueryAsync<string>("SELECT name FROM people ORDER BY id"), new[] { "Ann", "Bob", "Cy", "Dee" }, "simple type: the first column");
        Eq((await db.QueryOneAsync<Person>("SELECT * FROM people WHERE name = ?", "Cy"))!.Age, 42, "QueryOneAsync<T>");
        Eq(await db.QueryOneAsync<Person>("SELECT * FROM people WHERE id = -1"), null, "QueryOneAsync<T> none");
        await ThrowsAsync<InputException>(() => db.QueryAsync("UPDATE people SET age = 1 WHERE id = -1"), "query of a write");
        // The blocking API.
        Eq(db.QueryValue("SELECT COUNT(*) FROM people WHERE age >= :a", new Dictionary<string, object> { ["a"] = 0 }), 3L, "QueryValue (blocking, named)");
        Eq(db.Query("SELECT name FROM people WHERE city = ? ORDER BY name", "Kandy").Count, 2, "Query (blocking)");
        Eq(db.QueryValue<string>("SELECT name FROM people WHERE city = ?", "Galle"), "Bob", "QueryValue<T> (blocking)");
        Eq(db.QueryOne("SELECT * FROM people WHERE city = ?", "Nowhere"), null, "QueryOne (blocking)");
        Eq(db.Query<Person>("SELECT * FROM people WHERE city = :c ORDER BY id", new { c = "Kandy" }).Select(p => p.Name), new[] { "Ann", "Cy" }, "Query<T> (blocking)");
        Eq(db.Execute("UPDATE people SET city = ? WHERE id = -1", "X"), 0L, "Execute (blocking)");
        Eq(db.Sql("SELECT 1 AS one").Rows![0], M("one", 1L), "Sql (blocking)");

        // ---- Errors map to classes.
        var dup = await ThrowsAsync<ConflictException>(() => db.ExecuteAsync("INSERT INTO people (email) VALUES (?)", "a@x.org"), "duplicate");
        Eq(dup.Kind, "CONFLICT", "conflict kind");
        Eq(dup.SqlState, "23000", "conflict SQLSTATE");
        Ok(dup is KestroException && dup is DbException, "KestroException and DbException");
        Throws<ConflictException>(() => db.Execute("INSERT INTO people (email) VALUES (?)", "a@x.org"), "duplicate (blocking)");
        var bad = await ThrowsAsync<InputException>(() => db.QueryAsync("SELEC nonsense"), "bad SQL");
        Eq(bad.Code, "INPUT", "input code");
        await ThrowsAsync<InputException>(() => db.QueryAsync("SELECT * FROM people WHERE id = :id", new Dictionary<string, object>()), "missing named param");
        await ThrowsAsync<InputException>(() => db.QueryAsync("SELECT * FROM people WHERE id = ?"), "missing positional param");
        await ThrowsAsync<InputException>(() => db.QueryAsync("SELECT * FROM people WHERE id = :id AND age = ?", 1, 2), "mixed params");
        await ThrowsAsync<InputException>(() => db.QueryAsync("SELECT * FROM no_such_table"), "missing table");
        Eq(KestroException.FromMessage("STALE something").GetType(), typeof(KestroException), "other codes: base class");
        Eq(KestroException.FromMessage("STALE something").Kind, "STALE", "other codes: kind");
        Eq(KestroException.FromMessage("UNAVAILABLE x").GetType(), typeof(UnavailableException), "UNAVAILABLE class");
        Eq(KestroException.FromMessage("READONLY x").GetType(), typeof(ReadOnlyException), "READONLY class");
        Eq(KestroException.FromMessage("BUSY x").GetType(), typeof(BusyException), "BUSY class");
        Eq(KestroException.FromMessage("CORRUPT x").GetType(), typeof(CorruptException), "CORRUPT class");
        Eq(KestroException.FromMessage("LIMIT x").GetType(), typeof(LimitException), "LIMIT class");
        Eq(KestroException.FromMessage("AUTH x").GetType(), typeof(AuthException), "AUTH class");
        Eq(KestroException.FromMessage("IO x").GetType(), typeof(KestroIOException), "IO class");
        Eq(KestroException.FromMessage("CLOSED x").GetType(), typeof(ClosedException), "CLOSED class");
        Ok(KestroException.FromMessage("CLOSED x") is KestroIOException, "CLOSED is a KestroIOException");
        Eq(KestroException.FromMessage("weird message").Kind, "ERROR", "no code");
        Ok(KestroException.FromMessage("BUSY x").IsTransient && !KestroException.FromMessage("CONFLICT x").IsTransient, "IsTransient");
        Eq(await db.QueryValueAsync("SELECT COUNT(*) FROM people"), 4L, "usable after errors");

        // ---- Transactions.
        await db.ExecuteAsync("CREATE TABLE IF NOT EXISTS acct (id INTEGER PRIMARY KEY, bal INTEGER)");
        await db.ExecuteAsync("DELETE FROM acct");
        await db.ExecuteAsync("INSERT INTO acct (id, bal) VALUES (1, 100), (2, 0)");
        await using var other = await KestroClient.ConnectAsync(address, opts);
        await db.BeginAsync();
        Ok(db.InTransaction, "BeginAsync opens");
        await ThrowsAsync<InputException>(() => db.BeginAsync(), "nested begin");
        await db.ExecuteAsync("UPDATE acct SET bal = bal - 10 WHERE id = 1");
        Eq(await db.QueryValueAsync("SELECT bal FROM acct WHERE id = 1"), 90L, "reads its own write");
        Eq(await other.QueryValueAsync("SELECT bal FROM acct WHERE id = 1"), 100L, "others do not see it yet");
        await ThrowsAsync<InputException>(() => db.SetAsync("kv", "inside", 1), "key/value write inside a SQL transaction");
        Ok(db.InTransaction, "still open after a refused statement");
        await db.CommitAsync();
        Ok(!db.InTransaction, "CommitAsync closes");
        Eq(await other.QueryValueAsync("SELECT bal FROM acct WHERE id = 1"), 90L, "committed");
        await db.CommitAsync();   // nothing open: nothing to do
        await db.RollbackAsync();
        db.Begin();
        db.Execute("UPDATE acct SET bal = 0 WHERE id = 1");
        db.Rollback();
        Ok(!db.InTransaction, "Rollback closes");
        Eq(await db.QueryValueAsync("SELECT bal FROM acct WHERE id = 1"), 90L, "rolled back");
        // As SQL text.
        await db.SqlAsync("BEGIN");
        Ok(db.InTransaction, "BEGIN as SQL");
        await db.ExecuteAsync("UPDATE acct SET bal = 1 WHERE id = 2");
        await db.SqlAsync("ROLLBACK");
        Ok(!db.InTransaction, "ROLLBACK as SQL");
        await db.SqlAsync("start transaction");
        Ok(db.InTransaction, "START TRANSACTION");
        await db.SqlAsync("  commit;");
        Ok(!db.InTransaction, "commit; with spaces");
        // TransactionAsync(fn): commit, or rollback and rethrow.
        await db.TransactionAsync(async tx =>
        {
            await tx.ExecuteAsync("UPDATE acct SET bal = bal - 30 WHERE id = 1");
            await tx.ExecuteAsync("UPDATE acct SET bal = bal + 30 WHERE id = 2");
        });
        Eq(await db.QueryValueAsync("SELECT SUM(bal) FROM acct"), 90L, "TransactionAsync kept the total");
        Eq(await db.QueryValueAsync("SELECT bal FROM acct WHERE id = 2"), 30L, "TransactionAsync committed");
        var boom = await ThrowsAsync<InvalidOperationException>(() => db.TransactionAsync(async tx =>
        {
            await tx.ExecuteAsync("UPDATE acct SET bal = 0");
            throw new InvalidOperationException("boom");
        }), "TransactionAsync rethrows");
        Eq(boom.Message, "boom", "the body's exception");
        Ok(!db.InTransaction, "rolled back after the exception");
        Eq(await db.QueryValueAsync("SELECT bal FROM acct WHERE id = 2"), 30L, "TransactionAsync rolled back");
        Eq(await db.TransactionAsync(tx => tx.QueryValueAsync<long>("SELECT bal FROM acct WHERE id = 1")), 60L, "TransactionAsync<T>");
        await ThrowsAsync<ConflictException>(() => db.TransactionAsync(async tx =>
        {
            await tx.ExecuteAsync("INSERT INTO people (email) VALUES (?)", "zz@x.org");
            await tx.ExecuteAsync("INSERT INTO people (email) VALUES (?)", "zz@x.org");
        }), "conflict inside TransactionAsync");
        Ok(!db.InTransaction, "closed after a conflict");
        Eq(await db.QueryValueAsync("SELECT COUNT(*) FROM people WHERE email = 'zz@x.org'"), 0L, "nothing of it kept");
        await ThrowsAsync<InputException>(() => db.TransactionAsync(tx => tx.TransactionAsync(_ => Task.CompletedTask)), "nested TransactionAsync");
        Ok(!db.InTransaction, "the outer one rolled back");
        // The blocking Transaction.
        db.Transaction(tx => tx.Execute("UPDATE acct SET bal = bal + 1 WHERE id = 1"));
        Eq(db.QueryValue("SELECT bal FROM acct WHERE id = 1"), 61L, "Transaction committed");
        Throws<InvalidOperationException>(() => db.Transaction(tx =>
        {
            tx.Execute("UPDATE acct SET bal = 0 WHERE id = 1");
            throw new InvalidOperationException("sync boom");
        }), "Transaction rethrows");
        Eq(db.QueryValue("SELECT bal FROM acct WHERE id = 1"), 61L, "Transaction rolled back");
        Eq(db.Transaction(tx => tx.QueryValue<long>("SELECT bal FROM acct WHERE id = 2")), 30L, "Transaction<T>");
        // Other callers of the same client wait for a TransactionAsync body; calls inside it are serialized.
        var inBody = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var txTask = db.TransactionAsync(async tx =>
        {
            await tx.ExecuteAsync("UPDATE acct SET bal = 777 WHERE id = 2");
            inBody.SetResult();
            await Task.Delay(300);
            var both = await Task.WhenAll(tx.QueryValueAsync<long>("SELECT bal FROM acct WHERE id = 1"), tx.QueryValueAsync<long>("SELECT bal FROM acct WHERE id = 2"));
            Eq(both, new[] { 61L, 777L }, "concurrent calls inside the body");
            await tx.ExecuteAsync("UPDATE acct SET bal = 31 WHERE id = 2");
        });
        await inBody.Task;
        var outside = db.QueryValueAsync<long>("SELECT bal FROM acct WHERE id = 2");
        await Task.Delay(100);
        Ok(!outside.IsCompleted, "a call from outside waits for the transaction");
        await txTask;
        Eq(await outside, 31L, "and then sees its result");
        // Savepoints: ROLLBACK TO keeps the transaction; a SAVEPOINT outside one begins it.
        await db.BeginAsync();
        await db.ExecuteAsync("UPDATE acct SET bal = 1000 WHERE id = 1");
        await db.SqlAsync("SAVEPOINT sp1");
        await db.ExecuteAsync("UPDATE acct SET bal = 2000 WHERE id = 1");
        await db.SqlAsync("ROLLBACK TO SAVEPOINT sp1");
        Ok(db.InTransaction, "ROLLBACK TO keeps the transaction open");
        Eq(await db.QueryValueAsync("SELECT bal FROM acct WHERE id = 1"), 1000L, "ROLLBACK TO undid the later write");
        await db.RollbackAsync();
        Eq(await db.QueryValueAsync("SELECT bal FROM acct WHERE id = 1"), 61L, "whole rollback");
        await db.SqlAsync("SAVEPOINT outer_sp");
        Ok(db.InTransaction, "SAVEPOINT outside a transaction begins one");
        await db.ExecuteAsync("UPDATE acct SET bal = 62 WHERE id = 1");
        await db.SqlAsync("RELEASE SAVEPOINT outer_sp");
        Ok(!db.InTransaction, "releasing it commits");
        Eq(await other.QueryValueAsync("SELECT bal FROM acct WHERE id = 1"), 62L, "released savepoint committed");
        // BEGIN CONCURRENT: first committer wins; the loser's COMMIT fails and ends it.
        await db.BeginAsync(concurrent: true);
        await other.SqlAsync("BEGIN CONCURRENT");
        await db.ExecuteAsync("UPDATE acct SET bal = 1 WHERE id = 2");
        await other.ExecuteAsync("UPDATE acct SET bal = 2 WHERE id = 2");
        await other.CommitAsync();
        var lost = await ThrowsAsync<ConflictException>(() => db.CommitAsync(), "second committer loses");
        Ok(lost.Message.Contains("CONFLICT", StringComparison.Ordinal), "conflict message");
        Ok(!db.InTransaction, "a failed COMMIT ends the transaction");
        Eq(await db.QueryValueAsync("SELECT bal FROM acct WHERE id = 2"), 2L, "first committer's value");

        // ---- Key/value with versions.
        await db.DeleteAsync("kv", "k1");
        long v1 = await db.SetAsync("kv", "k1", M("n", 1L));
        Ok(v1 > 0, "set returns a version");
        var got = (await db.GetAsync("kv", "k1"))!;
        Eq(got.Version, v1, "get version");
        Eq(got.Value, M("n", 1L), "get value");
        Eq(await db.GetAsync("kv", "missing"), null, "get missing");
        long v2 = await db.SetAsync("kv", "k1", M("n", 2L), v1);
        Ok(v2 > v1, "conditional set at the right version");
        var stale = await ThrowsAsync<ConflictException>(() => db.SetAsync("kv", "k1", M("n", 3L), v1), "stale expectedVersion");
        Eq(stale.Kind, "CONFLICT", "stale kind");
        Eq((await db.GetAsync("kv", "k1"))!.Value, M("n", 2L), "not overwritten");
        await ThrowsAsync<ConflictException>(() => db.SetAsync("kv", "k1", 1, 0), "expectedVersion 0 on an existing key");
        await db.DeleteAsync("kv", "fresh");
        long vf = await db.SetAsync("kv", "fresh", "new", 0);
        Eq(await db.GetAsync("kv", "fresh"), new Versioned(vf, "new"), "expectedVersion 0 creates");
        await ThrowsAsync<ConflictException>(() => db.DeleteAsync("kv", "fresh", vf + 1000), "conditional delete, wrong version");
        await db.DeleteAsync("kv", "fresh", vf);
        Eq(await db.GetAsync("kv", "fresh"), null, "conditional delete");
        // Values of every JSON type.
        object?[] values =
        {
            null, true, false, 0L, -1L, long.MaxValue, long.MinValue, 3.5, -1.25e-10, "", "text \uD83D\uDE00 \"q\" \\ \n",
            L(), M(), M("deep", L(M("x", L(1L, null, "y")))), BigInteger.Parse("123456789012345678901234567890"),
        };
        for (int i = 0; i < values.Length; i++)
        {
            await db.SetAsync("kv", "type" + i, values[i]);
            Eq((await db.GetAsync("kv", "type" + i))!.Value, values[i], "value round trip " + i);
        }
        // Objects of your own types, and back.
        await db.SetAsync("kv", "profile", new { name = "Kestro", tags = new[] { "a", "b" }, at = new DateOnly(2026, 11, 20) });
        var profile = (await db.GetAsync("kv", "profile"))!;
        Eq(profile.Value, M("name", "Kestro", "tags", L("a", "b"), "at", "2026-11-20"), "anonymous object value");
        Eq(profile.As<Profile>()!.Tags, new[] { "a", "b" }, "Versioned.As<T>");
        Eq(profile.As<Profile>()!.At, new DateOnly(2026, 11, 20), "Versioned.As<T> date");
        // Unicode keys; key and collection rules.
        string ukey = "ключ/日本/\uD83D\uDE00 with spaces";
        await db.SetAsync("kv", ukey, "unicode key");
        Eq((await db.GetAsync("kv", ukey))!.Value, "unicode key", "unicode key");
        await ThrowsAsync<InputException>(() => db.GetAsync("bad-name", "k"), "invalid collection");
        await ThrowsAsync<InputException>(() => db.GetAsync("1abc", "k"), "collection starting with a digit");
        await ThrowsAsync<InputException>(() => db.GetAsync("abc\n", "k"), "collection with a newline");
        await ThrowsAsync<InputException>(() => db.GetAsync("kv", ""), "empty key");
        await ThrowsAsync<InputException>(() => db.GetAsync("kv", "a\0b"), "NUL in key");
        await ThrowsAsync<InputException>(() => db.GetAsync("kv", new string('x', 513)), "key over 512 bytes");
        await ThrowsAsync<InputException>(() => db.GetAsync("kv", "\uD800"), "lone surrogate key");
        await db.SetAsync("kv", new string('y', 512), 1);
        await ThrowsAsync<LimitException>(() => db.SetAsync("kv", "huge", new string('z', 300 * 1024)), "value over 256 KiB");
        string big = new string('b', 200 * 1024);
        await db.SetAsync("kv", "big", big);
        Eq((await db.GetAsync("kv", "big"))!.Value, big, "200 KiB value");
        // GetMany: one snapshot, in order, null where missing.
        var many = await db.GetManyAsync("kv", "k1", "missing", "type3", ukey);
        Eq(many.Count, 4, "GetMany size");
        Eq(many[0]!.Value, M("n", 2L), "GetMany 0");
        Eq(many[1], null, "GetMany missing");
        Eq(many[2]!.Value, 0L, "GetMany 2");
        Eq(many[3]!.Value, "unicode key", "GetMany unicode");
        Eq((await db.GetManyAsync("kv", new List<string>())).Count, 0, "GetMany none");
        await ThrowsAsync<LimitException>(() => db.GetManyAsync("kv", Enumerable.Range(0, 4097).Select(i => "k" + i)), "GetMany over 4096");
        // Batch: all or nothing, one version.
        long vb = await db.BatchAsync(Op.Set("kv", "b1", 1), Op.Set("kv", "b2", 2), Op.Delete("kv", "type0"));
        Eq((await db.GetAsync("kv", "b1"))!.Version, vb, "batch version b1");
        Eq((await db.GetAsync("kv", "b2"))!.Version, vb, "batch version b2");
        Eq(await db.GetAsync("kv", "type0"), null, "batch delete");
        await ThrowsAsync<ConflictException>(() => db.BatchAsync(Op.Set("kv", "b1", 10), Op.Set("kv", "b2", 20, vb - 1)), "batch with a stale version");
        Eq((await db.GetAsync("kv", "b1"))!.Value, 1L, "nothing of a failed batch applied");
        await ThrowsAsync<InputException>(() => db.BatchAsync(Op.Set("kv", "d", 1), Op.Delete("kv", "d")), "duplicate key in a batch");
        await ThrowsAsync<InputException>(() => db.BatchAsync(new List<Op>()), "empty batch");
        Throws<InputException>(() => Op.Set("kv", "x", 1, -5), "negative expectedVersion");
        long vbulk = await db.BatchAsync(Enumerable.Range(0, 1000).Select(i => Op.Set("bulk", "k" + i, M("i", (long)i))));
        Eq(await db.GetAsync("bulk", "k999"), new Versioned(vbulk, M("i", 999L)), "1000-op batch");
        // The blocking key/value API.
        long sv = db.Set("kv", "sync", "s1");
        Eq(db.Get("kv", "sync"), new Versioned(sv, "s1"), "Set/Get (blocking)");
        Throws<ConflictException>(() => db.Set("kv", "sync", "s2", sv + 1000), "Set expectedVersion (blocking)");
        Eq(db.GetMany("kv", "sync", "missing").Select(x => x?.Value), L("s1", null), "GetMany (blocking)");
        long sb = db.Batch(Op.Set("kv", "sync", "s3", sv), Op.Delete("kv", "b2"));
        Eq(db.Get("kv", "sync")!.Version, sb, "Batch (blocking)");
        db.Delete("kv", "sync");
        Eq(db.Get("kv", "sync"), null, "Delete (blocking)");
        // Server calls.
        Ok((long)(await db.StatsAsync())["keys"]! > 1000, "stats keys");
        await db.VerifyAsync();
        db.Verify();
        await ThrowsAsync<InputException>(() => db.BackupAsync("x.kdb"), "backup without --backup-dir");
        await ThrowsAsync<InputException>(() => db.BackupAsync(""), "backup name");
        var compacted = await db.CompactAsync();
        Ok(compacted.ContainsKey("before") && compacted.ContainsKey("after"), "CompactAsync");
        Eq((await db.GetAsync("bulk", "k5"))!.Value, M("i", 5L), "reads after compaction");

        // ---- Tasks and threads share one client (calls are serialized).
        var tasks = Enumerable.Range(0, 8).Select(n => Task.Run(async () =>
        {
            for (int i = 0; i < 50; i++)
            {
                await db.SetAsync("threads", "t" + n + "_" + i, i);
                var v = (await db.GetAsync("threads", "t" + n + "_" + i))!.Value;
                if (!Equals(v, (long)i)) throw new InvalidOperationException("task read " + v);
                await db.QueryValueAsync("SELECT COUNT(*) FROM people");
            }
        })).ToList();
        Exception? threadError = null;
        var threads = Enumerable.Range(0, 4).Select(n => new Thread(() =>
        {
            try
            {
                for (int i = 0; i < 50; i++)
                {
                    db.Set("threads", "s" + n + "_" + i, i);
                    var v = db.Get("threads", "s" + n + "_" + i)!.Value;
                    if (!Equals(v, (long)i)) throw new InvalidOperationException("thread read " + v);
                    db.QueryValue("SELECT COUNT(*) FROM people");
                }
            }
            catch (Exception e)
            {
                threadError ??= e;
            }
        })).ToList();
        threads.ForEach(t => t.Start());
        await Task.WhenAll(tasks);
        threads.ForEach(t => t.Join());
        if (threadError is not null) Fail("threads: " + threadError);
        Eq(await db.QueryValueAsync("SELECT COUNT(*) FROM people"), 4L, "after the threads");

        // ---- An idle connection is kept (no reconnect when nothing happened); cancellation.
        int opened = db.Opens;
        await Task.Delay(1200);
        Eq(await db.QueryValueAsync("SELECT COUNT(*) FROM people"), 4L, "after an idle second");
        Eq(db.Opens, opened, "the idle connection was kept, not reopened");
        await ThrowsAsync<OperationCanceledException>(() => db.QueryAsync("SELECT 1", new CancellationToken(true)), "a cancelled token");
        Eq(await db.QueryValueAsync("SELECT 1"), 1L, "usable after a cancelled call");
        Eq(db.Opens, opened, "nothing was sent: the connection was kept");

        // ---- Closed.
        await other.DisposeAsync();
        await db.DisposeAsync();
        await db.DisposeAsync();   // twice is fine
        db.Dispose();
        Ok(db.IsClosed, "IsClosed");
        var closed = await ThrowsAsync<ClosedException>(() => db.GetAsync("kv", "k1"), "use after close");
        Eq(closed.Kind, "CLOSED", "CLOSED kind");
        Throws<ClosedException>(() => db.Get("kv", "k1"), "use after close (blocking)");
        await using (var auto = await KestroClient.ConnectAsync(address, opts))
            Eq((await auto.GetAsync("kv", "k1"))!.Value, M("n", 2L), "await using");
        using (var auto = KestroClient.Connect(address, opts))
            Eq(auto.Get("kv", "k1")!.Value, M("n", 2L), "using (blocking)");
    }

    // ------------------------------------------------------------------ ADO.NET and Dapper

    private static async Task Ado(string address, string token)
    {
        string cs = "Address=" + address + ";Token=" + token;
        var wrong = new KestroConnection("Address=" + address + ";Token=wrong-token-0123456789");
        var e = Throws<DbException>(() => wrong.Open(), "ADO wrong token");
        Ok(e is AuthException, "an AuthException");
        Eq(wrong.State, ConnectionState.Closed, "closed after a failed Open");
        Throws<ArgumentException>(() => new KestroConnection("Address=x;Nonsense=1").Open(), "unknown connection string key");

        using var conn = new KestroConnection(cs);
        Eq(conn.State, ConnectionState.Closed, "not open yet");
        conn.Open();
        Eq(conn.State, ConnectionState.Open, "Open");
        Eq(conn.DataSource, address, "DataSource");
        Ok(conn.Client is not null, "Client");
        using (var cmd = conn.CreateCommand())
        {
            cmd.CommandText = "CREATE TABLE IF NOT EXISTS jt (id INTEGER PRIMARY KEY, name TEXT, n INTEGER, r REAL, ok BOOLEAN, d DATE, j JSON)";
            cmd.ExecuteNonQuery();
            cmd.CommandText = "DELETE FROM jt";
            cmd.ExecuteNonQuery();
            cmd.CommandText = "INSERT INTO jt (name, n, r, ok, d, j) VALUES (@name, @n, :r, @ok, @d, @j)";
            cmd.Parameters.AddWithValue("name", "prep \uD83D\uDE00");
            cmd.Parameters.AddWithValue("@n", 7);
            cmd.Parameters.AddWithValue(":r", 2.5);
            var p = cmd.CreateParameter();
            p.ParameterName = "ok";
            p.Value = true;
            p.DbType = DbType.Boolean;
            cmd.Parameters.Add(p);
            cmd.Parameters.AddWithValue("d", new DateOnly(2026, 11, 20));
            cmd.Parameters.AddWithValue("j", M("a", L(1L, 2L)));
            Eq(cmd.ExecuteNonQuery(), 1, "ExecuteNonQuery");
            Ok(cmd.Parameters.Contains("@name") && cmd.Parameters.IndexOf("j") == 5, "parameter names with or without @");
            cmd.Parameters.Clear();
            cmd.CommandText = "INSERT INTO jt (name, n, r, ok, d, j) VALUES (?, ?, ?, ?, ?, ?)";
            foreach (var v in new object[] { "nulls", DBNull.Value, DBNull.Value, DBNull.Value, DBNull.Value, DBNull.Value }) cmd.Parameters.Add(new KestroParameter(null, v));
            Eq(cmd.ExecuteNonQuery(), 1, "positional parameters, DBNull");
            cmd.Parameters.Clear();
            cmd.CommandText = "SELECT COUNT(*) FROM jt";
            Eq(cmd.ExecuteScalar(), 2L, "ExecuteScalar");
            Eq(await cmd.ExecuteScalarAsync(), 2L, "ExecuteScalarAsync");
            cmd.CommandText = "SELECT n FROM jt WHERE name = 'nulls'";
            Eq(cmd.ExecuteScalar(), DBNull.Value, "ExecuteScalar NULL is DBNull");
            cmd.CommandText = "SELECT n FROM jt WHERE id = -1";
            Eq(cmd.ExecuteScalar(), null, "ExecuteScalar without rows is null");
            cmd.CommandText = "SELECT * FROM jt";
            Eq(cmd.ExecuteNonQuery(), -1, "ExecuteNonQuery of a SELECT is -1");
            cmd.CommandText = "INSERT INTO jt (name) VALUES (@a)";
            cmd.Parameters.AddWithValue("a", 1);
            cmd.Parameters.Add(new KestroParameter(null, 2));
            Throws<InputException>(() => cmd.ExecuteNonQuery(), "named and unnamed parameters");
        }
        using (var cmd = new KestroCommand("SELECT id, name, n, r, ok, d, j FROM jt WHERE name = @name", conn))
        {
            cmd.Parameters.AddWithValue("name", "prep \uD83D\uDE00");
            using (var rs = cmd.ExecuteReader())
            {
                Eq(rs.FieldCount, 7, "FieldCount");
                Eq(rs.GetName(1), "name", "GetName");
                Eq(rs.GetOrdinal("NAME"), 1, "GetOrdinal ignores case");
                Eq(rs.GetFieldType(2), typeof(long), "integer column type");
                Eq(rs.GetFieldType(3), typeof(double), "real column type");
                Eq(rs.GetFieldType(4), typeof(bool), "boolean column type");
                Eq(rs.GetFieldType(1), typeof(string), "text column type");
                Eq(rs.GetDataTypeName(2), "INTEGER", "GetDataTypeName");
                Ok(rs.HasRows, "HasRows");
                Eq(rs.RecordsAffected, -1, "RecordsAffected of a SELECT");
                Ok(rs.Read(), "a row");
                Eq(rs.GetString(1), "prep \uD83D\uDE00", "GetString");
                Eq(rs.GetInt32(2), 7, "GetInt32");
                Eq(rs.GetInt64(2), 7L, "GetInt64");
                Eq(rs["n"], 7L, "indexer by name");
                Eq(rs.GetDouble(3), 2.5, "GetDouble");
                Eq(rs.GetDecimal(3), 2.5m, "GetDecimal");
                Ok(rs.GetBoolean(4), "GetBoolean");
                Eq(rs.GetDateTime(5), new DateTime(2026, 11, 20), "GetDateTime");
                Eq(rs.GetFieldValue<DateOnly>(5), new DateOnly(2026, 11, 20), "GetFieldValue<DateOnly>");
                Eq(rs.GetString(6), "{\"a\":[1,2]}", "JSON column as text");
                Eq(rs.GetFieldValue<int>(2), 7, "GetFieldValue<int>");
                Ok(!rs.IsDBNull(2), "IsDBNull false");
                Throws<InvalidCastException>(() => rs.GetInt32(1), "text as int");
                Throws<IndexOutOfRangeException>(() => rs.GetOrdinal("nope"), "unknown column");
                var values = new object[7];
                Eq(rs.GetValues(values), 7, "GetValues");
                Ok(!rs.Read(), "one row");
            }
            cmd.Parameters["name"].Value = "nulls";
            using (var rs = await cmd.ExecuteReaderAsync())
            {
                Ok(await rs.ReadAsync(), "null row");
                Ok(rs.IsDBNull(2), "IsDBNull");
                Eq(rs.GetValue(2), DBNull.Value, "GetValue of NULL");
                Eq(rs.GetFieldValue<int?>(2), null, "GetFieldValue<int?> of NULL");
                Throws<InvalidCastException>(() => rs.GetInt32(2), "GetInt32 of NULL");
            }
        }
        // Transactions.
        using (var tx = conn.BeginTransaction())
        {
            Ok(conn.Client!.InTransaction, "BeginTransaction");
            using var cmd = new KestroCommand("UPDATE jt SET n = 100 WHERE name = 'nulls'", conn) { Transaction = tx };
            cmd.ExecuteNonQuery();
            tx.Rollback();
        }
        Ok(!conn.Client!.InTransaction, "Rollback");
        using (var tx = await conn.BeginTransactionAsync())
        {
            await new KestroCommand("UPDATE jt SET n = 200 WHERE name = 'nulls'", conn).ExecuteNonQueryAsync();
            await tx.CommitAsync();
        }
        using (var tx = conn.BeginTransaction())
        {
            new KestroCommand("UPDATE jt SET n = 300 WHERE name = 'nulls'", conn).ExecuteNonQuery();
            // disposed without Commit: rolled back
        }
        Ok(!conn.Client!.InTransaction, "Dispose rolls back");
        Eq(new KestroCommand("SELECT n FROM jt WHERE name = 'nulls'", conn).ExecuteScalar(), 200L, "rollback, commit, dispose");
        // Errors are DbExceptions with SQLSTATEs.
        var dup = Throws<DbException>(() => new KestroCommand("INSERT INTO jt (id, name) VALUES (1, 'x'), (1, 'y')", conn).ExecuteNonQuery(), "ADO duplicate id");
        Eq(dup.SqlState, "23000", "SQLSTATE 23000");
        Ok(dup is ConflictException, "a ConflictException");
        Eq(Throws<DbException>(() => new KestroCommand("SELEC 1", conn).ExecuteReader(), "ADO bad SQL").SqlState, "42000", "SQLSTATE 42000");

        // Dapper.
        Eq(conn.Execute("INSERT INTO jt (name, n) VALUES (@name, @n)", new[] { new { name = "d1", n = 1 }, new { name = "d2", n = 2 } }), 2, "Dapper Execute with a list");
        var list = conn.Query<JtRow>("SELECT * FROM jt WHERE n >= @min ORDER BY n", new { min = 1 }).ToList();
        Eq(list.Count, 4, "Dapper Query<T>");
        Eq(list[0].Name, "d1", "Dapper column to property");
        Eq(list[2].N, 7, "Dapper long to int?");
        Eq(list[2].R, 2.5, "Dapper double");
        Eq(list[2].Ok, true, "Dapper bool");
        Eq(list[2].D, new DateTime(2026, 11, 20), "Dapper text to DateTime");
        Eq(list[2].J, "{\"a\":[1,2]}", "Dapper JSON as text");
        Eq(list[3].N, 200, "Dapper row 4");
        Eq(conn.ExecuteScalar<long>("SELECT COUNT(*) FROM jt"), 4L, "Dapper ExecuteScalar<T>");
        Eq(conn.QuerySingle<int>("SELECT n FROM jt WHERE name = @name", new { name = "d2" }), 2, "Dapper QuerySingle<int>");
        Eq(conn.QueryFirstOrDefault<JtRow>("SELECT * FROM jt WHERE id = -1"), null, "Dapper QueryFirstOrDefault none");
        Eq(conn.Query<string>("SELECT name FROM jt WHERE n IN @ns ORDER BY n", new { ns = new[] { 1, 2 } }).ToList(), new[] { "d1", "d2" }, "Dapper IN list");
        dynamic first = conn.Query("SELECT name, n FROM jt WHERE name = 'd1'").First();
        Eq((string)first.name, "d1", "Dapper dynamic rows");
        var asyncRows = (await conn.QueryAsync<JtRow>("SELECT * FROM jt WHERE name LIKE @p ORDER BY n", new { p = "d%" })).ToList();
        Eq(asyncRows.Select(r => r.N), L(1, 2), "Dapper QueryAsync");
        Eq(await conn.ExecuteAsync("UPDATE jt SET n = n + 10 WHERE name LIKE 'd%'"), 2, "Dapper ExecuteAsync");
        var dargs = new DynamicParameters();
        dargs.Add("name", "d1");
        Eq(conn.ExecuteScalar<int>("SELECT n FROM jt WHERE name = @name", dargs), 11, "Dapper DynamicParameters");
        using (var tx = conn.BeginTransaction())
        {
            conn.Execute("UPDATE jt SET n = 0 WHERE name = 'd1'", transaction: tx);
            Eq(conn.ExecuteScalar<int>("SELECT n FROM jt WHERE name = 'd1'", transaction: tx), 0, "Dapper inside a transaction");
            tx.Rollback();
        }
        Eq(conn.ExecuteScalar<int>("SELECT n FROM jt WHERE name = 'd1'"), 11, "Dapper transaction rolled back");
        // A closed connection Dapper opens and closes itself; wrapping a client; the factory.
        using (var closedConn = new KestroConnection(cs))
        {
            Eq(closedConn.ExecuteScalar<long>("SELECT COUNT(*) FROM jt"), 4L, "Dapper opens a closed connection");
            Eq(closedConn.State, ConnectionState.Closed, "and closes it");
        }
        await using (var client = await KestroClient.ConnectAsync(address, new KestroOptions { Token = token }))
        {
            using (var wrapped = new KestroConnection(client)) Eq(wrapped.ExecuteScalar<long>("SELECT COUNT(*) FROM jt"), 4L, "a wrapped client");
            Ok(!client.IsClosed, "closing the wrapper keeps the client");
        }
        DbConnection fromFactory = KestroFactory.Instance.CreateConnection();
        fromFactory.ConnectionString = cs;
        await fromFactory.OpenAsync();
        Eq(await fromFactory.ExecuteScalarAsync<long>("SELECT COUNT(*) FROM jt"), 4L, "KestroFactory");
        await fromFactory.DisposeAsync();
        conn.Close();
        Eq(conn.State, ConnectionState.Closed, "Close");
        Throws<InvalidOperationException>(() => new KestroCommand("SELECT 1", conn).ExecuteScalar(), "command on a closed connection");
    }

    // ------------------------------------------------------------------ reconnect

    private static async Task Reconnect(string address, string token)
    {
        var opts = new KestroOptions { Token = token };
        var db = await KestroClient.ConnectAsync(address, opts);
        var sync = KestroClient.Connect(address, opts);
        await db.ExecuteAsync("CREATE TABLE IF NOT EXISTS rc (id INTEGER PRIMARY KEY, v TEXT)");
        await db.ExecuteAsync("INSERT INTO rc (v) VALUES (?)", "before");
        await db.SetAsync("rkv", "k", "before");
        var tx = await KestroClient.ConnectAsync(address, opts);
        var off = await KestroClient.ConnectAsync(address, new KestroOptions { Token = token, Reconnect = false });
        await off.StatsAsync();
        var ado = new KestroConnection("Address=" + address + ";Token=" + token);
        ado.Open();
        sync.Stats();
        Say("READY");
        ReadLine();               // the server restarts meanwhile
        await Task.Delay(1500);   // idle long enough for the check before the next request
        int opened = db.Opens;
        await db.ExecuteAsync("INSERT INTO rc (v) VALUES (?)", "after");
        Eq(db.Opens, opened + 1, "reopened once");
        Eq(await db.QueryValueAsync("SELECT COUNT(*) FROM rc"), 2L, "rows after reconnect");
        Eq((await db.GetAsync("rkv", "k"))!.Value, "before", "key/value after reconnect");
        int syncOpened = sync.Opens;
        Eq(sync.QueryValue("SELECT COUNT(*) FROM rc"), 2L, "blocking client after reconnect");
        Eq(sync.Opens, syncOpened + 1, "blocking client reopened once");
        // A transaction cannot survive its connection: the next call says so, the one after reconnects.
        // (BEGIN CONCURRENT: two plain BEGINs would wait for each other's write gate.)
        await tx.BeginAsync(concurrent: true);
        Ok(tx.InTransaction, "transaction on the new server");
        await tx.ExecuteAsync("INSERT INTO rc (v) VALUES (?)", "in tx");
        sync.Begin(concurrent: true);
        sync.Execute("INSERT INTO rc (v) VALUES (?)", "in sync tx");
        Say("READY2");
        ReadLine();               // restarted again, with the transactions open
        var lostTx = await ThrowsAsync<KestroIOException>(() => tx.ExecuteAsync("INSERT INTO rc (v) VALUES (?)", "x"), "call inside a lost transaction");
        Ok(lostTx.Message.StartsWith("IO connection lost inside a transaction; it was rolled back", StringComparison.Ordinal), "lost transaction message: " + lostTx.Message);
        Ok(!tx.InTransaction, "no longer in a transaction");
        Eq(await tx.QueryValueAsync("SELECT COUNT(*) FROM rc"), 2L, "the next call reconnects; the transaction's row is gone");
        var lostSync = Throws<KestroIOException>(() => sync.Execute("INSERT INTO rc (v) VALUES (?)", "y"), "blocking call inside a lost transaction");
        Ok(lostSync.Message.StartsWith("IO connection lost inside a transaction; it was rolled back", StringComparison.Ordinal), "lost transaction message (blocking): " + lostSync.Message);
        Eq(sync.QueryValue("SELECT COUNT(*) FROM rc"), 2L, "blocking: the next call reconnects");
        // Reconnect = false: the dead connection fails with IO, and stays failed.
        var offErr = await ThrowsAsync<KestroIOException>(() => off.StatsAsync(), "reconnect off");
        Eq(offErr.Kind, "IO", "IO kind");
        await ThrowsAsync<KestroIOException>(() => off.StatsAsync(), "reconnect off, again");
        // ADO.NET reconnects the same way.
        Eq(new KestroCommand("SELECT COUNT(*) AS n FROM rc", ado).ExecuteScalar(), 2L, "ADO.NET after restart");
        ado.Dispose();
        await db.DisposeAsync();
        await tx.DisposeAsync();
        await off.DisposeAsync();
        sync.Dispose();
    }

    // ------------------------------------------------------------------ busy

    private static async Task Busy(string address, string token, int max)
    {
        var opts = new KestroOptions { Token = token };
        var held = new List<KestroClient>();
        for (int i = 0; i < max; i++)
        {
            var c = await KestroClient.ConnectAsync(address, opts);
            await c.StatsAsync();
            held.Add(c);
        }
        // The server answers BUSY and closes at once; the client's login frame, still unread,
        // can make that close a reset that discards the BUSY reply (then: IO). Every attempt
        // must fail, and BUSY must be seen.
        BusyException? busy = null;
        for (int i = 0; i < 20 && busy is null; i++)
        {
            try
            {
                var c = i % 2 == 0 ? await KestroClient.ConnectAsync(address, opts) : KestroClient.Connect(address, opts);
                await c.StatsAsync();
                Fail("a connection over --max-connections was accepted");
            }
            catch (BusyException e)
            {
                busy = e;
            }
            catch (KestroIOException e)
            {
                Ok(e.Message.Contains("before login", StringComparison.Ordinal) || e.Message.Contains("connection lost", StringComparison.Ordinal), "over the limit: " + e.Message);
            }
        }
        // Deterministic: a client that has sent nothing yet reads the server's BUSY before its
        // first request (a client without a token sends nothing when it connects).
        var quiet = await KestroClient.ConnectAsync(address);
        await Task.Delay(300);
        var waited = await ThrowsAsync<BusyException>(() => quiet.StatsAsync(), "BUSY waiting before the first request");
        Ok(waited.IsTransient, "BUSY is transient");
        await quiet.DisposeAsync();
        var quietSync = KestroClient.Connect(address);
        Thread.Sleep(300);
        Throws<BusyException>(() => quietSync.Stats(), "BUSY waiting before the first request (blocking)");
        quietSync.Dispose();
        busy ??= waited;
        Eq(busy.Kind, "BUSY", "BUSY kind");
        Ok(busy.Message.StartsWith("BUSY too many connections", StringComparison.Ordinal), "BUSY message: " + busy.Message);
        await held[0].DisposeAsync();
        held.RemoveAt(0);
        KestroClient? again = null;
        for (int i = 0; i < 50 && again is null; i++)
        {
            try
            {
                again = await KestroClient.ConnectAsync(address, opts);
                await again.StatsAsync();
            }
            catch (KestroException e) when (e is BusyException or KestroIOException)
            {
                again = null;
                await Task.Delay(100);
            }
        }
        Ok(again is not null, "a slot freed by DisposeAsync");
        await again!.DisposeAsync();
        foreach (var c in held) await c.DisposeAsync();
    }

    // ------------------------------------------------------------------ TLS

    private static async Task Tls(string address, string token, string ca, string wrongCa)
    {
        int port = int.Parse(address.Substring(address.LastIndexOf(':') + 1));
        var opts = new KestroOptions { Token = token, CaFile = ca };
        var db = await KestroClient.ConnectAsync(address, opts);
        Eq((await db.StatsAsync())["tls"], true, "stats tls");
        await db.ExecuteAsync("CREATE TABLE IF NOT EXISTS tl (id INTEGER PRIMARY KEY, body TEXT)");
        string big = new string('t', 300_000);   // many TLS records each way
        await db.ExecuteAsync("INSERT INTO tl (body) VALUES (?)", big);
        Eq(((string)(await db.QueryValueAsync("SELECT body FROM tl WHERE id = ?", db.LastInsertId))!).Length, 300_000, "300 KB over TLS");
        long v = await db.SetAsync("kv", "tls", M("ok", true));
        Eq(await db.GetAsync("kv", "tls"), new Versioned(v, M("ok", true)), "key/value over TLS");
        int opened = db.Opens;
        await Task.Delay(1200);
        Eq((await db.GetAsync("kv", "tls"))!.Version, v, "after an idle second");
        Eq(db.Opens, opened, "an idle TLS connection is kept");
        // The blocking path: handshake and requests.
        using (var blocking = KestroClient.Connect(address, opts))
        {
            Eq(blocking.Stats()["tls"], true, "blocking TLS");
            Eq(((string)blocking.QueryValue("SELECT body FROM tl WHERE id = ?", db.LastInsertId)!).Length, 300_000, "300 KB over blocking TLS");
        }
        // The certificate names localhost and 127.0.0.1.
        await using (var ip = await KestroClient.ConnectAsync("tls://127.0.0.1:" + port, opts))
            Eq((await ip.StatsAsync())["tls"], true, "IP address SAN");
        var wrong = await ThrowsAsync<KestroIOException>(() => KestroClient.ConnectAsync(address, new KestroOptions { Token = token, CaFile = wrongCa }), "wrong CA");
        Ok(wrong.Message.Contains("TLS handshake failed", StringComparison.Ordinal), "handshake message: " + wrong.Message);
        Throws<KestroIOException>(() => KestroClient.Connect(address, new KestroOptions { Token = token, CaFile = wrongCa }), "wrong CA (blocking)");
        await ThrowsAsync<KestroIOException>(() => KestroClient.ConnectAsync(address, new KestroOptions { Token = token }), "the system store does not know the test CA");
        await ThrowsAsync<KestroIOException>(() => KestroClient.ConnectAsync(address, new KestroOptions { Token = token, CaFile = ca, ServerName = "other.example" }), "wrong host name");
        await ThrowsAsync<KestroIOException>(() => KestroClient.ConnectAsync(address, new KestroOptions { Token = token, CaFile = "/no/such/ca.pem" }), "missing CA file");
        await using (var insecure = await KestroClient.ConnectAsync(address, new KestroOptions { Token = token, Verify = false }))
            Eq((await insecure.StatsAsync())["tls"], true, "Verify = false");
        int calls = 0;
        await using (var custom = await KestroClient.ConnectAsync(address, new KestroOptions { Token = token, RemoteCertificateValidation = (_, cert, _, _) => { calls++; return cert is not null; } }))
            Eq((await custom.StatsAsync())["tls"], true, "RemoteCertificateValidation");
        Ok(calls > 0, "the callback was called");
        await ThrowsAsync<KestroIOException>(async () =>
        {
            await using var plain = await KestroClient.ConnectAsync("tcp://localhost:" + port, new KestroOptions { Token = token, Timeout = TimeSpan.FromSeconds(5) });
            await plain.StatsAsync();
        }, "plain TCP to a TLS port");
        using (var conn = new KestroConnection("Address=tls://localhost:" + port + ";Token=" + token + ";CA File=" + ca))
        {
            conn.Open();
            Ok((long)new KestroCommand("SELECT COUNT(*) AS n FROM tl", conn).ExecuteScalar()! >= 1, "ADO.NET over TLS");
        }
        Say("READY");
        ReadLine();               // the TLS server restarts
        await Task.Delay(1500);
        Eq((await db.GetAsync("kv", "tls"))!.Value, M("ok", true), "TLS reconnect after a restart");
        Eq(db.Opens, opened + 1, "reopened once");
        await db.DisposeAsync();
    }

    // ------------------------------------------------------------------ cluster

    private static async Task Cluster(string list, string token, string follower)
    {
        // Only the follower: a write is refused with READONLY naming the leader; reads work.
        await using (var f = await KestroClient.ConnectAsync(follower, new KestroOptions { Token = token }))
        {
            var ro = await ThrowsAsync<ReadOnlyException>(() => f.ExecuteAsync("INSERT INTO t (v) VALUES ('x')"), "a follower refuses writes");
            Eq(ro.Kind, "READONLY", "READONLY kind");
            Ok(ro.Message.Contains("the leader is", StringComparison.Ordinal), "names the leader: " + ro.Message);
            await f.QueryValueAsync("SELECT COUNT(*) FROM t");
        }
        var opts = new KestroOptions { Token = token, Timeout = TimeSpan.FromSeconds(3), FailoverTimeout = TimeSpan.FromSeconds(25) };
        var db = await KestroClient.ConnectAsync(list, opts);
        Eq(db.Address, follower, "starts at the first member");
        for (int i = 0; i < 5; i++) await db.ExecuteAsync("INSERT INTO t (v) VALUES (?)", "a" + i);
        long v = await db.SetAsync("ckv", "k", "v1");
        Eq((await db.GetAsync("ckv", "k"))!.Version, v, "key/value at the leader");
        Say("ADDRESS " + db.Address);
        using (var conn = new KestroConnection("Address=" + list + ";Token=" + token + ";Timeout=3;Failover Timeout=25"))
        {
            conn.Open();
            Eq(new KestroCommand("INSERT INTO t (v) VALUES ('ado')", conn).ExecuteNonQuery(), 1, "ADO.NET follows the leader");
            Eq(conn.Client!.Address, db.Address, "ADO.NET at the leader");
        }
        Say("READY");
        ReadLine();               // the leader is killed and a new one elected
        await Task.Delay(1500);
        for (int i = 0; i < 5; i++) await db.ExecuteAsync("INSERT INTO t (v) VALUES (?)", "b" + i);
        Eq((await db.GetAsync("ckv", "k"))!.Value, "v1", "key/value survived the failover");
        Say("ADDRESS " + db.Address);
        await db.DisposeAsync();
    }

    private static async Task ClusterNew(string list, string token)
    {
        // Blocking, to cover that path's leader following too.
        using var db = KestroClient.Connect(list.Split(','), new KestroOptions { Token = token, Timeout = TimeSpan.FromSeconds(3), FailoverTimeout = TimeSpan.FromSeconds(25) });
        for (int i = 0; i < 5; i++) db.Execute("INSERT INTO t (v) VALUES (?)", "c" + i);
        Say("ADDRESS " + db.Address);
        await Task.CompletedTask;
    }
}

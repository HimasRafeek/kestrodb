using System;
using System.Buffers.Binary;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Numerics;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace KestroDB;

public sealed partial class KestroClient
{
    private static readonly byte[] EmptyList = { (byte)'[', (byte)']' };

    // ------------------------------------------------------------------ parameters

    /// <summary>
    /// Parameters as JSON. null: none. An array, list or other sequence: positional (<c>?</c>,
    /// <c>?N</c>, <c>$N</c>). A dictionary, an anonymous object or another object with
    /// properties: named (<c>:name</c>, <c>@name</c>; keys may include the leading : or @).
    /// One plain value (a number, text, Guid, date, ...): the single positional parameter.
    /// </summary>
    internal static byte[] EncodeParameters(object? parameters)
    {
        switch (parameters)
        {
            case null:
                return EmptyList;
            case object?[] array:
                return array.Length == 0 ? EmptyList : Json.ToBytes(array);
        }
        if (Json.IsScalar(parameters)) return Json.ToBytes(new[] { parameters });
        var entries = Json.MapEntries(parameters);
        if (entries is not null) return Named(entries);
        if (parameters is IEnumerable list) return Json.ToBytes(list);
        var props = Json.Properties(parameters.GetType());
        if (props.Length == 0 && !Json.IsAnonymous(parameters.GetType()))
            throw new InputException("INPUT parameters must be a list, a dictionary or an object with properties, not " + parameters.GetType().FullName);
        var named = new Row(props.Length);
        foreach (var p in props) named[p.Name] = p.Get(parameters);
        return Json.ToBytes(named);
    }

    private static byte[] Named(IEnumerable<KeyValuePair<string, object?>> entries)
    {
        var m = new Row();
        foreach (var e in entries)
        {
            string k = e.Key ?? throw new InputException("INPUT a parameter name cannot be null");
            if (k.StartsWith(':') || k.StartsWith('@')) k = k.Substring(1);
            m[k] = e.Value;
        }
        return Json.ToBytes(m);
    }

    private static byte[] Positional(object?[]? args) => args is null || args.Length == 0 ? EmptyList : Json.ToBytes(args);

    // ------------------------------------------------------------------ SQL core

    private async ValueTask<SqlResult> RunAsync(string text, byte[] paramsJson, bool async, CancellationToken ct)
    {
        using (await LockAsync(async, ct).ConfigureAwait(false)) return await RunUnlockedAsync(text, paramsJson, async, ct).ConfigureAwait(false);
    }

    private async ValueTask<SqlResult> RunUnlockedAsync(string text, byte[] paramsJson, bool async, CancellationToken ct)
    {
        if (text is null) throw new InputException("INPUT sql text must be a string");
        Json.CheckText(text);
        byte[] t = Encoding.UTF8.GetBytes(text);
        byte[] payload = new byte[4 + t.Length + paramsJson.Length];
        BinaryPrimitives.WriteInt32LittleEndian(payload, t.Length);
        t.CopyTo(payload, 4);
        paramsJson.CopyTo(payload, 4 + t.Length);
        byte[] reply;
        try
        {
            reply = await CallAsync('E', payload, async, ct).ConfigureAwait(false);
        }
        catch (KestroException e)
        {
            // A COMMIT that fails (a conflict) has rolled the transaction back on the server.
            if (e.Kind != "IO" && CommitOrEnd.IsMatch(text))
            {
                _inTransaction = false;
                _implicitSavepoint = null;
            }
            throw;
        }
        var r = new SqlResult(ParseReply(reply, 1));
        Track(text);
        if (r.Raw.ContainsKey("lastInsertId")) Volatile.Write(ref _lastInsertId, r.LastInsertId);
        return r;
    }

    private static object? ParseReply(byte[] reply, int offset)
    {
        try
        {
            return Json.Parse(reply.AsSpan(offset));
        }
        catch (InputException e)
        {
            throw new KestroIOException("IO the server's reply is not valid JSON (" + e.Message + ")", e);
        }
    }

    // BEGIN/COMMIT/ROLLBACK (and savepoints) sent as SQL keep InTransaction right.
    private void Track(string text)
    {
        string[] words = text.Trim().Split((char[]?)null, 4, StringSplitOptions.RemoveEmptyEntries);
        if (words.Length == 0) return;
        string w0 = words[0].ToUpperInvariant();
        if (w0.EndsWith(';')) w0 = w0.Substring(0, w0.Length - 1);
        switch (w0)
        {
            case "BEGIN":
            case "START":
                _inTransaction = true;
                _implicitSavepoint = null;
                break;
            case "COMMIT":
            case "END":
                _inTransaction = false;
                _implicitSavepoint = null;
                break;
            case "ROLLBACK":
                // ROLLBACK TO [SAVEPOINT] name keeps the transaction open.
                if (words.Length < 2 || !words[1].Equals("TO", StringComparison.OrdinalIgnoreCase))
                {
                    _inTransaction = false;
                    _implicitSavepoint = null;
                }
                break;
            case "SAVEPOINT":
                if (!_inTransaction && words.Length >= 2)
                {
                    _inTransaction = true;
                    _implicitSavepoint = Unquote(words[1]);
                }
                break;
            case "RELEASE":
            {
                string? name = words.Length >= 3 && words[1].Equals("SAVEPOINT", StringComparison.OrdinalIgnoreCase) ? words[2] : words.Length >= 2 ? words[1] : null;
                if (name is not null && _implicitSavepoint is not null && Unquote(name).Equals(_implicitSavepoint, StringComparison.OrdinalIgnoreCase))
                {
                    _inTransaction = false;
                    _implicitSavepoint = null;
                }
                break;
            }
        }
    }

    private static string Unquote(string w)
    {
        string s = w.EndsWith(';') ? w.Substring(0, w.Length - 1) : w;
        if (s.Length >= 2 && (s[0] == '"' || s[0] == '`') && s[s.Length - 1] == s[0]) s = s.Substring(1, s.Length - 2);
        return s;
    }

    private static IReadOnlyList<Row> RowsOf(SqlResult r) =>
        r.Rows ?? throw new InputException("INPUT a query needs a statement that returns rows; use Execute for writes");

    private static bool IsSimple(Type t)
    {
        var u = Nullable.GetUnderlyingType(t) ?? t;
        return u.IsPrimitive || u.IsEnum || u == typeof(string) || u == typeof(decimal) || u == typeof(DateTime) || u == typeof(DateTimeOffset)
               || u == typeof(DateOnly) || u == typeof(TimeOnly) || u == typeof(TimeSpan) || u == typeof(Guid) || u == typeof(BigInteger) || u == typeof(Uri);
    }

    /// <summary>A row as T: the row itself (Row, IDictionary, object), the first column (simple types), or an object (System.Text.Json).</summary>
    internal static T? MapRow<T>(Row row)
    {
        if (typeof(T).IsAssignableFrom(typeof(Row))) return (T)(object)row;
        if (IsSimple(typeof(T))) return Json.Convert<T>(row.Count > 0 ? row[0] : null);
        return Json.Convert<T>(row);
    }

    private static List<T> MapRows<T>(IReadOnlyList<Row> rows)
    {
        var list = new List<T>(rows.Count);
        foreach (var r in rows) list.Add(MapRow<T>(r)!);
        return list;
    }

    private static Row? First(IReadOnlyList<Row> rows) => rows.Count == 0 ? null : rows[0];

    private static object? FirstValue(Row? row) => row is null || row.Count == 0 ? null : row[0];

    // ------------------------------------------------------------------ SQL: async

    /// <summary>
    /// One SQL statement (see SQL.md). <paramref name="parameters"/>: null; a list or array for
    /// <c>?</c>, <c>?N</c> and <c>$N</c>; a dictionary or an anonymous object for <c>:name</c> and
    /// <c>@name</c>; or one plain value as the only positional parameter.
    /// </summary>
    public async Task<SqlResult> SqlAsync(string sql, object? parameters = null, CancellationToken cancellationToken = default) =>
        await RunAsync(sql, EncodeParameters(parameters), true, cancellationToken).ConfigureAwait(false);

    public Task<SqlResult> SqlAsync(string sql, CancellationToken cancellationToken) => SqlAsync(sql, null, cancellationToken);

    /// <summary>One SQL statement with positional parameters.</summary>
    public async Task<SqlResult> SqlAsync(string sql, params object?[] args) =>
        await RunAsync(sql, Positional(args), true, default).ConfigureAwait(false);

    /// <summary>All rows of a SELECT (or another statement that returns rows).</summary>
    public async Task<IReadOnlyList<Row>> QueryAsync(string sql, object? parameters = null, CancellationToken cancellationToken = default) =>
        RowsOf(await SqlAsync(sql, parameters, cancellationToken).ConfigureAwait(false));

    public Task<IReadOnlyList<Row>> QueryAsync(string sql, CancellationToken cancellationToken) => QueryAsync(sql, null, cancellationToken);

    public async Task<IReadOnlyList<Row>> QueryAsync(string sql, params object?[] args) => RowsOf(await SqlAsync(sql, args).ConfigureAwait(false));

    /// <summary>All rows as T: simple types take the first column; other types are filled by name (System.Text.Json).</summary>
    public async Task<IReadOnlyList<T>> QueryAsync<T>(string sql, object? parameters = null, CancellationToken cancellationToken = default) =>
        MapRows<T>(await QueryAsync(sql, parameters, cancellationToken).ConfigureAwait(false));

    public Task<IReadOnlyList<T>> QueryAsync<T>(string sql, CancellationToken cancellationToken) => QueryAsync<T>(sql, null, cancellationToken);

    public async Task<IReadOnlyList<T>> QueryAsync<T>(string sql, params object?[] args) => MapRows<T>(await QueryAsync(sql, args).ConfigureAwait(false));

    /// <summary>The first row, or null.</summary>
    public async Task<Row?> QueryOneAsync(string sql, object? parameters = null, CancellationToken cancellationToken = default) =>
        First(await QueryAsync(sql, parameters, cancellationToken).ConfigureAwait(false));

    public Task<Row?> QueryOneAsync(string sql, CancellationToken cancellationToken) => QueryOneAsync(sql, null, cancellationToken);

    public async Task<Row?> QueryOneAsync(string sql, params object?[] args) => First(await QueryAsync(sql, args).ConfigureAwait(false));

    /// <summary>The first row as T, or default.</summary>
    public async Task<T?> QueryOneAsync<T>(string sql, object? parameters = null, CancellationToken cancellationToken = default)
    {
        var row = await QueryOneAsync(sql, parameters, cancellationToken).ConfigureAwait(false);
        return row is null ? default : MapRow<T>(row);
    }

    public Task<T?> QueryOneAsync<T>(string sql, CancellationToken cancellationToken) => QueryOneAsync<T>(sql, null, cancellationToken);

    public async Task<T?> QueryOneAsync<T>(string sql, params object?[] args)
    {
        var row = await QueryOneAsync(sql, args).ConfigureAwait(false);
        return row is null ? default : MapRow<T>(row);
    }

    /// <summary>The first column of the first row, or null.</summary>
    public async Task<object?> QueryValueAsync(string sql, object? parameters = null, CancellationToken cancellationToken = default) =>
        FirstValue(await QueryOneAsync(sql, parameters, cancellationToken).ConfigureAwait(false));

    public Task<object?> QueryValueAsync(string sql, CancellationToken cancellationToken) => QueryValueAsync(sql, null, cancellationToken);

    public async Task<object?> QueryValueAsync(string sql, params object?[] args) => FirstValue(await QueryOneAsync(sql, args).ConfigureAwait(false));

    /// <summary>The first column of the first row as T, or default.</summary>
    public async Task<T?> QueryValueAsync<T>(string sql, object? parameters = null, CancellationToken cancellationToken = default) =>
        Json.Convert<T>(await QueryValueAsync(sql, parameters, cancellationToken).ConfigureAwait(false));

    public Task<T?> QueryValueAsync<T>(string sql, CancellationToken cancellationToken) => QueryValueAsync<T>(sql, null, cancellationToken);

    public async Task<T?> QueryValueAsync<T>(string sql, params object?[] args) => Json.Convert<T>(await QueryValueAsync(sql, args).ConfigureAwait(false));

    /// <summary>INSERT, UPDATE, DELETE or DDL: the number of rows changed.</summary>
    public async Task<long> ExecuteAsync(string sql, object? parameters = null, CancellationToken cancellationToken = default) =>
        (await SqlAsync(sql, parameters, cancellationToken).ConfigureAwait(false)).Changes;

    public Task<long> ExecuteAsync(string sql, CancellationToken cancellationToken) => ExecuteAsync(sql, null, cancellationToken);

    public async Task<long> ExecuteAsync(string sql, params object?[] args) => (await SqlAsync(sql, args).ConfigureAwait(false)).Changes;

    // ------------------------------------------------------------------ SQL: blocking

    /// <summary>One SQL statement (see <see cref="SqlAsync(string, object?, CancellationToken)"/>).</summary>
    public SqlResult Sql(string sql, object? parameters = null) => Wait(RunAsync(sql, EncodeParameters(parameters), false, default));

    public SqlResult Sql(string sql, params object?[] args) => Wait(RunAsync(sql, Positional(args), false, default));

    public IReadOnlyList<Row> Query(string sql, object? parameters = null) => RowsOf(Sql(sql, parameters));

    public IReadOnlyList<Row> Query(string sql, params object?[] args) => RowsOf(Sql(sql, args));

    public IReadOnlyList<T> Query<T>(string sql, object? parameters = null) => MapRows<T>(Query(sql, parameters));

    public IReadOnlyList<T> Query<T>(string sql, params object?[] args) => MapRows<T>(Query(sql, args));

    public Row? QueryOne(string sql, object? parameters = null) => First(Query(sql, parameters));

    public Row? QueryOne(string sql, params object?[] args) => First(Query(sql, args));

    public T? QueryOne<T>(string sql, object? parameters = null) => QueryOne(sql, parameters) is { } row ? MapRow<T>(row) : default;

    public T? QueryOne<T>(string sql, params object?[] args) => QueryOne(sql, args) is { } row ? MapRow<T>(row) : default;

    public object? QueryValue(string sql, object? parameters = null) => FirstValue(QueryOne(sql, parameters));

    public object? QueryValue(string sql, params object?[] args) => FirstValue(QueryOne(sql, args));

    public T? QueryValue<T>(string sql, object? parameters = null) => Json.Convert<T>(QueryValue(sql, parameters));

    public T? QueryValue<T>(string sql, params object?[] args) => Json.Convert<T>(QueryValue(sql, args));

    public long Execute(string sql, object? parameters = null) => Sql(sql, parameters).Changes;

    public long Execute(string sql, params object?[] args) => Sql(sql, args).Changes;

    // ------------------------------------------------------------------ transactions

    private async ValueTask BeginCoreAsync(bool concurrent, bool async, CancellationToken ct)
    {
        using (await LockAsync(async, ct).ConfigureAwait(false))
        {
            if (_inTransaction) throw new InputException("INPUT a transaction is already open");
            await RunUnlockedAsync(concurrent ? "BEGIN CONCURRENT" : "BEGIN", EmptyList, async, ct).ConfigureAwait(false);
        }
    }

    private async ValueTask EndCoreAsync(string sql, bool async, CancellationToken ct)
    {
        using (await LockAsync(async, ct).ConfigureAwait(false))
        {
            if (_inTransaction) await RunUnlockedAsync(sql, EmptyList, async, ct).ConfigureAwait(false);
        }
    }

    /// <summary>
    /// BEGIN (or BEGIN CONCURRENT: optimistic, the first committer wins and a later conflicting
    /// COMMIT fails with <see cref="ConflictException"/>). A transaction holds the write gate until COMMIT or ROLLBACK.
    /// </summary>
    public Task BeginAsync(bool concurrent = false, CancellationToken cancellationToken = default) => BeginCoreAsync(concurrent, true, cancellationToken).AsTask();

    /// <summary>COMMIT; nothing to do without an open transaction. A CONFLICT here rolled it back.</summary>
    public Task CommitAsync(CancellationToken cancellationToken = default) => EndCoreAsync("COMMIT", true, cancellationToken).AsTask();

    /// <summary>ROLLBACK; nothing to do without an open transaction.</summary>
    public Task RollbackAsync(CancellationToken cancellationToken = default) => EndCoreAsync("ROLLBACK", true, cancellationToken).AsTask();

    public void Begin(bool concurrent = false) => Wait(BeginCoreAsync(concurrent, false, default));

    public void Commit() => Wait(EndCoreAsync("COMMIT", false, default));

    public void Rollback() => Wait(EndCoreAsync("ROLLBACK", false, default));

    /// <summary>
    /// Runs <paramref name="body"/> inside BEGIN ... COMMIT; on an exception: ROLLBACK and rethrow.
    /// Other callers of this client wait until it ends.
    /// </summary>
    public async Task<T> TransactionAsync<T>(Func<KestroClient, Task<T>> body, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(body);
        bool owner = OwnsGate;
        object token = new();
        object? previous = _owner.Value;
        if (!owner) await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (!owner)
            {
                Volatile.Write(ref _gateOwner, token);
                _owner.Value = token;
            }
            await BeginCoreAsync(false, true, cancellationToken).ConfigureAwait(false);
            try
            {
                T result = await body(this).ConfigureAwait(false);
                await EndCoreAsync("COMMIT", true, cancellationToken).ConfigureAwait(false);
                return result;
            }
            catch
            {
                if (_inTransaction)
                {
                    try { await EndCoreAsync("ROLLBACK", true, CancellationToken.None).ConfigureAwait(false); }
                    catch (KestroException) { /* the body's exception matters; the connection may be gone */ }
                }
                throw;
            }
        }
        finally
        {
            if (!owner)
            {
                Volatile.Write(ref _gateOwner, null);
                _owner.Value = previous;
                _gate.Release();
            }
        }
    }

    /// <summary>As <see cref="TransactionAsync{T}"/>, without a result.</summary>
    public Task TransactionAsync(Func<KestroClient, Task> body, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(body);
        return TransactionAsync<bool>(async db => { await body(db).ConfigureAwait(false); return true; }, cancellationToken);
    }

    /// <summary>Runs <paramref name="body"/> (blocking) inside BEGIN ... COMMIT; on an exception: ROLLBACK and rethrow.</summary>
    public T Transaction<T>(Func<KestroClient, T> body)
    {
        ArgumentNullException.ThrowIfNull(body);
        bool owner = OwnsGate;
        object token = new();
        object? previous = _owner.Value;
        if (!owner) _gate.Wait();
        try
        {
            if (!owner)
            {
                Volatile.Write(ref _gateOwner, token);
                _owner.Value = token;
            }
            Begin();
            try
            {
                T result = body(this);
                Commit();
                return result;
            }
            catch
            {
                if (_inTransaction)
                {
                    try { Rollback(); }
                    catch (KestroException) { /* the body's exception matters */ }
                }
                throw;
            }
        }
        finally
        {
            if (!owner)
            {
                Volatile.Write(ref _gateOwner, null);
                _owner.Value = previous;
                _gate.Release();
            }
        }
    }

    /// <summary>As <see cref="Transaction{T}"/>, without a result.</summary>
    public void Transaction(Action<KestroClient> body)
    {
        ArgumentNullException.ThrowIfNull(body);
        Transaction<bool>(db => { body(db); return true; });
    }

    // ------------------------------------------------------------------ key/value

    private static byte[] Id(string collection, string key)
    {
        if (collection is null || !ValidCollection(collection))
            throw new InputException("INPUT Collection must be a 1-64 character ASCII identifier.");
        if (key is null) throw new InputException("INPUT Key must be a string.");
        Json.CheckText(key);
        byte[] k = Encoding.UTF8.GetBytes(key);
        if (k.Length == 0 || k.Length > 512 || Array.IndexOf(k, (byte)0) >= 0)
            throw new InputException("INPUT Key must contain 1-512 UTF-8 bytes and no NUL.");
        byte[] outBytes = new byte[collection.Length + 1 + k.Length];
        Encoding.ASCII.GetBytes(collection, 0, collection.Length, outBytes, 0);
        k.CopyTo(outBytes, collection.Length + 1);
        return outBytes;
    }

    private static bool ValidCollection(string c)
    {
        if (c.Length < 1 || c.Length > 64) return false;
        for (int i = 0; i < c.Length; i++)
        {
            char ch = c[i];
            bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_' || (i > 0 && ch >= '0' && ch <= '9');
            if (!ok) return false;
        }
        return true;
    }

    private async ValueTask<byte[]> LockedCallAsync(char command, byte[] payload, bool async, CancellationToken ct)
    {
        using (await LockAsync(async, ct).ConfigureAwait(false)) return await CallAsync(command, payload, async, ct).ConfigureAwait(false);
    }

    private async ValueTask<Versioned?> GetCoreAsync(string collection, string key, bool async, CancellationToken ct)
    {
        byte[] reply = await LockedCallAsync('G', Id(collection, key), async, ct).ConfigureAwait(false);
        if (reply[0] == 1) return null;
        if (reply.Length < 9) throw new KestroIOException("IO short reply");
        return new Versioned(BinaryPrimitives.ReadInt64LittleEndian(reply.AsSpan(1)), ParseReply(reply, 9));
    }

    private async ValueTask<IReadOnlyList<Versioned?>> GetManyCoreAsync(string collection, IEnumerable<string> keys, bool async, CancellationToken ct)
    {
        if (keys is null) throw new InputException("INPUT keys");
        var list = new List<string>(keys);
        if (list.Count > 4096) throw new LimitException("LIMIT getMany count");
        using var b = new MemoryStream();
        byte[] n = new byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(n, list.Count);
        b.Write(n);
        foreach (var k in list)
        {
            byte[] id = Id(collection, k);
            BinaryPrimitives.WriteInt32LittleEndian(n, id.Length);
            b.Write(n);
            b.Write(id);
        }
        byte[] reply = await LockedCallAsync('M', b.ToArray(), async, ct).ConfigureAwait(false);
        try
        {
            int count = BinaryPrimitives.ReadInt32LittleEndian(reply.AsSpan(1)), p = 5;
            if (count != list.Count) throw new KestroIOException("IO getMany reply count");
            var result = new List<Versioned?>(count);
            for (int i = 0; i < count; i++)
            {
                if (reply[p++] == 0)
                {
                    result.Add(null);
                    continue;
                }
                long version = BinaryPrimitives.ReadInt64LittleEndian(reply.AsSpan(p));
                int length = BinaryPrimitives.ReadInt32LittleEndian(reply.AsSpan(p + 8));
                p += 12;
                result.Add(new Versioned(version, ParseReply(reply.AsSpan(p, length).ToArray(), 0)));
                p += length;
            }
            return result;
        }
        catch (Exception e) when (e is IndexOutOfRangeException or ArgumentOutOfRangeException)
        {
            throw new KestroIOException("IO malformed getMany reply", e);
        }
    }

    private async ValueTask<long> BatchCoreAsync(IEnumerable<Op> operations, bool async, CancellationToken ct)
    {
        var ops = operations is null ? new List<Op>() : new List<Op>(operations);
        if (ops.Count == 0 || ops.Count > 65536) throw new InputException("INPUT operation count");
        using var b = new MemoryStream();
        byte[] h = new byte[17];
        BinaryPrimitives.WriteInt32LittleEndian(h, ops.Count);
        b.Write(h, 0, 4);
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var op in ops)
        {
            if (op is null) throw new InputException("INPUT operation type");
            byte[] id = Id(op.Collection, op.Key);
            if (!seen.Add(op.Collection + "\0" + op.Key)) throw new InputException("INPUT duplicate transaction key");
            byte[] v = op.IsSet ? EncodeValue(op.Value) : NoBytes;
            h[0] = (byte)(op.IsSet ? 1 : 0);
            BinaryPrimitives.WriteInt64LittleEndian(h.AsSpan(1), op.ExpectedVersion ?? -1L);
            BinaryPrimitives.WriteInt32LittleEndian(h.AsSpan(9), id.Length);
            BinaryPrimitives.WriteInt32LittleEndian(h.AsSpan(13), v.Length);
            b.Write(h, 0, 17);
            b.Write(id);
            b.Write(v);
            if (b.Length > MaxFrame - 64) throw new LimitException("LIMIT transaction bytes");
        }
        byte[] reply = await LockedCallAsync('B', b.ToArray(), async, ct).ConfigureAwait(false);
        if (reply.Length < 9) throw new KestroIOException("IO short reply");
        return BinaryPrimitives.ReadInt64LittleEndian(reply.AsSpan(1));
    }

    private static byte[] EncodeValue(object? value)
    {
        byte[] v = Json.ToBytes(value);
        if (v.Length > MaxValue) throw new LimitException("LIMIT A value may contain at most 256 KiB of JSON.");
        return v;
    }

    /// <summary>The value and version of a key, or null when it does not exist.</summary>
    public Task<Versioned?> GetAsync(string collection, string key, CancellationToken cancellationToken = default) =>
        GetCoreAsync(collection, key, true, cancellationToken).AsTask();

    /// <summary>Up to 4096 keys read in one consistent snapshot, in order; null where a key is missing.</summary>
    public Task<IReadOnlyList<Versioned?>> GetManyAsync(string collection, IEnumerable<string> keys, CancellationToken cancellationToken = default) =>
        GetManyCoreAsync(collection, keys, true, cancellationToken).AsTask();

    public Task<IReadOnlyList<Versioned?>> GetManyAsync(string collection, params string[] keys) => GetManyCoreAsync(collection, keys, true, default).AsTask();

    /// <summary>
    /// Stores a JSON value; returns its new version. With <paramref name="expectedVersion"/>: only if
    /// the key is at that version (0: it must not exist), else <see cref="ConflictException"/>.
    /// </summary>
    public Task<long> SetAsync(string collection, string key, object? value, long? expectedVersion = null, CancellationToken cancellationToken = default) =>
        BatchCoreAsync(new[] { Op.Set(collection, key, value, expectedVersion) }, true, cancellationToken).AsTask();

    /// <summary>Deletes a key (deleting a missing key is not an error); returns the transaction's version.</summary>
    public Task<long> DeleteAsync(string collection, string key, long? expectedVersion = null, CancellationToken cancellationToken = default) =>
        BatchCoreAsync(new[] { Op.Delete(collection, key, expectedVersion) }, true, cancellationToken).AsTask();

    /// <summary>All-or-nothing and durable when it returns; the version shared by every operation.</summary>
    public Task<long> BatchAsync(IEnumerable<Op> operations, CancellationToken cancellationToken = default) =>
        BatchCoreAsync(operations, true, cancellationToken).AsTask();

    public Task<long> BatchAsync(params Op[] operations) => BatchCoreAsync(operations, true, default).AsTask();

    public Versioned? Get(string collection, string key) => Wait(GetCoreAsync(collection, key, false, default));

    public IReadOnlyList<Versioned?> GetMany(string collection, IEnumerable<string> keys) => Wait(GetManyCoreAsync(collection, keys, false, default));

    public IReadOnlyList<Versioned?> GetMany(string collection, params string[] keys) => Wait(GetManyCoreAsync(collection, keys, false, default));

    public long Set(string collection, string key, object? value, long? expectedVersion = null) =>
        Wait(BatchCoreAsync(new[] { Op.Set(collection, key, value, expectedVersion) }, false, default));

    public long Delete(string collection, string key, long? expectedVersion = null) =>
        Wait(BatchCoreAsync(new[] { Op.Delete(collection, key, expectedVersion) }, false, default));

    public long Batch(IEnumerable<Op> operations) => Wait(BatchCoreAsync(operations, false, default));

    public long Batch(params Op[] operations) => Wait(BatchCoreAsync(operations, false, default));

    // ------------------------------------------------------------------ server

    private async ValueTask<Row> ObjectCallAsync(char command, byte[] payload, bool async, CancellationToken ct)
    {
        byte[] reply = await LockedCallAsync(command, payload, async, ct).ConfigureAwait(false);
        return ParseReply(reply, 1) as Row ?? throw new KestroIOException("IO the server's reply is not a JSON object");
    }

    private static byte[] BackupName(string name)
    {
        if (string.IsNullOrEmpty(name)) throw new InputException("INPUT a backup name is a plain file name");
        return Encoding.UTF8.GetBytes(name);
    }

    /// <summary>Server statistics: keys, bytes, txid, connections, tls, user, role, cluster, ...</summary>
    public Task<Row> StatsAsync(CancellationToken cancellationToken = default) => ObjectCallAsync('S', NoBytes, true, cancellationToken).AsTask();

    /// <summary>Checks every checksum of the database file (admin).</summary>
    public async Task VerifyAsync(CancellationToken cancellationToken = default) => await LockedCallAsync('V', NoBytes, true, cancellationToken).ConfigureAwait(false);

    /// <summary>Online backup to a plain file name in the server's --backup-dir (admin).</summary>
    public async Task BackupAsync(string name, CancellationToken cancellationToken = default) => await LockedCallAsync('P', BackupName(name), true, cancellationToken).ConfigureAwait(false);

    /// <summary>A compacted copy as a plain file name in the server's --backup-dir (admin).</summary>
    public async Task CompactToAsync(string name, CancellationToken cancellationToken = default) => await LockedCallAsync('C', BackupName(name), true, cancellationToken).ConfigureAwait(false);

    /// <summary>Online compaction of the server's file (admin): {before, after, keys}.</summary>
    public Task<Row> CompactAsync(CancellationToken cancellationToken = default) => ObjectCallAsync('K', NoBytes, true, cancellationToken).AsTask();

    /// <summary>Turns a replica into a writable server (admin); it stops following its leader.</summary>
    public async Task PromoteAsync(CancellationToken cancellationToken = default) => await LockedCallAsync('X', NoBytes, true, cancellationToken).ConfigureAwait(false);

    public Row Stats() => Wait(ObjectCallAsync('S', NoBytes, false, default));

    public void Verify() => Wait(LockedCallAsync('V', NoBytes, false, default));

    public void Backup(string name) => Wait(LockedCallAsync('P', BackupName(name), false, default));

    public void CompactTo(string name) => Wait(LockedCallAsync('C', BackupName(name), false, default));

    public Row Compact() => Wait(ObjectCallAsync('K', NoBytes, false, default));

    public void Promote() => Wait(LockedCallAsync('X', NoBytes, false, default));
}

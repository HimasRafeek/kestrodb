using System;
using System.Collections.Generic;

namespace KestroDB;

/// <summary>
/// The result of one SQL statement: <see cref="Rows"/> for a statement that returns rows (SELECT,
/// SHOW, EXPLAIN, ... and writes with RETURNING), or <see cref="Changes"/> and
/// <see cref="LastInsertId"/> for a write.
/// </summary>
public sealed class SqlResult
{
    private static readonly IReadOnlyList<string> NoColumns = Array.Empty<string>();
    private IReadOnlyList<string>? _columns;

    internal SqlResult(object? parsed)
    {
        if (parsed is not Row raw) throw new KestroIOException("IO the server's SQL result is not a JSON object");
        Raw = raw;
        if (raw["rows"] is List<object?> list)
        {
            var rows = new List<Row>(list.Count);
            foreach (var r in list)
            {
                if (r is not Row row) throw new KestroIOException("IO the server's SQL result has a row that is not a JSON object");
                rows.Add(row);
            }
            Rows = rows;
        }
    }

    /// <summary>True when the statement returned rows (even zero of them).</summary>
    public bool HasRows => Rows is not null;

    /// <summary>The rows, each an ordered map from column name to value; null for a statement without rows.</summary>
    public IReadOnlyList<Row>? Rows { get; }

    /// <summary>
    /// The column names in SELECT order: every name of every row, in the order first seen (rows
    /// are JSON documents, so a schemaless table's rows may differ). Empty without rows.
    /// </summary>
    public IReadOnlyList<string> Columns => _columns ??= ColumnsOf(Rows);

    /// <summary>Rows changed by a write (0 when the result has none).</summary>
    public long Changes => Raw["changes"] switch
    {
        long l => l,
        double d => (long)d,
        _ => 0,
    };

    /// <summary>The id of the row an INSERT created (a long, or a string for a text id), or null.</summary>
    public object? LastInsertId => Raw["lastInsertId"];

    /// <summary>The whole result as the server sent it.</summary>
    public Row Raw { get; }

    public override string ToString() => Raw.ToString();

    internal static IReadOnlyList<string> ColumnsOf(IReadOnlyList<Row>? rows)
    {
        if (rows is null || rows.Count == 0) return NoColumns;
        var names = new List<string>(rows[0].Keys);
        HashSet<string>? seen = null;
        for (int i = 1; i < rows.Count; i++)
        {
            var keys = rows[i].Keys;
            bool same = keys.Count == names.Count;
            for (int k = 0; same && k < keys.Count; k++) same = keys[k] == names[k];
            if (same) continue;
            seen ??= new HashSet<string>(names, StringComparer.Ordinal);
            foreach (var key in keys) if (seen.Add(key)) names.Add(key);
        }
        return names;
    }
}

/// <summary>A stored key/value: its version (the transaction that wrote it) and its decoded JSON value.</summary>
public sealed record Versioned(long Version, object? Value)
{
    /// <summary>The value as <typeparamref name="T"/> (see <see cref="Json.Convert{T}"/>).</summary>
    public T? As<T>() => Json.Convert<T>(Value);

    public bool Equals(Versioned? other) => other is not null && other.Version == Version && Row.DeepEquals(Value, other.Value);

    public override int GetHashCode() => Version.GetHashCode();

    public override string ToString() => "{\"version\":" + Version + ",\"value\":" + Json.Stringify(Value) + "}";
}

/// <summary>
/// One operation of an all-or-nothing <see cref="KestroClient.BatchAsync(IEnumerable{Op}, System.Threading.CancellationToken)"/>.
/// An expected version makes it conditional: the whole batch fails with <see cref="ConflictException"/>
/// unless the key is at that version (0: the key must not exist).
/// </summary>
public sealed class Op
{
    private Op(bool isSet, string collection, string key, object? value, long? expectedVersion)
    {
        if (expectedVersion is < 0) throw new InputException("INPUT expectedVersion must be 0 or more");
        IsSet = isSet;
        Collection = collection;
        Key = key;
        Value = value;
        ExpectedVersion = expectedVersion;
    }

    /// <summary>Stores a JSON value; with <paramref name="expectedVersion"/>, only if the key is at that version (0: must not exist).</summary>
    public static Op Set(string collection, string key, object? value, long? expectedVersion = null) => new(true, collection, key, value, expectedVersion);

    /// <summary>Deletes a key (a missing key is not an error); with <paramref name="expectedVersion"/>, only at that version.</summary>
    public static Op Delete(string collection, string key, long? expectedVersion = null) => new(false, collection, key, null, expectedVersion);

    public bool IsSet { get; }
    public string Collection { get; }
    public string Key { get; }
    public object? Value { get; }

    /// <summary>The required version, or null for none.</summary>
    public long? ExpectedVersion { get; }
}

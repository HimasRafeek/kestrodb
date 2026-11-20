using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Text.Json;

namespace KestroDB;

/// <summary>
/// An ordered map from names to values: a SQL row (columns in SELECT order), and also every JSON
/// object decoded from the server (nested values, key/value values, stats). Values are
/// <c>null</c>, <see cref="bool"/>, <see cref="long"/> (integral numbers), <see cref="double"/>,
/// <see cref="System.Numerics.BigInteger"/> (integers beyond the long range), <see cref="string"/>,
/// <see cref="List{T}"/> of object (JSON arrays) or <see cref="Row"/> (JSON objects).
/// Looking up a missing name with the indexer returns null.
/// </summary>
public sealed class Row : IDictionary<string, object?>, IReadOnlyDictionary<string, object?>
{
    private readonly List<string> _keys;
    private readonly List<object?> _values;
    private readonly Dictionary<string, int> _index;

    public Row() : this(4) { }

    public Row(int capacity)
    {
        _keys = new List<string>(capacity);
        _values = new List<object?>(capacity);
        _index = new Dictionary<string, int>(capacity, StringComparer.Ordinal);
    }

    /// <summary>A row with these entries, in order.</summary>
    public Row(IEnumerable<KeyValuePair<string, object?>> entries) : this()
    {
        foreach (var e in entries) this[e.Key] = e.Value;
    }

    /// <summary>The value of a name, or null when the row has no such name (use <see cref="ContainsKey"/> to tell them apart).</summary>
    public object? this[string key]
    {
        get => _index.TryGetValue(key, out int i) ? _values[i] : null;
        set
        {
            ArgumentNullException.ThrowIfNull(key);
            if (_index.TryGetValue(key, out int i)) _values[i] = value;
            else { _index.Add(key, _keys.Count); _keys.Add(key); _values.Add(value); }
        }
    }

    /// <summary>The value at a position (columns in SELECT order).</summary>
    public object? this[int ordinal] => _values[ordinal];

    /// <summary>The value converted to <typeparamref name="T"/> (long to int, text to Guid or DateTime, ...).</summary>
    public T? Get<T>(string key) => Json.Convert<T>(this[key]);

    /// <summary>The names in order.</summary>
    public IReadOnlyList<string> Keys => _keys;

    /// <summary>The values in order.</summary>
    public IReadOnlyList<object?> Values => _values;

    public int Count => _keys.Count;

    public bool ContainsKey(string key) => _index.ContainsKey(key);

    public bool TryGetValue(string key, [MaybeNullWhen(false)] out object? value)
    {
        if (_index.TryGetValue(key, out int i)) { value = _values[i]; return true; }
        value = null;
        return false;
    }

    public void Add(string key, object? value)
    {
        ArgumentNullException.ThrowIfNull(key);
        if (_index.ContainsKey(key)) throw new ArgumentException("an entry with the name " + key + " already exists", nameof(key));
        this[key] = value;
    }

    public bool Remove(string key)
    {
        if (!_index.TryGetValue(key, out int i)) return false;
        _keys.RemoveAt(i);
        _values.RemoveAt(i);
        _index.Remove(key);
        for (int k = i; k < _keys.Count; k++) _index[_keys[k]] = k;
        return true;
    }

    public void Clear()
    {
        _keys.Clear();
        _values.Clear();
        _index.Clear();
    }

    /// <summary>This row as an object of type <typeparamref name="T"/> (System.Text.Json, names matched without regard to case).</summary>
    public T? As<T>() => Json.Convert<T>(this);

    /// <summary>The JSON text of this object.</summary>
    public override string ToString() => Json.Stringify(this);

    public IEnumerator<KeyValuePair<string, object?>> GetEnumerator()
    {
        for (int i = 0; i < _keys.Count; i++) yield return new KeyValuePair<string, object?>(_keys[i], _values[i]);
    }

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    ICollection<string> IDictionary<string, object?>.Keys => _keys.AsReadOnly();
    ICollection<object?> IDictionary<string, object?>.Values => _values.AsReadOnly();
    IEnumerable<string> IReadOnlyDictionary<string, object?>.Keys => _keys;
    IEnumerable<object?> IReadOnlyDictionary<string, object?>.Values => _values;
    bool ICollection<KeyValuePair<string, object?>>.IsReadOnly => false;
    void ICollection<KeyValuePair<string, object?>>.Add(KeyValuePair<string, object?> item) => Add(item.Key, item.Value);

    bool ICollection<KeyValuePair<string, object?>>.Contains(KeyValuePair<string, object?> item) =>
        _index.TryGetValue(item.Key, out int i) && Equals(_values[i], item.Value);

    void ICollection<KeyValuePair<string, object?>>.CopyTo(KeyValuePair<string, object?>[] array, int arrayIndex)
    {
        for (int i = 0; i < _keys.Count; i++) array[arrayIndex + i] = new KeyValuePair<string, object?>(_keys[i], _values[i]);
    }

    bool ICollection<KeyValuePair<string, object?>>.Remove(KeyValuePair<string, object?> item) =>
        ((ICollection<KeyValuePair<string, object?>>)this).Contains(item) && Remove(item.Key);

    /// <summary>Deep equality of two decoded JSON values (rows, lists, numbers compared by value).</summary>
    public static bool DeepEquals(object? a, object? b)
    {
        if (a is null || b is null) return a is null && b is null;
        if (a is Row ra && b is Row rb)
        {
            if (ra.Count != rb.Count) return false;
            for (int i = 0; i < ra.Count; i++)
            {
                if (!rb.TryGetValue(ra._keys[i], out var v) || !DeepEquals(ra._values[i], v)) return false;
            }
            return true;
        }
        if (a is IList la && b is IList lb && a is not string && b is not string)
        {
            if (la.Count != lb.Count) return false;
            for (int i = 0; i < la.Count; i++) if (!DeepEquals(la[i], lb[i])) return false;
            return true;
        }
        if (a is JsonElement || b is JsonElement) return Json.Stringify(a) == Json.Stringify(b);
        return a.Equals(b);
    }
}

using System;
using System.Collections;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;

namespace KestroDB;

/// <summary>
/// JSON as KestroDB stores it. Writing: null/DBNull, bool, every .NET number type (NaN and
/// infinities are refused), string and char, enums (their name), Guid, DateTime / DateTimeOffset /
/// DateOnly / TimeOnly / TimeSpan (ISO 8601 text), JsonElement / JsonNode (as they are),
/// dictionaries (objects), other enumerables (arrays), and anonymous types, records and other
/// classes (objects of their public properties; [JsonPropertyName] and [JsonIgnore] apply).
/// byte[] is refused (encode it first). Reading: see <see cref="Row"/>.
/// </summary>
public static class Json
{
    private const int MaxDepth = 1000;

    /// <summary>Options used to turn rows and values into your own types: names match without regard to case.</summary>
    public static JsonSerializerOptions SerializerOptions { get; } = new JsonSerializerOptions
    {
        PropertyNameCaseInsensitive = true,
        NumberHandling = JsonNumberHandling.AllowReadingFromString,
    };

    // ------------------------------------------------------------------ writing

    /// <summary>The JSON text of a value (see the class comment for the types accepted).</summary>
    public static string Stringify(object? value)
    {
        var sb = new StringBuilder();
        new Writer(sb).Write(value, 0);
        return sb.ToString();
    }

    /// <summary>The UTF-8 bytes of <see cref="Stringify"/>.</summary>
    public static byte[] ToBytes(object? value) => Encoding.UTF8.GetBytes(Stringify(value));

    internal static void Write(StringBuilder sb, object? value) => new Writer(sb).Write(value, 0);

    private readonly struct Writer
    {
        private readonly StringBuilder _sb;
        private readonly List<object> _stack;

        public Writer(StringBuilder sb)
        {
            _sb = sb;
            _stack = new List<object>();
        }

        public void Write(object? v, int depth)
        {
            if (depth > MaxDepth) throw new InputException("INPUT a value is nested too deeply (or contains itself)");
            switch (v)
            {
                case null:
                case DBNull:
                    _sb.Append("null");
                    return;
                case string s:
                    WriteString(_sb, s);
                    return;
                case bool b:
                    _sb.Append(b ? "true" : "false");
                    return;
                case char c:
                    WriteString(_sb, c.ToString());
                    return;
                case int or long or short or sbyte or byte or ushort or uint or ulong or decimal or BigInteger or Int128 or UInt128:
                    _sb.Append(((IFormattable)v).ToString(null, CultureInfo.InvariantCulture));
                    return;
                case double d:
                    WriteDouble(d);
                    return;
                case float f:
                    if (!float.IsFinite(f)) throw new InputException("INPUT a number must be finite (not NaN or Infinity)");
                    _sb.Append(f.ToString("R", CultureInfo.InvariantCulture));
                    return;
                case Half h:
                    WriteDouble((double)h);
                    return;
                case Enum e:
                    WriteString(_sb, e.ToString());
                    return;
                case Guid g:
                    WriteString(_sb, g.ToString("D"));
                    return;
                case DateTime dt:
                    WriteString(_sb, dt.ToString("yyyy-MM-dd'T'HH:mm:ss.FFFFFFFK", CultureInfo.InvariantCulture));
                    return;
                case DateTimeOffset dto:
                    WriteString(_sb, dto.ToString("yyyy-MM-dd'T'HH:mm:ss.FFFFFFFzzz", CultureInfo.InvariantCulture));
                    return;
                case DateOnly date:
                    WriteString(_sb, date.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture));
                    return;
                case TimeOnly time:
                    WriteString(_sb, time.ToString("HH:mm:ss.FFFFFFF", CultureInfo.InvariantCulture));
                    return;
                case TimeSpan span:
                    WriteString(_sb, span.ToString("c", CultureInfo.InvariantCulture));
                    return;
                case Uri uri:
                    WriteString(_sb, uri.ToString());
                    return;
                case byte[] or Memory<byte> or ReadOnlyMemory<byte>:
                    throw new InputException("INPUT bytes are not a JSON value: encode them first (for example Convert.ToBase64String)");
                case JsonElement el:
                    _sb.Append(el.ValueKind == JsonValueKind.Undefined ? "null" : el.GetRawText());
                    return;
                case JsonDocument doc:
                    _sb.Append(doc.RootElement.GetRawText());
                    return;
                case JsonNode node:
                    _sb.Append(node.ToJsonString());
                    return;
            }
            Enter(v);
            try
            {
                if (TryWriteMap(v, depth)) return;
                if (v is IEnumerable list)
                {
                    _sb.Append('[');
                    bool first = true;
                    foreach (var item in list)
                    {
                        if (!first) _sb.Append(',');
                        first = false;
                        Write(item, depth + 1);
                    }
                    _sb.Append(']');
                    return;
                }
                WriteProperties(v, depth);
            }
            finally
            {
                _stack.RemoveAt(_stack.Count - 1);
            }
        }

        private void Enter(object v)
        {
            foreach (var o in _stack)
                if (ReferenceEquals(o, v)) throw new InputException("INPUT a value contains itself (a cycle) and cannot be stored as JSON");
            _stack.Add(v);
        }

        private void WriteDouble(double d)
        {
            if (!double.IsFinite(d)) throw new InputException("INPUT a number must be finite (not NaN or Infinity)");
            _sb.Append(d.ToString("R", CultureInfo.InvariantCulture));
        }

        private bool TryWriteMap(object v, int depth)
        {
            var entries = MapEntries(v);
            if (entries is null) return false;
            _sb.Append('{');
            bool first = true;
            foreach (var e in entries)
            {
                if (!first) _sb.Append(',');
                first = false;
                WriteString(_sb, e.Key);
                _sb.Append(':');
                Write(e.Value, depth + 1);
            }
            _sb.Append('}');
            return true;
        }

        private void WriteProperties(object v, int depth)
        {
            var props = Properties(v.GetType());
            if (props.Length == 0 && !IsAnonymous(v.GetType()))
                throw new InputException("INPUT a value of type " + v.GetType().FullName + " cannot be stored as JSON");
            _sb.Append('{');
            for (int i = 0; i < props.Length; i++)
            {
                if (i > 0) _sb.Append(',');
                WriteString(_sb, props[i].Name);
                _sb.Append(':');
                Write(props[i].Get(v), depth + 1);
            }
            _sb.Append('}');
        }
    }

    private static string KeyText(object? key) => key switch
    {
        null => throw new InputException("INPUT a JSON object key cannot be null"),
        string s => s,
        IFormattable f => f.ToString(null, CultureInfo.InvariantCulture),
        _ => key.ToString() ?? "",
    };

    internal static void WriteString(StringBuilder sb, string s)
    {
        sb.Append('"');
        for (int i = 0; i < s.Length; i++)
        {
            char c = s[i];
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                case '\b': sb.Append("\\b"); break;
                case '\f': sb.Append("\\f"); break;
                default:
                    if (c < 0x20)
                    {
                        sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                    }
                    else if (char.IsHighSurrogate(c))
                    {
                        if (i + 1 >= s.Length || !char.IsLowSurrogate(s[i + 1])) throw LoneSurrogate();
                        sb.Append(c).Append(s[++i]);
                    }
                    else if (char.IsLowSurrogate(c))
                    {
                        throw LoneSurrogate();
                    }
                    else sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
    }

    private static InputException LoneSurrogate() => new("INPUT a string contains a lone surrogate (invalid UTF-16)");

    /// <summary>Checks a string is valid UTF-16 (no lone surrogates).</summary>
    internal static void CheckText(string s)
    {
        for (int i = 0; i < s.Length; i++)
        {
            char c = s[i];
            if (char.IsHighSurrogate(c))
            {
                if (i + 1 >= s.Length || !char.IsLowSurrogate(s[i + 1])) throw LoneSurrogate();
                i++;
            }
            else if (char.IsLowSurrogate(c)) throw LoneSurrogate();
        }
    }

    // Public readable instance properties of a class (cached), honouring [JsonIgnore] / [JsonPropertyName].
    internal readonly record struct Prop(string Name, Func<object, object?> Get);

    private static readonly ConcurrentDictionary<Type, Prop[]> s_props = new();
    private static readonly ConcurrentDictionary<Type, (PropertyInfo Key, PropertyInfo Value)?> s_pairs = new();

    internal static Prop[] Properties(Type t) => s_props.GetOrAdd(t, static type =>
    {
        var list = new List<Prop>();
        foreach (var p in type.GetProperties(BindingFlags.Public | BindingFlags.Instance))
        {
            if (!p.CanRead || p.GetIndexParameters().Length > 0 || p.GetMethod is null || !p.GetMethod.IsPublic) continue;
            var ignore = p.GetCustomAttribute<JsonIgnoreAttribute>();
            if (ignore is not null && ignore.Condition == JsonIgnoreCondition.Always) continue;
            string name = p.GetCustomAttribute<JsonPropertyNameAttribute>()?.Name ?? p.Name;
            var prop = p;
            list.Add(new Prop(name, o => prop.GetValue(o)));
        }
        return list.ToArray();
    });

    // A type that enumerates KeyValuePair<K, V> (a dictionary without the non-generic IDictionary).
    private static (PropertyInfo Key, PropertyInfo Value)? PairAccessors(Type t) => s_pairs.GetOrAdd(t, static type =>
    {
        foreach (var i in type.GetInterfaces())
        {
            if (!i.IsGenericType || i.GetGenericTypeDefinition() != typeof(IEnumerable<>)) continue;
            var item = i.GetGenericArguments()[0];
            if (item.IsGenericType && item.GetGenericTypeDefinition() == typeof(KeyValuePair<,>))
                return (item.GetProperty("Key")!, item.GetProperty("Value")!);
        }
        return null;
    });

    internal static bool IsAnonymous(Type t) =>
        t.IsDefined(typeof(CompilerGeneratedAttribute), false) && t.Name.Contains("AnonymousType", StringComparison.Ordinal);

    /// <summary>True for values written as a single JSON scalar (not an object or array).</summary>
    internal static bool IsScalar(object v) =>
        v is string or bool or char or Enum or Guid or DateTime or DateTimeOffset or DateOnly or TimeOnly or TimeSpan or Uri
        or int or long or short or sbyte or byte or ushort or uint or ulong or decimal or BigInteger or Int128 or UInt128
        or double or float or Half or DBNull or byte[] or JsonElement or JsonDocument or JsonNode;

    internal static bool IsMap(object v) => v is IDictionary or IEnumerable<KeyValuePair<string, object?>> || PairAccessors(v.GetType()) is not null;

    /// <summary>The entries of a dictionary-like value (keys as text), or null when it is not one.</summary>
    internal static IEnumerable<KeyValuePair<string, object?>>? MapEntries(object v)
    {
        if (v is IDictionary dict) return DictionaryEntries(dict);
        if (v is IEnumerable<KeyValuePair<string, object?>> pairs) return CheckedPairs(pairs);
        var kv = PairAccessors(v.GetType());
        return kv is null ? null : ReflectedEntries((IEnumerable)v, kv.Value.Key, kv.Value.Value);
    }

    private static IEnumerable<KeyValuePair<string, object?>> DictionaryEntries(IDictionary dict)
    {
        foreach (DictionaryEntry e in dict) yield return new KeyValuePair<string, object?>(KeyText(e.Key), e.Value);
    }

    private static IEnumerable<KeyValuePair<string, object?>> CheckedPairs(IEnumerable<KeyValuePair<string, object?>> pairs)
    {
        foreach (var e in pairs) yield return new KeyValuePair<string, object?>(KeyText(e.Key), e.Value);
    }

    private static IEnumerable<KeyValuePair<string, object?>> ReflectedEntries(IEnumerable items, PropertyInfo key, PropertyInfo value)
    {
        foreach (var item in items) yield return new KeyValuePair<string, object?>(KeyText(key.GetValue(item)), value.GetValue(item));
    }

    // ------------------------------------------------------------------ reading

    /// <summary>Parses JSON text into null, bool, long, BigInteger, double, string, List&lt;object?&gt; and <see cref="Row"/>.</summary>
    public static object? Parse(string text) => Parse(Encoding.UTF8.GetBytes(text));

    /// <summary>Parses UTF-8 JSON (see <see cref="Parse(string)"/>); invalid JSON is an <see cref="InputException"/>.</summary>
    public static object? Parse(ReadOnlySpan<byte> utf8)
    {
        try
        {
            var reader = new Utf8JsonReader(utf8, new JsonReaderOptions { MaxDepth = MaxDepth });
            if (!reader.Read()) throw new InputException("INPUT empty JSON");
            object? v = ReadValue(ref reader);
            if (reader.Read()) throw new InputException("INPUT trailing characters after JSON");
            return v;
        }
        catch (JsonException e)
        {
            throw new InputException("INPUT invalid JSON: " + e.Message, e);
        }
        catch (InvalidOperationException e)
        {
            throw new InputException("INPUT invalid JSON: " + e.Message, e);
        }
    }

    private static object? ReadValue(ref Utf8JsonReader r)
    {
        switch (r.TokenType)
        {
            case JsonTokenType.StartObject:
            {
                var row = new Row();
                while (r.Read() && r.TokenType != JsonTokenType.EndObject)
                {
                    string name = r.GetString()!;
                    r.Read();
                    row[name] = ReadValue(ref r);
                }
                return row;
            }
            case JsonTokenType.StartArray:
            {
                var list = new List<object?>();
                while (r.Read() && r.TokenType != JsonTokenType.EndArray) list.Add(ReadValue(ref r));
                return list;
            }
            case JsonTokenType.String:
                return r.GetString();
            case JsonTokenType.Number:
            {
                ReadOnlySpan<byte> raw = r.ValueSpan;
                if (raw.IndexOfAny((byte)'.', (byte)'e', (byte)'E') >= 0) return r.GetDouble();
                if (r.TryGetInt64(out long l)) return l;
                return BigInteger.Parse(Encoding.ASCII.GetString(raw), NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture);
            }
            case JsonTokenType.True: return true;
            case JsonTokenType.False: return false;
            case JsonTokenType.Null: return null;
            default: throw new InputException("INPUT unexpected JSON token " + r.TokenType);
        }
    }

    // ------------------------------------------------------------------ converting

    /// <summary>
    /// A decoded value as <typeparamref name="T"/>: numbers between number types (checked), text to
    /// Guid, DateTime, DateOnly, enums..., and rows or lists to your own types through System.Text.Json.
    /// </summary>
    public static T? Convert<T>(object? value)
    {
        if (value is null) return default;
        return (T?)ConvertTo(value, typeof(T));
    }

    internal static object? ConvertTo(object? v, Type type)
    {
        if (v is null || v is DBNull) return null;
        Type u = Nullable.GetUnderlyingType(type) ?? type;
        if (u == typeof(object) || u.IsInstanceOfType(v)) return v;
        try
        {
            if (u.IsEnum)
            {
                if (v is string es) return Enum.Parse(u, es, true);
                return Enum.ToObject(u, System.Convert.ToInt64(v, CultureInfo.InvariantCulture));
            }
            if (u == typeof(string))
            {
                return v switch
                {
                    bool b => b ? "true" : "false",
                    Row or IList => Stringify(v),
                    IFormattable f => f.ToString(null, CultureInfo.InvariantCulture),
                    _ => v.ToString(),
                };
            }
            if (v is string s)
            {
                if (u == typeof(Guid)) return Guid.Parse(s);
                if (u == typeof(DateTime)) return DateTime.Parse(s, CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind);
                if (u == typeof(DateTimeOffset)) return DateTimeOffset.Parse(s, CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal);
                if (u == typeof(DateOnly)) return DateOnly.FromDateTime(DateTime.Parse(s, CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind));
                if (u == typeof(TimeOnly)) return TimeOnly.Parse(s, CultureInfo.InvariantCulture);
                if (u == typeof(TimeSpan)) return TimeSpan.Parse(s, CultureInfo.InvariantCulture);
                if (u == typeof(Uri)) return new Uri(s, UriKind.RelativeOrAbsolute);
                if (u == typeof(char) && s.Length == 1) return s[0];
                if (u == typeof(BigInteger)) return BigInteger.Parse(s, CultureInfo.InvariantCulture);
            }
            if (u == typeof(BigInteger))
            {
                return v switch
                {
                    long l => new BigInteger(l),
                    double d => new BigInteger(d),
                    _ => new BigInteger(System.Convert.ToDecimal(v, CultureInfo.InvariantCulture)),
                };
            }
            if (v is BigInteger big && (u.IsPrimitive || u == typeof(decimal)))
            {
                if (u == typeof(decimal)) return (decimal)big;
                if (u == typeof(double)) return (double)big;
                if (u == typeof(ulong)) return (ulong)big;
                return System.Convert.ChangeType((long)big, u, CultureInfo.InvariantCulture);
            }
            if (v is IConvertible && (u.IsPrimitive || u == typeof(decimal)))
            {
                if (v is double d && u != typeof(double) && u != typeof(float) && u != typeof(decimal) && d != Math.Floor(d))
                    throw new InvalidCastException("cannot convert " + d.ToString("R", CultureInfo.InvariantCulture) + " to " + u.Name + " without losing its fraction");
                return System.Convert.ChangeType(v, u, CultureInfo.InvariantCulture);
            }
            return JsonSerializer.Deserialize(Stringify(v), type, SerializerOptions);
        }
        catch (Exception e) when (e is FormatException or OverflowException or InvalidCastException or JsonException or ArgumentException or NotSupportedException)
        {
            throw new InvalidCastException("cannot convert " + Describe(v) + " to " + type.Name + ": " + e.Message, e);
        }
    }

    private static string Describe(object v)
    {
        string text;
        try { text = Stringify(v); } catch (KestroException) { text = v.ToString() ?? ""; }
        if (text.Length > 60) text = text.Substring(0, 57) + "...";
        return text + " (" + v.GetType().Name + ")";
    }
}

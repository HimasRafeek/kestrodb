using System;
using System.Collections;
using System.Collections.Generic;
using System.Data;
using System.Data.Common;
using System.Diagnostics.CodeAnalysis;
using System.Globalization;
using System.Numerics;
using System.Threading;
using System.Threading.Tasks;

namespace KestroDB;

/// <summary>
/// A minimal ADO.NET connection over <see cref="KestroClient"/>, enough for Dapper and similar code.
/// Connection string keys (case-insensitive): <c>Address</c> (also <c>Server</c>, <c>Host</c>,
/// <c>Data Source</c>; comma-separated for a cluster), <c>Token</c> (also <c>Password</c>),
/// <c>Timeout</c> and <c>Connect Timeout</c> and <c>Failover Timeout</c> (seconds),
/// <c>Reconnect</c>, <c>CA File</c>, <c>Verify</c>, <c>Server Name</c>.
/// <code>
/// using var conn = new KestroConnection("Address=tcp://127.0.0.1:7820;Token=...");
/// var users = conn.Query&lt;User&gt;("SELECT * FROM users WHERE city = @city", new { city = "Kandy" });   // Dapper
/// </code>
/// </summary>
public sealed class KestroConnection : DbConnection
{
    private string _connectionString = "";
    private KestroClient? _client;
    private readonly bool _ownsClient = true;
    private ConnectionState _state = ConnectionState.Closed;

    public KestroConnection() { }

    public KestroConnection(string connectionString) => ConnectionString = connectionString;

    /// <summary>Wraps an open client (closing the connection does not close the client).</summary>
    public KestroConnection(KestroClient client)
    {
        _client = client ?? throw new ArgumentNullException(nameof(client));
        _ownsClient = false;
        _state = ConnectionState.Open;
    }

    /// <summary>The client underneath (null while closed).</summary>
    public KestroClient? Client => _client;

    [AllowNull]
    public override string ConnectionString
    {
        get => _connectionString;
        set
        {
            if (_state != ConnectionState.Closed) throw new InvalidOperationException("the connection is open");
            _connectionString = value ?? "";
        }
    }

    public override string Database => "";

    public override string DataSource => _client?.Address ?? Parse(_connectionString).Address;

    public override string ServerVersion => KestroClient.ClientVersion;

    public override ConnectionState State => _client is { IsClosed: true } ? ConnectionState.Closed : _state;

    protected override DbProviderFactory DbProviderFactory => KestroFactory.Instance;

    internal static (string Address, KestroOptions Options) Parse(string connectionString)
    {
        var b = new DbConnectionStringBuilder { ConnectionString = connectionString };
        string? address = null;
        var o = new KestroOptions();
        foreach (string key in b.Keys)
        {
            string value = Convert.ToString(b[key], CultureInfo.InvariantCulture) ?? "";
            switch (key.Replace(" ", "", StringComparison.Ordinal).ToLowerInvariant())
            {
                case "address": case "server": case "host": case "datasource": address = value; break;
                case "token": case "password": o.Token = value.Length == 0 ? null : value; break;
                case "timeout": case "commandtimeout": o.Timeout = TimeSpan.FromSeconds(double.Parse(value, CultureInfo.InvariantCulture)); break;
                case "connecttimeout": case "connectiontimeout": o.ConnectTimeout = TimeSpan.FromSeconds(double.Parse(value, CultureInfo.InvariantCulture)); break;
                case "failovertimeout": o.FailoverTimeout = TimeSpan.FromSeconds(double.Parse(value, CultureInfo.InvariantCulture)); break;
                case "reconnect": o.Reconnect = bool.Parse(value); break;
                case "cafile": o.CaFile = value; break;
                case "verify": o.Verify = bool.Parse(value); break;
                case "servername": o.ServerName = value; break;
                case "userid": case "user": case "uid": break;   // tokens name their user
                default: throw new ArgumentException("unknown connection string key: " + key);
            }
        }
        return (address ?? "tcp://127.0.0.1:7820", o);
    }

    public override void Open()
    {
        if (State == ConnectionState.Open) return;
        var (address, options) = Parse(_connectionString);
        _state = ConnectionState.Connecting;
        try { _client = KestroClient.Connect(address, options); }
        catch { _state = ConnectionState.Closed; throw; }
        _state = ConnectionState.Open;
    }

    public override async Task OpenAsync(CancellationToken cancellationToken)
    {
        if (State == ConnectionState.Open) return;
        var (address, options) = Parse(_connectionString);
        _state = ConnectionState.Connecting;
        try { _client = await KestroClient.ConnectAsync(address, options, cancellationToken).ConfigureAwait(false); }
        catch { _state = ConnectionState.Closed; throw; }
        _state = ConnectionState.Open;
    }

    public override void Close()
    {
        if (_ownsClient)
        {
            _client?.Dispose();
            _client = null;
        }
        _state = ConnectionState.Closed;
    }

    public override async Task CloseAsync()
    {
        if (_ownsClient && _client is not null)
        {
            await _client.DisposeAsync().ConfigureAwait(false);
            _client = null;
        }
        _state = ConnectionState.Closed;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) Close();
        base.Dispose(disposing);
    }

    public override async ValueTask DisposeAsync()
    {
        await CloseAsync().ConfigureAwait(false);
        GC.SuppressFinalize(this);
    }

    public override void ChangeDatabase(string databaseName) => throw new NotSupportedException("a KestroDB server has one database");

    internal KestroClient OpenClient() =>
        State == ConnectionState.Open && _client is not null ? _client : throw new InvalidOperationException("the connection is not open");

    protected override DbTransaction BeginDbTransaction(IsolationLevel isolationLevel)
    {
        OpenClient().Begin();
        return new KestroTransaction(this, isolationLevel);
    }

    protected override async ValueTask<DbTransaction> BeginDbTransactionAsync(IsolationLevel isolationLevel, CancellationToken cancellationToken)
    {
        await OpenClient().BeginAsync(false, cancellationToken).ConfigureAwait(false);
        return new KestroTransaction(this, isolationLevel);
    }

    public new KestroTransaction BeginTransaction() => (KestroTransaction)BeginDbTransaction(IsolationLevel.Serializable);

    public new KestroCommand CreateCommand() => new() { Connection = this };

    protected override DbCommand CreateDbCommand() => CreateCommand();
}

/// <summary>An open transaction of a <see cref="KestroConnection"/> (BEGIN ... COMMIT/ROLLBACK).</summary>
public sealed class KestroTransaction : DbTransaction
{
    private KestroConnection? _connection;

    internal KestroTransaction(KestroConnection connection, IsolationLevel level)
    {
        _connection = connection;
        IsolationLevel = level == IsolationLevel.Unspecified ? IsolationLevel.Serializable : level;
    }

    public override IsolationLevel IsolationLevel { get; }

    protected override DbConnection? DbConnection => _connection;

    private KestroClient Client => (_connection ?? throw new InvalidOperationException("the transaction has ended")).OpenClient();

    public override void Commit()
    {
        Client.Commit();
        _connection = null;
    }

    public override void Rollback()
    {
        Client.Rollback();
        _connection = null;
    }

    public override async Task CommitAsync(CancellationToken cancellationToken = default)
    {
        await Client.CommitAsync(cancellationToken).ConfigureAwait(false);
        _connection = null;
    }

    public override async Task RollbackAsync(CancellationToken cancellationToken = default)
    {
        await Client.RollbackAsync(cancellationToken).ConfigureAwait(false);
        _connection = null;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing && _connection is { State: ConnectionState.Open } c && c.Client is { InTransaction: true } client)
        {
            try { client.Rollback(); } catch (KestroException) { /* the connection may be gone */ }
        }
        _connection = null;
        base.Dispose(disposing);
    }
}

/// <summary>
/// A SQL statement of a <see cref="KestroConnection"/>. Parameters are all named (sent as
/// <c>:name</c> / <c>@name</c> values; a leading @ or : in the name is dropped) or all unnamed
/// (positional, for <c>?</c>, <c>?N</c> and <c>$N</c>).
/// </summary>
public sealed class KestroCommand : DbCommand
{
    private KestroConnection? _connection;
    private readonly KestroParameterCollection _parameters = new();

    public KestroCommand() { }

    public KestroCommand(string commandText, KestroConnection? connection = null)
    {
        CommandText = commandText;
        _connection = connection;
    }

    [AllowNull]
    public override string CommandText { get; set; } = "";

    public override int CommandTimeout { get; set; } = 30;

    public override CommandType CommandType { get; set; } = CommandType.Text;

    public override bool DesignTimeVisible { get; set; }

    public override UpdateRowSource UpdatedRowSource { get; set; }

    protected override DbConnection? DbConnection
    {
        get => _connection;
        set => _connection = (KestroConnection?)value;
    }

    public new KestroConnection? Connection
    {
        get => _connection;
        set => _connection = value;
    }

    protected override DbParameterCollection DbParameterCollection => _parameters;

    public new KestroParameterCollection Parameters => _parameters;

    protected override DbTransaction? DbTransaction { get; set; }

    public override void Cancel() { }

    public override void Prepare() { }

    protected override DbParameter CreateDbParameter() => new KestroParameter();

    public new KestroParameter CreateParameter() => new();

    private KestroClient Client
    {
        get
        {
            if (CommandType != CommandType.Text) throw new NotSupportedException("only CommandType.Text is supported");
            return (_connection ?? throw new InvalidOperationException("the command has no connection")).OpenClient();
        }
    }

    internal object? ParameterValue()
    {
        if (_parameters.Count == 0) return null;
        int named = 0;
        foreach (KestroParameter p in _parameters)
        {
            if (p.Direction != ParameterDirection.Input) throw new NotSupportedException("only input parameters are supported");
            if (!string.IsNullOrEmpty(p.ParameterName)) named++;
        }
        if (named == 0)
        {
            var list = new object?[_parameters.Count];
            for (int i = 0; i < list.Length; i++) list[i] = Unwrap(_parameters[i].Value);
            return list;
        }
        if (named != _parameters.Count) throw new InputException("INPUT parameters must be all named or all unnamed");
        var map = new Row(_parameters.Count);
        foreach (KestroParameter p in _parameters) map[KestroParameterCollection.Bare(p.ParameterName)] = Unwrap(p.Value);
        return map;
    }

    private static object? Unwrap(object? v) => v is DBNull ? null : v;

    private static int Affected(SqlResult r) => r.HasRows ? -1 : (int)Math.Min(int.MaxValue, r.Changes);

    private static object? Scalar(SqlResult r)
    {
        if (r.Rows is not { Count: > 0 } rows || rows[0].Count == 0) return null;
        return KestroDataReader.AdoValue(rows[0][0]);
    }

    public override int ExecuteNonQuery() => Affected(Client.Sql(CommandText, ParameterValue()));

    public override async Task<int> ExecuteNonQueryAsync(CancellationToken cancellationToken) =>
        Affected(await Client.SqlAsync(CommandText, ParameterValue(), cancellationToken).ConfigureAwait(false));

    public override object? ExecuteScalar() => Scalar(Client.Sql(CommandText, ParameterValue()));

    public override async Task<object?> ExecuteScalarAsync(CancellationToken cancellationToken) =>
        Scalar(await Client.SqlAsync(CommandText, ParameterValue(), cancellationToken).ConfigureAwait(false));

    protected override DbDataReader ExecuteDbDataReader(CommandBehavior behavior) =>
        new KestroDataReader(Client.Sql(CommandText, ParameterValue()), behavior, _connection);

    protected override async Task<DbDataReader> ExecuteDbDataReaderAsync(CommandBehavior behavior, CancellationToken cancellationToken) =>
        new KestroDataReader(await Client.SqlAsync(CommandText, ParameterValue(), cancellationToken).ConfigureAwait(false), behavior, _connection);

    public new KestroDataReader ExecuteReader() => (KestroDataReader)ExecuteDbDataReader(CommandBehavior.Default);
}

/// <summary>A parameter of a <see cref="KestroCommand"/> (input only; DbType is a hint and is not used).</summary>
public sealed class KestroParameter : DbParameter
{
    public KestroParameter() { }

    public KestroParameter(string? name, object? value)
    {
        ParameterName = name ?? "";
        Value = value;
    }

    public override DbType DbType { get; set; } = DbType.Object;

    public override ParameterDirection Direction { get; set; } = ParameterDirection.Input;

    public override bool IsNullable { get; set; } = true;

    [AllowNull]
    public override string ParameterName { get; set; } = "";

    public override int Size { get; set; }

    [AllowNull]
    public override string SourceColumn { get; set; } = "";

    public override bool SourceColumnNullMapping { get; set; }

    public override object? Value { get; set; }

    public override void ResetDbType() => DbType = DbType.Object;
}

/// <summary>The parameters of a <see cref="KestroCommand"/>; names match with or without a leading @ or :.</summary>
public sealed class KestroParameterCollection : DbParameterCollection
{
    private readonly List<KestroParameter> _list = new();

    internal static string Bare(string? name) =>
        string.IsNullOrEmpty(name) ? "" : name[0] is '@' or ':' ? name.Substring(1) : name;

    public new KestroParameter this[int index]
    {
        get => _list[index];
        set => _list[index] = value;
    }

    public override int Count => _list.Count;

    public override object SyncRoot => ((ICollection)_list).SyncRoot;

    /// <summary>Adds a parameter with a name and value.</summary>
    public KestroParameter AddWithValue(string? name, object? value)
    {
        var p = new KestroParameter(name, value);
        _list.Add(p);
        return p;
    }

    public override int Add(object value)
    {
        _list.Add(Check(value));
        return _list.Count - 1;
    }

    public override void AddRange(Array values)
    {
        foreach (var v in values) _list.Add(Check(v));
    }

    private static KestroParameter Check(object? value) =>
        value as KestroParameter ?? throw new InvalidCastException("a KestroCommand takes KestroParameter objects");

    public override void Clear() => _list.Clear();

    public override bool Contains(object value) => value is KestroParameter p && _list.Contains(p);

    public override bool Contains(string value) => IndexOf(value) >= 0;

    public override void CopyTo(Array array, int index) => ((ICollection)_list).CopyTo(array, index);

    public override IEnumerator GetEnumerator() => _list.GetEnumerator();

    protected override DbParameter GetParameter(int index) => _list[index];

    protected override DbParameter GetParameter(string parameterName)
    {
        int i = IndexOf(parameterName);
        return i >= 0 ? _list[i] : throw new IndexOutOfRangeException("no parameter " + parameterName);
    }

    public override int IndexOf(object value) => value is KestroParameter p ? _list.IndexOf(p) : -1;

    public override int IndexOf(string parameterName)
    {
        string bare = Bare(parameterName);
        for (int i = 0; i < _list.Count; i++) if (string.Equals(Bare(_list[i].ParameterName), bare, StringComparison.Ordinal)) return i;
        return -1;
    }

    public override void Insert(int index, object value) => _list.Insert(index, Check(value));

    public override void Remove(object value) => _list.Remove(Check(value));

    public override void RemoveAt(int index) => _list.RemoveAt(index);

    public override void RemoveAt(string parameterName)
    {
        int i = IndexOf(parameterName);
        if (i >= 0) _list.RemoveAt(i);
    }

    protected override void SetParameter(int index, DbParameter value) => _list[index] = Check(value);

    protected override void SetParameter(string parameterName, DbParameter value)
    {
        int i = IndexOf(parameterName);
        if (i >= 0) _list[i] = Check(value);
        else _list.Add(Check(value));
    }
}

/// <summary>
/// The rows of one statement (already read). Columns are every name of every row in the order
/// first seen. Types: INTEGER columns are Int64, REAL Double, BOOLEAN Boolean, text String,
/// and JSON objects and arrays String (their JSON text); missing values and SQL NULL are DBNull.
/// </summary>
public sealed class KestroDataReader : DbDataReader
{
    private readonly IReadOnlyList<Row> _rows;
    private readonly IReadOnlyList<string> _columns;
    private readonly Type[] _types;
    private readonly int _affected;
    private readonly CommandBehavior _behavior;
    private readonly KestroConnection? _connection;
    private int _position = -1;
    private bool _closed;

    internal KestroDataReader(SqlResult result, CommandBehavior behavior, KestroConnection? connection)
    {
        _rows = result.Rows ?? Array.Empty<Row>();
        _columns = result.Columns;
        _affected = result.HasRows ? -1 : (int)Math.Min(int.MaxValue, result.Changes);
        _behavior = behavior;
        _connection = connection;
        _types = new Type[_columns.Count];
        for (int c = 0; c < _columns.Count; c++) _types[c] = InferType(_columns[c]);
    }

    private Type InferType(string column)
    {
        Type? t = null;
        foreach (var row in _rows)
        {
            object? v = row[column];
            Type vt = v switch
            {
                null => typeof(void),
                long => typeof(long),
                double => typeof(double),
                bool => typeof(bool),
                BigInteger => typeof(decimal),
                _ => typeof(string),
            };
            if (vt == typeof(void)) continue;
            if (t is null) t = vt;
            else if (t != vt)
            {
                if ((t == typeof(long) || t == typeof(double) || t == typeof(decimal)) && (vt == typeof(long) || vt == typeof(double) || vt == typeof(decimal)))
                    t = t == typeof(decimal) || vt == typeof(decimal) ? typeof(decimal) : typeof(double);
                else return typeof(object);
            }
        }
        return t ?? typeof(object);
    }

    /// <summary>A decoded value as ADO.NET shows it: DBNull for null, JSON text for objects and arrays.</summary>
    internal static object AdoValue(object? v) => v switch
    {
        null => DBNull.Value,
        Row or List<object?> => Json.Stringify(v),
        BigInteger b => (decimal)b,
        _ => v,
    };

    private object? Raw(int ordinal)
    {
        if (_closed) throw new InvalidOperationException("the reader is closed");
        if (_position < 0 || _position >= _rows.Count) throw new InvalidOperationException("no current row (call Read())");
        if (ordinal < 0 || ordinal >= _columns.Count) throw new IndexOutOfRangeException("column " + ordinal + " out of range 0.." + (_columns.Count - 1));
        return _rows[_position][_columns[ordinal]];
    }

    public override object GetValue(int ordinal)
    {
        object v = AdoValue(Raw(ordinal));
        Type t = _types[ordinal];
        if (v is DBNull || t == typeof(object) || t == v.GetType()) return v;
        return Convert.ChangeType(v, t, CultureInfo.InvariantCulture);
    }

    public override T GetFieldValue<T>(int ordinal)
    {
        object? raw = Raw(ordinal);
        if (raw is null)
        {
            if (typeof(T) == typeof(DBNull) || typeof(T) == typeof(object)) return (T)(object)DBNull.Value;
            if (default(T) is null) return default!;
            throw new InvalidCastException("column " + _columns[ordinal] + " is NULL");
        }
        if (typeof(T) == typeof(string) || typeof(T) == typeof(object)) return (T)(object)(raw is Row or List<object?> ? Json.Stringify(raw) : GetValue(ordinal));
        return (T)Json.ConvertTo(raw, typeof(T))!;
    }

    private T Get<T>(int ordinal) => GetFieldValue<T>(ordinal);

    public override bool GetBoolean(int ordinal) => Get<bool>(ordinal);
    public override byte GetByte(int ordinal) => Get<byte>(ordinal);
    public override char GetChar(int ordinal) => Get<char>(ordinal);
    public override DateTime GetDateTime(int ordinal) => Get<DateTime>(ordinal);
    public override decimal GetDecimal(int ordinal) => Get<decimal>(ordinal);
    public override double GetDouble(int ordinal) => Get<double>(ordinal);
    public override float GetFloat(int ordinal) => Get<float>(ordinal);
    public override Guid GetGuid(int ordinal) => Get<Guid>(ordinal);
    public override short GetInt16(int ordinal) => Get<short>(ordinal);
    public override int GetInt32(int ordinal) => Get<int>(ordinal);
    public override long GetInt64(int ordinal) => Get<long>(ordinal);

    public override string GetString(int ordinal)
    {
        object? raw = Raw(ordinal);
        if (raw is null) throw new InvalidCastException("column " + _columns[ordinal] + " is NULL");
        return raw as string ?? Json.ConvertTo(raw, typeof(string)) as string ?? "";
    }

    public override long GetBytes(int ordinal, long dataOffset, byte[]? buffer, int bufferOffset, int length) =>
        throw new NotSupportedException("KestroDB values are JSON: there are no byte columns");

    public override long GetChars(int ordinal, long dataOffset, char[]? buffer, int bufferOffset, int length)
    {
        string s = GetString(ordinal);
        if (buffer is null) return s.Length;
        int n = (int)Math.Max(0, Math.Min(length, s.Length - dataOffset));
        s.CopyTo((int)dataOffset, buffer, bufferOffset, n);
        return n;
    }

    public override string GetDataTypeName(int ordinal)
    {
        Type t = _types[ordinal];
        return t == typeof(long) ? "INTEGER" : t == typeof(double) || t == typeof(decimal) ? "REAL" : t == typeof(bool) ? "BOOLEAN" : t == typeof(string) ? "TEXT" : "JSON";
    }

    public override Type GetFieldType(int ordinal) => _types[ordinal];

    public override string GetName(int ordinal) => _columns[ordinal];

    public override int GetOrdinal(string name)
    {
        for (int i = 0; i < _columns.Count; i++) if (_columns[i] == name) return i;
        for (int i = 0; i < _columns.Count; i++) if (string.Equals(_columns[i], name, StringComparison.OrdinalIgnoreCase)) return i;
        throw new IndexOutOfRangeException("no column " + name);
    }

    public override int GetValues(object[] values)
    {
        int n = Math.Min(values.Length, _columns.Count);
        for (int i = 0; i < n; i++) values[i] = GetValue(i);
        return n;
    }

    public override bool IsDBNull(int ordinal) => Raw(ordinal) is null;

    public override int FieldCount => _columns.Count;

    public override object this[int ordinal] => GetValue(ordinal);

    public override object this[string name] => GetValue(GetOrdinal(name));

    public override int RecordsAffected => _affected;

    public override bool HasRows => _rows.Count > 0;

    public override bool IsClosed => _closed;

    public override int Depth => 0;

    public override bool NextResult()
    {
        _position = _rows.Count;
        return false;
    }

    public override bool Read()
    {
        if (_closed) throw new InvalidOperationException("the reader is closed");
        if (_position < _rows.Count) _position++;
        if ((_behavior & CommandBehavior.SingleRow) != 0 && _position > 0) _position = _rows.Count;
        return _position < _rows.Count;
    }

    public override Task<bool> ReadAsync(CancellationToken cancellationToken) => Task.FromResult(Read());

    public override Task<bool> NextResultAsync(CancellationToken cancellationToken) => Task.FromResult(NextResult());

    public override void Close()
    {
        if (_closed) return;
        _closed = true;
        if ((_behavior & CommandBehavior.CloseConnection) != 0) _connection?.Close();
    }

    public override IEnumerator GetEnumerator() => new DbEnumerator(this, closeReader: false);
}

/// <summary>The ADO.NET factory (register with DbProviderFactories.RegisterFactory("KestroDB", KestroFactory.Instance)).</summary>
public sealed class KestroFactory : DbProviderFactory
{
    public static readonly KestroFactory Instance = new();

    private KestroFactory() { }

    public override DbConnection CreateConnection() => new KestroConnection();

    public override DbCommand CreateCommand() => new KestroCommand();

    public override DbParameter CreateParameter() => new KestroParameter();

    public override DbConnectionStringBuilder CreateConnectionStringBuilder() => new();
}

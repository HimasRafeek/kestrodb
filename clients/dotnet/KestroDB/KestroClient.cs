using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace KestroDB;

/// <summary>
/// A connection to kestro-server.
/// <code>
/// await using var db = await KestroClient.ConnectAsync("tcp://127.0.0.1:7820", new KestroOptions { Token = token });
/// await db.ExecuteAsync("INSERT INTO users (email, name) VALUES (:email, :name)", new { email = "a@example.com", name = "Ann" });
/// Row? user = await db.QueryOneAsync("SELECT * FROM users WHERE id = ?", db.LastInsertId);
/// await db.TransactionAsync(async tx =&gt;
/// {
///     await tx.ExecuteAsync("UPDATE accounts SET balance = balance - ? WHERE id = ?", 10, 1);
///     await tx.ExecuteAsync("UPDATE accounts SET balance = balance + ? WHERE id = ?", 10, 2);
/// });
/// long v = await db.SetAsync("users", "kestro", new { name = "Kestro" });
/// await db.SetAsync("users", "kestro", new { name = "K" }, expectedVersion: v);   // only if still at version v
/// </code>
/// <para>Addresses: <c>tcp://host:port</c>, <c>tls://host:port</c>, <c>unix:///path</c>, or
/// <c>host:port</c>. Several comma-separated addresses (or a list) name a cluster's members: writes
/// go to the leader, which a follower's READONLY reply names; the client moves there and sends the
/// request again (nothing was applied), also during an election, for up to
/// <see cref="KestroOptions.FailoverTimeout"/>. Never inside a transaction.</para>
/// <para>A connection the server closed while idle (a restart, the network) is reopened before the
/// next request, except inside a transaction: that call fails with <c>IO ... connection lost inside a
/// transaction; it was rolled back</c>. A request already sent when the connection broke fails with
/// <see cref="KestroIOException"/> (for a write: unknown outcome).</para>
/// <para>Calls are serialized, so tasks and threads may share a client; the delegate given to
/// <c>TransactionAsync</c> runs with the client to itself. A transaction belongs to the connection:
/// use one client per concurrent unit of work when they use explicit transactions.</para>
/// </summary>
public sealed partial class KestroClient : IAsyncDisposable, IDisposable
{
    /// <summary>This client's version (the KestroDB release it was written for).</summary>
    public const string ClientVersion = "0.9.0";

    /// <summary>Largest frame either way (64 MB), as on the server.</summary>
    public const int MaxFrame = 64 * 1024 * 1024;

    /// <summary>Largest JSON value of a key (256 KiB).</summary>
    public const int MaxValue = 256 * 1024;

    private static readonly Regex LeaderPattern = new(@"the leader is (\S+)", RegexOptions.CultureInvariant);
    private static readonly Regex CommitOrEnd = new(@"^\s*(COMMIT|END)\b", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
    private static readonly byte[] NoBytes = Array.Empty<byte>();
    private static readonly long OneSecond = Stopwatch.Frequency;

    /// <summary>One parsed member address.</summary>
    private sealed class MemberAddress
    {
        public readonly string Text;
        public readonly string Host = "";
        public readonly int Port;
        public readonly bool Tls;
        public readonly string? UnixPath;

        public MemberAddress(string text)
        {
            Text = text;
            if (text.StartsWith("unix://", StringComparison.Ordinal))
            {
                UnixPath = text.Substring(7);
                if (UnixPath.Length == 0) throw new InputException("INPUT address must be tcp://host:port, tls://host:port or unix:///path");
                return;
            }
            Tls = text.StartsWith("tls://", StringComparison.Ordinal);
            string hp = text.StartsWith("tcp://", StringComparison.Ordinal) || Tls ? text.Substring(6) : text;
            int colon = hp.LastIndexOf(':');
            if (colon < 0) throw new InputException("INPUT address must be tcp://host:port, tls://host:port or unix:///path: " + text);
            string h = hp.Substring(0, colon);
            if (h.StartsWith('[') && h.EndsWith(']')) h = h.Substring(1, h.Length - 2);
            Host = h.Length == 0 ? "127.0.0.1" : h;
            if (!int.TryParse(hp.AsSpan(colon + 1), NumberStyles.None, CultureInfo.InvariantCulture, out int p) || p < 1 || p > 65535)
                throw new InputException("INPUT invalid port in address " + text);
            Port = p;
        }

        /// <summary>The same member however it is written.</summary>
        public string Key => UnixPath is not null ? Text : (Tls ? "tls://" : "tcp://") + Host.ToLowerInvariant() + ":" + Port.ToString(CultureInfo.InvariantCulture);
    }

    /// <summary>An open connection: a socket and its (possibly TLS) stream.</summary>
    private sealed class Conn : IDisposable
    {
        public readonly Socket Socket;
        public readonly Stream Stream;
        public readonly bool Tls;
        public readonly byte[] Head = new byte[4];
        public long Exchanges;

        public Conn(Socket socket, Stream stream, bool tls)
        {
            Socket = socket;
            Stream = stream;
            Tls = tls;
        }

        public void Dispose()
        {
            try { Stream.Dispose(); } catch (Exception) { /* closing anyway */ }
            try { Socket.Dispose(); } catch (Exception) { /* closing anyway */ }
        }
    }

    private readonly IReadOnlyList<MemberAddress> _addresses;
    private readonly KestroOptions _options;
    private readonly SemaphoreSlim _gate = new(1, 1);   // one call, or one TransactionAsync with its body
    private readonly SemaphoreSlim _io = new(1, 1);     // one call (also inside a TransactionAsync body)
    private readonly AsyncLocal<object?> _owner = new();
    private object? _gateOwner;
    private MemberAddress _current;
    private Conn? _conn;
    private volatile bool _closed;
    private bool _lost;                  // the connection broke (as opposed to a move to another member)
    private long _lastUse;
    private volatile bool _inTransaction;
    private string? _implicitSavepoint;  // a SAVEPOINT that began the transaction: releasing it commits
    private object? _lastInsertId;
    private X509Certificate2Collection? _caCerts;
    internal int Opens;                  // connections opened (tests: kept or reopened)

    private KestroClient(IEnumerable<string?> list, KestroOptions? options)
    {
        var parsed = new List<MemberAddress>();
        foreach (var a in list)
        {
            if (a is null) continue;
            string t = a.Trim();
            if (t.Length > 0) parsed.Add(new MemberAddress(t));
        }
        if (parsed.Count == 0) throw new InputException("INPUT no server address");
        _addresses = parsed.AsReadOnly();
        _options = options?.Clone() ?? new KestroOptions();
        _current = parsed[0];
    }

    // ------------------------------------------------------------------ connect

    /// <summary>Connects to one server, or to a cluster given as comma-separated member addresses.</summary>
    public static Task<KestroClient> ConnectAsync(string address, KestroOptions? options = null, CancellationToken cancellationToken = default)
    {
        if (address is null) throw new InputException("INPUT no server address");
        return ConnectAsync(address.Split(','), options, cancellationToken);
    }

    /// <summary>Connects to a cluster's members (any order); the client finds the leader.</summary>
    public static async Task<KestroClient> ConnectAsync(IEnumerable<string> addresses, KestroOptions? options = null, CancellationToken cancellationToken = default)
    {
        var c = new KestroClient(addresses ?? throw new InputException("INPUT no server address"), options);
        await c.OpenAsync(true, cancellationToken).ConfigureAwait(false);
        return c;
    }

    /// <summary>Connects (blocking) to one server, or to a cluster given as comma-separated member addresses.</summary>
    public static KestroClient Connect(string address, KestroOptions? options = null)
    {
        if (address is null) throw new InputException("INPUT no server address");
        return Connect(address.Split(','), options);
    }

    /// <summary>Connects (blocking) to a cluster's members (any order); the client finds the leader.</summary>
    public static KestroClient Connect(IEnumerable<string> addresses, KestroOptions? options = null)
    {
        var c = new KestroClient(addresses ?? throw new InputException("INPUT no server address"), options);
        Wait(c.OpenAsync(false, default));
        return c;
    }

    /// <summary>The member this client uses now (as written in the address list).</summary>
    public string Address => _current.Text;

    /// <summary>True between BEGIN (or START TRANSACTION, or a SAVEPOINT outside one) and its COMMIT/ROLLBACK.</summary>
    public bool InTransaction => _inTransaction;

    /// <summary>The id of the last row an INSERT on this client created (a long, or a string for text ids), or null.</summary>
    public object? LastInsertId => Volatile.Read(ref _lastInsertId);

    /// <summary>True once the client was closed or disposed.</summary>
    public bool IsClosed => _closed;

    // The current address first, then the other members.
    private async ValueTask OpenAsync(bool async, CancellationToken ct)
    {
        var order = new List<MemberAddress> { _current };
        foreach (var a in _addresses) if (!ReferenceEquals(a, _current)) order.Add(a);
        KestroException? last = null;
        foreach (var a in order)
        {
            try
            {
                _conn = await OpenOneAsync(a, async, ct).ConfigureAwait(false);
                _current = a;
                _lost = false;
                Opens++;
                _lastUse = Stopwatch.GetTimestamp();
                return;
            }
            catch (AuthException)
            {
                throw;
            }
            catch (KestroException e)
            {
                last = e;
            }
        }
        throw last!;
    }

    private static bool Limited(TimeSpan t) => t > TimeSpan.Zero && t != Timeout.InfiniteTimeSpan;

    private static int Millis(TimeSpan t) => Limited(t) ? (int)Math.Clamp(t.TotalMilliseconds, 1, int.MaxValue) : 0;

    private static string Seconds(TimeSpan t) => t.TotalSeconds.ToString("0.###", CultureInfo.InvariantCulture) + " s";

    private async ValueTask<Conn> OpenOneAsync(MemberAddress a, bool async, CancellationToken ct)
    {
        TimeSpan connectTimeout = _options.ConnectTimeout ?? _options.Timeout;
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        if (Limited(connectTimeout)) cts.CancelAfter(connectTimeout);
        Socket? socket = null;
        Stream? stream = null;
        Conn? conn = null;
        string stage = "connect";
        try
        {
            if (a.UnixPath is not null)
            {
                socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
                var ep = new UnixDomainSocketEndPoint(a.UnixPath);
                if (async) await socket.ConnectAsync(ep, cts.Token).ConfigureAwait(false);
                else ConnectBlocking(socket, ep, connectTimeout);
            }
            else
            {
                socket = await ConnectTcpAsync(a, connectTimeout, async, cts.Token).ConfigureAwait(false);
            }
            int ms = Millis(_options.Timeout);
            socket.ReceiveTimeout = Limited(connectTimeout) ? Millis(connectTimeout) : ms;
            socket.SendTimeout = socket.ReceiveTimeout;
            stream = new NetworkStream(socket, ownsSocket: true);
            if (a.Tls)
            {
                stage = "tls";
                var ssl = new SslStream(stream, leaveInnerStreamOpen: false);
                stream = ssl;
                var auth = new SslClientAuthenticationOptions
                {
                    TargetHost = _options.ServerName ?? a.Host,
                    EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13,
                    CertificateRevocationCheckMode = X509RevocationMode.NoCheck,
                    RemoteCertificateValidationCallback = ValidateCertificate,
                    ClientCertificates = _options.ClientCertificates,
                };
                if (_options.CaFile is not null && _options.Verify && _options.RemoteCertificateValidation is null) LoadCa();
                if (async) await ssl.AuthenticateAsClientAsync(auth, cts.Token).ConfigureAwait(false);
                else ssl.AuthenticateAsClient(auth);
            }
            socket.ReceiveTimeout = ms;
            socket.SendTimeout = ms;
            conn = new Conn(socket, stream, a.Tls);
            if (_options.Token is not null)
            {
                stage = "login";
                var early = Unsolicited(conn);   // BUSY already here: report it, do not log in
                if (early is not null) throw early;
                await ExchangeAsync(conn, (byte)'A', Encoding.UTF8.GetBytes(_options.Token), _options.Timeout, async, ct).ConfigureAwait(false);
            }
            return conn;
        }
        catch (Exception e)
        {
            if (conn is not null) conn.Dispose();
            else
            {
                try { stream?.Dispose(); } catch (Exception) { /* closing */ }
                try { socket?.Dispose(); } catch (Exception) { /* closing */ }
            }
            if (e is KestroException) throw;
            if (e is OperationCanceledException && ct.IsCancellationRequested) throw;
            string why = e is OperationCanceledException || (e is SocketException se && se.SocketErrorCode == SocketError.TimedOut)
                ? "timed out after " + Seconds(connectTimeout)
                : Describe(e);
            if (stage == "tls") why = "TLS handshake failed: " + why;
            if (stage == "login") why += " (the server closed the connection before login; it may be at its connection limit)";
            throw new KestroIOException("IO cannot connect to " + a.Text + ": " + why, e);
        }
    }

    // Every address of the host in turn (a new socket for each: a failed connect leaves a socket unusable).
    private static async ValueTask<Socket> ConnectTcpAsync(MemberAddress a, TimeSpan timeout, bool async, CancellationToken ct)
    {
        IPAddress[] ips;
        if (IPAddress.TryParse(a.Host, out var ip)) ips = new[] { ip };
        else ips = async ? await Dns.GetHostAddressesAsync(a.Host, ct).ConfigureAwait(false) : Dns.GetHostAddresses(a.Host);
        if (ips.Length == 0) throw new SocketException((int)SocketError.HostNotFound);
        Exception? last = null;
        foreach (var address in ips)
        {
            var s = new Socket(address.AddressFamily, SocketType.Stream, ProtocolType.Tcp);
            try
            {
                s.NoDelay = true;
                s.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.KeepAlive, true);
                var ep = new IPEndPoint(address, a.Port);
                if (async) await s.ConnectAsync(ep, ct).ConfigureAwait(false);
                else ConnectBlocking(s, ep, timeout);
                return s;
            }
            catch (Exception e)
            {
                s.Dispose();
                last = e;
                if (ct.IsCancellationRequested || e is OperationCanceledException) throw;
            }
        }
        throw last!;
    }

    // A blocking connect with a time limit: non-blocking connect, then wait until writable.
    private static void ConnectBlocking(Socket s, EndPoint ep, TimeSpan timeout)
    {
        if (!Limited(timeout))
        {
            s.Connect(ep);
            return;
        }
        s.Blocking = false;
        try
        {
            s.Connect(ep);
        }
        catch (SocketException e) when (e.SocketErrorCode is SocketError.WouldBlock or SocketError.InProgress or SocketError.AlreadyInProgress)
        {
            var write = new List<Socket> { s };
            var error = new List<Socket> { s };
            long us = (long)Math.Min(int.MaxValue, timeout.TotalMicroseconds);
            Socket.Select(null, write, error, (int)us);
            if (write.Count == 0 && error.Count == 0) throw new SocketException((int)SocketError.TimedOut);
            int code = (int)(s.GetSocketOption(SocketOptionLevel.Socket, SocketOptionName.Error) ?? 0);
            if (code != 0) throw new SocketException(code);
            if (error.Count > 0) throw new SocketException((int)SocketError.ConnectionRefused);
        }
        finally
        {
            s.Blocking = true;
        }
        if (!s.Connected) throw new SocketException((int)SocketError.NotConnected);
    }

    private static string Describe(Exception e)
    {
        var inner = e;
        while (inner.InnerException is not null && (inner is AuthenticationException || inner is IOException) && inner.Message.Contains("see inner exception", StringComparison.OrdinalIgnoreCase))
            inner = inner.InnerException;
        string m = inner.Message;
        if (inner != e && !string.IsNullOrEmpty(e.Message) && !e.Message.Contains("see inner exception", StringComparison.OrdinalIgnoreCase)) m = e.Message + ": " + m;
        return string.IsNullOrEmpty(m) ? e.GetType().Name : m;
    }

    private X509Certificate2Collection LoadCa()
    {
        if (_caCerts is not null) return _caCerts;
        var certs = new X509Certificate2Collection();
        try
        {
            certs.ImportFromPemFile(_options.CaFile!);
        }
        catch (Exception e) when (e is IOException or CryptographicException or UnauthorizedAccessException or ArgumentException)
        {
            throw new KestroIOException("IO TLS setup: cannot read the CA file " + _options.CaFile + ": " + e.Message, e);
        }
        if (certs.Count == 0) throw new KestroIOException("IO TLS setup: no certificate in " + _options.CaFile);
        return _caCerts = certs;
    }

    private bool ValidateCertificate(object sender, X509Certificate? certificate, X509Chain? chain, SslPolicyErrors errors)
    {
        if (_options.RemoteCertificateValidation is not null) return _options.RemoteCertificateValidation(sender, certificate, chain, errors);
        if (!_options.Verify) return true;
        if (_options.CaFile is null) return errors == SslPolicyErrors.None;
        if (certificate is null || (errors & (SslPolicyErrors.RemoteCertificateNotAvailable | SslPolicyErrors.RemoteCertificateNameMismatch)) != 0) return false;
        using var custom = new X509Chain();
        custom.ChainPolicy.TrustMode = X509ChainTrustMode.CustomRootTrust;
        custom.ChainPolicy.CustomTrustStore.AddRange(LoadCa());
        custom.ChainPolicy.RevocationMode = X509RevocationMode.NoCheck;
        custom.ChainPolicy.ApplicationPolicy.Add(new Oid("1.3.6.1.5.5.7.3.1"));   // server authentication
        if (chain is not null) foreach (var element in chain.ChainElements) custom.ChainPolicy.ExtraStore.Add(element.Certificate);
        using var leaf = new X509Certificate2(certificate);
        return custom.Build(leaf);
    }

    // ------------------------------------------------------------------ frames

    /// <summary>Sends one request on c and reads its reply; an error reply becomes a KestroException.</summary>
    private static async ValueTask<byte[]> ExchangeAsync(Conn c, byte cmd, byte[] payload, TimeSpan timeout, bool async, CancellationToken ct)
    {
        byte[] frame = new byte[5 + payload.Length];
        BinaryPrimitives.WriteInt32LittleEndian(frame, 1 + payload.Length);
        frame[4] = cmd;
        payload.CopyTo(frame, 5);
        byte[] reply;
        if (async)
        {
            CancellationTokenSource? cts = null;
            CancellationToken token = ct;
            if (Limited(timeout))
            {
                cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
                cts.CancelAfter(timeout);
                token = cts.Token;
            }
            try
            {
                await c.Stream.WriteAsync(frame, token).ConfigureAwait(false);
                await c.Stream.FlushAsync(token).ConfigureAwait(false);
                await c.Stream.ReadExactlyAsync(c.Head, token).ConfigureAwait(false);
                reply = new byte[CheckLength(c.Head)];
                await c.Stream.ReadExactlyAsync(reply, token).ConfigureAwait(false);
            }
            catch (OperationCanceledException e) when (!ct.IsCancellationRequested)
            {
                throw new TimeoutException("no reply within " + Seconds(timeout), e);
            }
            finally
            {
                cts?.Dispose();
            }
        }
        else
        {
            int ms = Millis(timeout);
            if (c.Socket.ReceiveTimeout != ms) c.Socket.ReceiveTimeout = ms;
            c.Stream.Write(frame);
            c.Stream.Flush();
            c.Stream.ReadExactly(c.Head);
            reply = new byte[CheckLength(c.Head)];
            c.Stream.ReadExactly(reply);
        }
        c.Exchanges++;
        if (reply[0] == 255) throw KestroException.FromMessage(Encoding.UTF8.GetString(reply, 1, reply.Length - 1));
        return reply;
    }

    private static int CheckLength(byte[] head)
    {
        int length = BinaryPrimitives.ReadInt32LittleEndian(head);
        if (length <= 0 || length > MaxFrame + 4096) throw new IOException("invalid response length " + (uint)length);
        return length;
    }

    /// <summary>
    /// An error the server sent without a request (BUSY right after accepting a connection it will
    /// not serve), when bytes have already arrived; otherwise null. Other unexpected bytes, or a
    /// connection closed meanwhile, come back as a <see cref="KestroIOException"/>.
    /// </summary>
    private static KestroException? Unsolicited(Conn c)
    {
        int available;
        try { available = c.Socket.Available; }
        catch (Exception e) when (e is SocketException or ObjectDisposedException) { return new KestroIOException("IO connection lost", e); }
        if (available == 0) return null;
        // TLS 1.3 session tickets can wait unread until the first reply: not a reply to anything.
        if (c.Tls && c.Exchanges == 0) return null;
        int old = c.Socket.ReceiveTimeout;
        try
        {
            c.Socket.ReceiveTimeout = 1000;
            var head = new byte[4];
            c.Stream.ReadExactly(head);
            int length = BinaryPrimitives.ReadInt32LittleEndian(head);
            if (length <= 0 || length > 64 * 1024) return new KestroIOException("IO the server sent bytes outside a reply");
            var frame = new byte[length];
            c.Stream.ReadExactly(frame);
            if (frame[0] != 255) return new KestroIOException("IO the server sent bytes outside a reply");
            return KestroException.FromMessage(Encoding.UTF8.GetString(frame, 1, frame.Length - 1));
        }
        catch (Exception e) when (e is IOException or SocketException or ObjectDisposedException or InvalidOperationException)
        {
            return new KestroIOException("IO connection lost (" + Describe(e) + ")", e);
        }
        finally
        {
            try { c.Socket.ReceiveTimeout = old; } catch (Exception) { /* closed */ }
        }
    }

    /// <summary>False when the server closed this idle connection.</summary>
    private static bool Alive(Conn c)
    {
        try
        {
            if (!c.Socket.Poll(0, SelectMode.SelectRead)) return true;   // nothing to read: open
            if (c.Socket.Available == 0) return false;                    // readable with nothing: closed
            return c.Tls && c.Exchanges == 0;                             // TLS session tickets
        }
        catch (Exception e) when (e is SocketException or ObjectDisposedException)
        {
            return false;
        }
    }

    private void Drop()
    {
        _conn?.Dispose();
        _conn = null;
    }

    /// <summary>Follows the cluster's leader (see the class comment); one server: as CallOnce.</summary>
    private async ValueTask<byte[]> CallAsync(char command, byte[] payload, bool async, CancellationToken ct)
    {
        byte cmd = (byte)command;
        if (_addresses.Count < 2 || command == 'A' || command == 'Q') return await CallOnceAsync(cmd, payload, async, ct).ConfigureAwait(false);
        long deadline = Stopwatch.GetTimestamp() + (long)(Math.Max(0, _options.FailoverTimeout.TotalSeconds) * Stopwatch.Frequency);
        int wait = 50;
        for (;;)
        {
            try
            {
                return await CallOnceAsync(cmd, payload, async, ct).ConfigureAwait(false);
            }
            catch (KestroException e)
            {
                string message = e.Message;
                bool moved = e.Kind == "READONLY";
                bool unreachable = message.StartsWith("IO cannot connect", StringComparison.Ordinal);
                if ((!moved && !unreachable) || _inTransaction || _closed || Stopwatch.GetTimestamp() > deadline) throw;
                MemberAddress? target = null;
                var named = LeaderPattern.Match(message);
                if (named.Success) target = Member(named.Groups[1].Value);
                if (target is null)
                {
                    if (async) await Task.Delay(wait, ct).ConfigureAwait(false);
                    else Thread.Sleep(wait);
                    wait = Math.Min(wait * 2, 500);
                    int at = 0;
                    for (int i = 0; i < _addresses.Count; i++) if (ReferenceEquals(_addresses[i], _current)) at = i;
                    target = _addresses[(at + 1) % _addresses.Count];
                }
                if (!ReferenceEquals(target, _current) || _conn is null)
                {
                    Drop();
                    _lost = false;   // a move, not a failure: connect even with Reconnect off
                    _current = target;
                }
            }
        }
    }

    /// <summary>The listed member a server named (as written in its --cluster list), or null.</summary>
    private MemberAddress? Member(string name)
    {
        foreach (var a in _addresses) if (a.Text == name) return a;
        try
        {
            string key = new MemberAddress(name).Key;
            foreach (var a in _addresses) if (a.Key == key) return a;
        }
        catch (KestroException)
        {
            // not an address
        }
        return null;
    }

    private async ValueTask<byte[]> CallOnceAsync(byte cmd, byte[] payload, bool async, CancellationToken ct)
    {
        if (_closed) throw new ClosedException("CLOSED the connection was closed");
        if (payload.Length > MaxFrame - 1) throw new LimitException("LIMIT request too large");
        if (_conn is not null && cmd != 'A')
        {
            // An error the server sent on its own (BUSY) means it closed the connection.
            var early = Unsolicited(_conn);
            if (early is not null)
            {
                Drop();
                _lost = true;
                if (early is BusyException && !_inTransaction) throw early;
            }
        }
        if (_conn is not null && _options.Reconnect && !_inTransaction && cmd != 'A'
            && Stopwatch.GetTimestamp() - _lastUse > OneSecond && !Alive(_conn))
        {
            Drop();
            _lost = true;
        }
        if (_conn is null)
        {
            if (_inTransaction)
            {
                _inTransaction = false;
                _implicitSavepoint = null;
                throw new KestroIOException("IO connection lost inside a transaction; it was rolled back");
            }
            if (cmd == 'Q') return new byte[] { 0 };
            if (_lost && !_options.Reconnect) throw new KestroIOException("IO the connection was lost (reconnect is off)");
            await OpenAsync(async, ct).ConfigureAwait(false);
        }
        try
        {
            return await ExchangeAsync(_conn!, cmd, payload, _options.Timeout, async, ct).ConfigureAwait(false);
        }
        catch (BusyException)
        {
            Drop();   // the server closes a connection it answered BUSY
            _lost = true;
            throw;
        }
        catch (KestroException)
        {
            throw;
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested)
        {
            // The reply may still come: the connection cannot be used again. A transaction on it is
            // gone, which the next call reports.
            Drop();
            _lost = true;
            throw;
        }
        catch (Exception e) when (e is IOException or SocketException or ObjectDisposedException or TimeoutException or OperationCanceledException or AuthenticationException or InvalidOperationException)
        {
            Drop();
            _lost = true;
            bool tx = _inTransaction;
            _inTransaction = false;
            _implicitSavepoint = null;
            throw new KestroIOException(tx
                ? "IO connection lost inside a transaction; it was rolled back (" + Describe(e) + ")"
                : "IO connection lost; a write without a reply has an unknown outcome (" + Describe(e) + ")", e);
        }
        finally
        {
            _lastUse = Stopwatch.GetTimestamp();
        }
    }

    // ------------------------------------------------------------------ locking

    private bool OwnsGate => _owner.Value is { } o && ReferenceEquals(o, Volatile.Read(ref _gateOwner));

    private readonly struct Releaser : IDisposable
    {
        private readonly KestroClient _client;
        private readonly bool _gate;

        public Releaser(KestroClient client, bool gate)
        {
            _client = client;
            _gate = gate;
        }

        public void Dispose()
        {
            _client._io.Release();
            if (_gate) _client._gate.Release();
        }
    }

    private async ValueTask<Releaser> LockAsync(bool async, CancellationToken ct)
    {
        bool gate = !OwnsGate;
        if (gate)
        {
            if (async) await _gate.WaitAsync(ct).ConfigureAwait(false);
            else _gate.Wait(ct);
        }
        try
        {
            if (async) await _io.WaitAsync(ct).ConfigureAwait(false);
            else _io.Wait(ct);
        }
        catch
        {
            if (gate) _gate.Release();
            throw;
        }
        return new Releaser(this, gate);
    }

    /// <summary>The result of a ValueTask that ran with async = false (it completed synchronously).</summary>
    private static T Wait<T>(ValueTask<T> task) => task.IsCompletedSuccessfully ? task.Result : task.AsTask().GetAwaiter().GetResult();

    private static void Wait(ValueTask task)
    {
        if (task.IsCompletedSuccessfully) return;
        task.AsTask().GetAwaiter().GetResult();
    }

    // ------------------------------------------------------------------ close

    private async ValueTask CloseAsync(bool async)
    {
        if (_closed) return;
        using (await LockAsync(async, default).ConfigureAwait(false))
        {
            if (_closed) return;
            if (_conn is not null)
            {
                try
                {
                    TimeSpan t = Limited(_options.Timeout) && _options.Timeout < TimeSpan.FromSeconds(5) ? _options.Timeout : TimeSpan.FromSeconds(5);
                    await ExchangeAsync(_conn, (byte)'Q', NoBytes, t, async, default).ConfigureAwait(false);
                }
                catch (Exception)
                {
                    // closing anyway
                }
                Drop();
            }
            _closed = true;
            _inTransaction = false;
            _implicitSavepoint = null;
        }
    }

    /// <summary>Ends the connection (an open transaction is rolled back by the server). Twice is fine.</summary>
    public ValueTask DisposeAsync() => CloseAsync(true);

    /// <summary>Ends the connection (an open transaction is rolled back by the server). Twice is fine.</summary>
    public void Dispose() => Wait(CloseAsync(false));

    /// <summary>The same as <see cref="Dispose"/>.</summary>
    public void Close() => Dispose();
}

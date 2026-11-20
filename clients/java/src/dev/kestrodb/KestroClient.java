package dev.kestrodb;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.Channels;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.security.KeyStore;
import java.security.SecureRandom;
import java.security.cert.Certificate;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import javax.net.ssl.SNIHostName;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

/**
 * A connection to kestro-server.
 *
 * <pre>{@code
 * try (KestroClient db = KestroClient.connect("tcp://127.0.0.1:7820", System.getenv("KESTRODB_TOKEN"))) {
 *     db.execute("INSERT INTO users (email, name) VALUES (:email, :name)", Map.of("email", "a@x.org", "name", "Ann"));
 *     Map<String, Object> user = db.queryOne("SELECT * FROM users WHERE id = ?", db.lastInsertId());
 *     db.transaction(tx -> {
 *         tx.execute("UPDATE accounts SET balance = balance - ? WHERE id = ?", 10, 1);
 *         tx.execute("UPDATE accounts SET balance = balance + ? WHERE id = ?", 10, 2);
 *     });
 *     long v = db.set("users", "kestro", Map.of("name", "Kestro"));
 *     db.set("users", "kestro", Map.of("name", "K"), v);   // only if still at version v
 * }
 * }</pre>
 *
 * <p>Addresses: {@code tcp://host:port}, {@code tls://host:port}, {@code unix:///path}, or
 * {@code host:port}. Several comma-separated addresses (or a list) name a cluster's members:
 * writes go to the leader, which a follower's {@code READONLY} reply names; the client moves
 * there and sends the request again (nothing was applied), also during an election, for up to
 * {@link Options#failoverTimeout}. Never inside a transaction.
 *
 * <p>A connection the server closed while idle (a restart, the network) is reopened before
 * the next request, except inside a transaction: that call fails with {@code IO ... connection
 * lost inside a transaction; it was rolled back}. A request already sent when the connection
 * broke fails with {@link ConnectionException} (for a write: unknown outcome).
 *
 * <p>Calls are serialized, so a client may be shared by threads, but a transaction belongs to
 * the connection: use one client per thread when threads use transactions.
 */
public final class KestroClient implements AutoCloseable {
    /** This client's version (the KestroDB release it was written for). */
    public static final String VERSION = "0.9.0";
    /** Largest frame either way (64 MB), as on the server. */
    public static final int MAX_FRAME = 64 * 1024 * 1024;
    /** Largest JSON value of a key (256 KiB). */
    public static final int MAX_VALUE = 256 * 1024;

    private static final Pattern COLLECTION = Pattern.compile("[A-Za-z_][A-Za-z0-9_]{0,63}");
    private static final Pattern LEADER = Pattern.compile("the leader is (\\S+)");
    private static final Pattern COMMIT_OR_END = Pattern.compile("^\\s*(COMMIT|END)\\b", Pattern.CASE_INSENSITIVE);
    private static final byte[] NO_BYTES = new byte[0];

    /** Connection settings; every setter returns this. */
    public static final class Options {
        Duration timeout = Duration.ofSeconds(30);
        Duration connectTimeout;
        boolean reconnect = true;
        Duration failoverTimeout = Duration.ofSeconds(15);
        Path caFile;
        boolean verify = true;
        String serverName;
        SSLContext sslContext;

        /** Socket read timeout for every reply (default 30 s; 0: none). Also the connect timeout unless set. */
        public Options timeout(Duration d) { timeout = d; return this; }

        /** Time allowed to open the TCP connection (default: {@link #timeout}). */
        public Options connectTimeout(Duration d) { connectTimeout = d; return this; }

        /** Reopen a connection the server closed while idle (default true). */
        public Options reconnect(boolean on) { reconnect = on; return this; }

        /** How long a cluster client keeps looking for the leader (default 15 s). */
        public Options failoverTimeout(Duration d) { failoverTimeout = d; return this; }

        /** tls://: trust only the CA certificates in this PEM file (default: the JVM's trust store). */
        public Options caFile(String path) { caFile = path == null ? null : Paths.get(path); return this; }

        public Options caFile(Path path) { caFile = path; return this; }

        /** tls://: false skips certificate and host name checks (testing only). */
        public Options verify(boolean on) { verify = on; return this; }

        /** tls://: the name to send (SNI) and check the certificate against, instead of the address's host. */
        public Options serverName(String name) { serverName = name; return this; }

        /** tls://: your own SSLContext (key material, trust), instead of caFile/verify. */
        public Options sslContext(SSLContext context) { sslContext = context; return this; }
    }

    /** One parsed member address. */
    private static final class Address {
        final String text;
        final String host;
        final int port;
        final boolean tls;
        final String unixPath;

        /** The same member however it is written: tcp://host:port, tls://host:port or unix://path. */
        String key() { return unixPath != null ? text : (tls ? "tls://" : "tcp://") + host.toLowerCase(Locale.ROOT) + ":" + port; }

        Address(String text) {
            this.text = text;
            if (text.startsWith("unix://")) {
                unixPath = text.substring(7);
                host = null; port = 0; tls = false;
                if (unixPath.isEmpty()) throw new InputException("INPUT address must be tcp://host:port, tls://host:port or unix:///path");
                return;
            }
            unixPath = null;
            tls = text.startsWith("tls://");
            String hp = text.startsWith("tcp://") || tls ? text.substring(6) : text;
            int colon = hp.lastIndexOf(':');
            if (colon < 0) throw new InputException("INPUT address must be tcp://host:port, tls://host:port or unix:///path: " + text);
            String h = hp.substring(0, colon);
            if (h.startsWith("[") && h.endsWith("]")) h = h.substring(1, h.length() - 1);
            host = h.isEmpty() ? "127.0.0.1" : h;
            int p;
            try { p = Integer.parseInt(hp.substring(colon + 1)); } catch (NumberFormatException e) { p = -1; }
            if (p < 1 || p > 65535) throw new InputException("INPUT invalid port in address " + text);
            port = p;
        }
    }

    /** An open connection: plain or TLS socket, or a Unix socket channel. */
    private static final class Conn {
        Socket socket;
        SocketChannel channel;
        InputStream in;
        OutputStream out;

        void close() {
            try { if (socket != null) socket.close(); } catch (IOException ignored) { }
            try { if (channel != null) channel.close(); } catch (IOException ignored) { }
        }
    }

    private final List<Address> addresses;
    private final String token;
    private final Options options;
    private final int timeoutMs;
    private Address current;
    private Conn conn;
    private boolean closed;
    private boolean lost;            // the connection broke (as opposed to a move to another member)
    private long lastUse;
    private volatile boolean inTransaction;
    private String implicitSavepoint; // a SAVEPOINT that began the transaction: releasing it commits
    private volatile Object lastInsertId;
    private SSLContext ssl;
    int opens;                       // connections opened (tests: kept or reopened)

    private KestroClient(List<String> list, String token, Options options) {
        List<Address> parsed = new ArrayList<>();
        for (String a : list) {
            if (a == null) continue;
            String t = a.trim();
            if (!t.isEmpty()) parsed.add(new Address(t));
        }
        if (parsed.isEmpty()) throw new InputException("INPUT no server address");
        this.addresses = Collections.unmodifiableList(parsed);
        this.token = token;
        this.options = options == null ? new Options() : options;
        long ms = this.options.timeout == null ? 0 : this.options.timeout.toMillis();
        this.timeoutMs = (int) Math.min(Integer.MAX_VALUE, Math.max(0, ms));
        this.current = parsed.get(0);
    }

    // ------------------------------------------------------------------ connect

    /** Connects to tcp://127.0.0.1:7820 without a token. */
    public static KestroClient connect() { return connect("tcp://127.0.0.1:7820", null, null); }

    /** Connects without a token (a server started with --no-auth, or loopback without users). */
    public static KestroClient connect(String address) { return connect(address, null, null); }

    public static KestroClient connect(String address, String token) { return connect(address, token, null); }

    /** address: one member, or several separated by commas (a cluster). token: null for none. */
    public static KestroClient connect(String address, String token, Options options) {
        if (address == null) throw new InputException("INPUT no server address");
        return connect(Arrays.asList(address.split(",")), token, options);
    }

    /** A cluster's members (any order); the client finds the leader. */
    public static KestroClient connect(List<String> addresses, String token, Options options) {
        KestroClient c = new KestroClient(addresses, token, options);
        synchronized (c) { c.open(); }
        return c;
    }

    /** The member this client uses now (as written in the address list). */
    public synchronized String address() { return current.text; }

    /** True between BEGIN (or START TRANSACTION, or a SAVEPOINT outside one) and its COMMIT/ROLLBACK. */
    public boolean inTransaction() { return inTransaction; }

    /** The id of the last row an INSERT on this connection created (Long, or String for text ids), or null. */
    public Object lastInsertId() { return lastInsertId; }

    public synchronized boolean isClosed() { return closed; }

    // The current address first, then the other members.
    private void open() {
        List<Address> order = new ArrayList<>();
        order.add(current);
        for (Address a : addresses) if (a != current) order.add(a);
        KestroException last = null;
        for (Address a : order) {
            try {
                conn = openOne(a);
                current = a;
                lost = false;
                opens++;
                lastUse = System.nanoTime();
                return;
            } catch (AuthException e) {
                throw e;
            } catch (KestroException e) {
                last = e;
            }
        }
        throw last;
    }

    private Conn openOne(Address a) {
        Conn c = new Conn();
        try {
            if (a.unixPath != null) {
                c.channel = SocketChannel.open(StandardProtocolFamily.UNIX);
                c.channel.connect(UnixDomainSocketAddress.of(a.unixPath));
                c.in = new BufferedInputStream(Channels.newInputStream(c.channel), 64 * 1024);
                c.out = Channels.newOutputStream(c.channel);
            } else {
                Socket s = new Socket();
                c.socket = s;
                s.setTcpNoDelay(true);
                s.setKeepAlive(true);
                Duration ct = options.connectTimeout != null ? options.connectTimeout : options.timeout;
                int connectMs = ct == null ? 0 : (int) Math.min(Integer.MAX_VALUE, Math.max(0, ct.toMillis()));
                s.connect(new InetSocketAddress(a.host, a.port), connectMs);
                s.setSoTimeout(timeoutMs);
                if (a.tls) {
                    String name = options.serverName != null ? options.serverName : a.host;
                    SSLSocket t = (SSLSocket) sslContext().getSocketFactory().createSocket(s, name, a.port, true);
                    c.socket = t;
                    SSLParameters p = t.getSSLParameters();
                    List<String> protocols = new ArrayList<>();
                    for (String proto : t.getSupportedProtocols()) if (proto.equals("TLSv1.3") || proto.equals("TLSv1.2")) protocols.add(proto);
                    p.setProtocols(protocols.toArray(new String[0]));
                    p.setEndpointIdentificationAlgorithm(options.verify ? "HTTPS" : null);
                    if (!isIpLiteral(name)) p.setServerNames(Collections.singletonList(new SNIHostName(name)));
                    else p.setServerNames(Collections.emptyList());   // SNI never carries an IP address
                    t.setSSLParameters(p);
                    t.setSoTimeout(timeoutMs);
                    try {
                        t.startHandshake();
                    } catch (IOException e) {
                        throw new IOException("TLS handshake failed: " + e.getMessage(), e);
                    }
                }
                c.in = new BufferedInputStream(c.socket.getInputStream(), 64 * 1024);
                c.out = c.socket.getOutputStream();
            }
        } catch (IOException | RuntimeException e) {
            c.close();
            if (e instanceof KestroException) throw (KestroException) e;
            throw new ConnectionException("IO cannot connect to " + a.text + ": " + describe(e), e);
        }
        if (token != null) {
            try {
                KestroException early = unsolicited(c);   // BUSY already here: report it, do not log in
                if (early != null) throw early;
                exchange(c, (byte) 'A', Json.utf8(token));
            } catch (IOException e) {
                c.close();
                // A server at its connection limit sends BUSY and closes at once; the reset that
                // follows can discard the BUSY reply before it is read.
                throw new ConnectionException("IO cannot connect to " + a.text + ": " + describe(e)
                        + " (the server closed the connection before login; it may be at its connection limit)", e);
            } catch (KestroException e) {
                c.close();
                throw e;
            }
        }
        return c;
    }

    private static boolean isIpLiteral(String host) {
        return host.indexOf(':') >= 0 || host.matches("[0-9.]+");
    }

    private static String describe(Throwable e) {
        String m = e.getMessage();
        return m == null || m.isEmpty() ? e.getClass().getSimpleName() : m;
    }

    private synchronized SSLContext sslContext() throws IOException {
        if (options.sslContext != null) return options.sslContext;
        if (ssl != null) return ssl;
        try {
            SSLContext ctx = SSLContext.getInstance("TLS");
            if (!options.verify) {
                TrustManager trustAll = new X509TrustManager() {
                    public void checkClientTrusted(X509Certificate[] chain, String authType) { }
                    public void checkServerTrusted(X509Certificate[] chain, String authType) { }
                    public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
                };
                ctx.init(null, new TrustManager[] {trustAll}, new SecureRandom());
            } else if (options.caFile != null) {
                KeyStore store = KeyStore.getInstance(KeyStore.getDefaultType());
                store.load(null, null);
                CertificateFactory cf = CertificateFactory.getInstance("X.509");
                Collection<? extends Certificate> certs;
                try (InputStream in = Files.newInputStream(options.caFile)) { certs = cf.generateCertificates(in); }
                if (certs.isEmpty()) throw new IOException("no certificate in " + options.caFile);
                int i = 0;
                for (Certificate cert : certs) store.setCertificateEntry("ca" + (i++), cert);
                TrustManagerFactory tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
                tmf.init(store);
                ctx.init(null, tmf.getTrustManagers(), new SecureRandom());
            } else {
                ctx.init(null, null, null);   // the JVM's default trust store
            }
            ssl = ctx;
            return ctx;
        } catch (IOException e) {
            throw e;
        } catch (Exception e) {
            throw new IOException("TLS setup: " + describe(e), e);
        }
    }

    // ------------------------------------------------------------------ frames

    /** Sends one request on c and reads its reply; an error reply becomes a KestroException. */
    private static byte[] exchange(Conn c, byte cmd, byte[] payload) throws IOException {
        byte[] frame = new byte[5 + payload.length];
        putInt(frame, 0, 1 + payload.length);
        frame[4] = cmd;
        System.arraycopy(payload, 0, frame, 5, payload.length);
        c.out.write(frame);
        c.out.flush();
        byte[] head = readFully(c.in, 4);
        int length = getInt(head, 0);
        if (length <= 0 || length > MAX_FRAME + 4096) throw new IOException("invalid response length " + Integer.toUnsignedString(length));
        byte[] reply = readFully(c.in, length);
        if ((reply[0] & 0xFF) == 255) throw KestroException.of(new String(reply, 1, reply.length - 1, StandardCharsets.UTF_8));
        return reply;
    }

    private static byte[] readFully(InputStream in, int n) throws IOException {
        byte[] b = new byte[n];
        int got = 0;
        while (got < n) {
            int r = in.read(b, got, n - got);
            if (r < 0) throw new EOFException("connection closed");
            got += r;
        }
        return b;
    }

    /**
     * An error the server sent without a request (BUSY right after accepting a connection
     * it will not serve), when it has already arrived; otherwise null. Costs one available().
     * Other unexpected bytes come back as a ConnectionException.
     */
    private static KestroException unsolicited(Conn c) throws IOException {
        if (c.in.available() < 4) return null;
        byte[] head = readFully(c.in, 4);
        int length = getInt(head, 0);
        if (length <= 0 || length > 64 * 1024) return new ConnectionException("IO the server sent bytes outside a reply");
        byte[] frame = readFully(c.in, length);
        if ((frame[0] & 0xFF) != 255) return new ConnectionException("IO the server sent bytes outside a reply");
        return KestroException.of(new String(frame, 1, frame.length - 1, StandardCharsets.UTF_8));
    }

    /** False when the server closed this idle connection (or sent bytes outside a reply). */
    private boolean alive(Conn c) {
        try {
            if (c.in.available() > 0) return false;
            if (c.channel != null) {
                c.channel.configureBlocking(false);
                try {
                    return c.channel.read(ByteBuffer.allocate(1)) == 0;
                } finally {
                    c.channel.configureBlocking(true);
                }
            }
            c.socket.setSoTimeout(1);
            try {
                c.in.read();      // -1: closed; a byte: not a reply to anything
                return false;
            } catch (SocketTimeoutException e) {
                return true;      // nothing to read: the connection is open
            } finally {
                c.socket.setSoTimeout(timeoutMs);
            }
        } catch (IOException | RuntimeException e) {
            return false;
        }
    }

    private void drop() {
        if (conn != null) conn.close();
        conn = null;
    }

    /** Follows the cluster's leader (see the class comment); one server: as callOnce. */
    private synchronized byte[] call(char command, byte[] payload) {
        byte cmd = (byte) command;
        if (addresses.size() < 2 || command == 'A' || command == 'Q') return callOnce(cmd, payload);
        long failoverNanos = options.failoverTimeout == null ? 0 : options.failoverTimeout.toNanos();
        long deadline = System.nanoTime() + failoverNanos;
        long wait = 50;
        for (;;) {
            try {
                return callOnce(cmd, payload);
            } catch (KestroException e) {
                String message = e.getMessage() == null ? "" : e.getMessage();
                boolean moved = e.kind().equals("READONLY");
                boolean unreachable = message.startsWith("IO cannot connect");
                if ((!moved && !unreachable) || inTransaction || closed || System.nanoTime() - deadline > 0) throw e;
                Matcher named = LEADER.matcher(message);
                Address target = null;
                if (named.find()) target = member(named.group(1));
                if (target == null) {
                    sleep(wait);
                    wait = Math.min(wait * 2, 500);
                    target = addresses.get((addresses.indexOf(current) + 1) % addresses.size());
                }
                if (target != current || conn == null) {
                    drop();
                    lost = false;   // a move, not a failure: connect even with reconnect off
                    current = target;
                }
            }
        }
    }

    /** The listed member a server named (as written in its --cluster list), or null. */
    private Address member(String name) {
        for (Address a : addresses) if (a.text.equals(name)) return a;
        try {
            String key = new Address(name).key();
            for (Address a : addresses) if (a.key().equals(key)) return a;
        } catch (KestroException ignored) {
            // not an address
        }
        return null;
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new ConnectionException("IO interrupted", e);
        }
    }

    private byte[] callOnce(byte cmd, byte[] payload) {
        if (closed) throw new ClosedException("CLOSED the connection was closed");
        if (payload.length > MAX_FRAME - 1) throw new LimitException("LIMIT request too large");
        if (conn != null && cmd != 'A') {
            // An error the server sent on its own (BUSY) means it closed the connection.
            KestroException early;
            try { early = unsolicited(conn); } catch (IOException e) { early = new ConnectionException("IO connection lost", e); }
            if (early != null) {
                drop();
                lost = true;
                if (early instanceof BusyException && !inTransaction) throw early;
            }
        }
        if (conn != null && options.reconnect && !inTransaction && cmd != 'A'
                && System.nanoTime() - lastUse > 1_000_000_000L && !alive(conn)) {
            drop();
            lost = true;
        }
        if (conn == null) {
            if (inTransaction) {
                inTransaction = false;
                implicitSavepoint = null;
                throw new ConnectionException("IO connection lost inside a transaction; it was rolled back");
            }
            if (cmd == 'Q') return new byte[] {0};
            if (lost && !options.reconnect) throw new ConnectionException("IO the connection was lost (reconnect is off)");
            open();
        }
        byte[] reply;
        try {
            reply = exchange(conn, cmd, payload);
        } catch (IOException e) {
            drop();
            lost = true;
            boolean tx = inTransaction;
            inTransaction = false;
            implicitSavepoint = null;
            throw new ConnectionException(tx ? "IO connection lost inside a transaction; it was rolled back (" + describe(e) + ")"
                    : "IO connection lost; a write without a reply has an unknown outcome (" + describe(e) + ")", e);
        } finally {
            lastUse = System.nanoTime();
        }
        return reply;
    }

    // ------------------------------------------------------------------ SQL

    /**
     * One SQL statement (see SQL.md) with positional parameters for {@code ?}, {@code ?N} and
     * {@code $N}. A parameter is null, Boolean, a Number, a String, a Map or List (JSON), or
     * a java.time value / UUID / enum (as text).
     */
    public SqlResult sql(String text, Object... params) { return run(text, positional(params)); }

    /** Positional parameters as a list. */
    public SqlResult sql(String text, List<?> params) { return run(text, positional(params == null ? null : params.toArray())); }

    /** Named parameters for {@code :name} and {@code @name}; keys may include the leading : or @. */
    public SqlResult sql(String text, Map<String, ?> params) { return run(text, named(params)); }

    private static byte[] positional(Object[] params) {
        if (params == null || params.length == 0) return new byte[] {'[', ']'};
        return Json.toBytes(Arrays.asList(params));
    }

    private static byte[] named(Map<String, ?> params) {
        if (params == null) return new byte[] {'[', ']'};
        Map<String, Object> m = new LinkedHashMap<>();
        for (Map.Entry<String, ?> e : params.entrySet()) {
            String k = e.getKey();
            if (k == null) throw new InputException("INPUT a parameter name cannot be null");
            if (k.startsWith(":") || k.startsWith("@")) k = k.substring(1);
            m.put(k, e.getValue());
        }
        return Json.toBytes(m);
    }

    private synchronized SqlResult run(String text, byte[] paramsJson) {
        if (text == null) throw new InputException("INPUT sql text must be a string");
        byte[] t = Json.utf8(text);
        byte[] payload = new byte[4 + t.length + paramsJson.length];
        putInt(payload, 0, t.length);
        System.arraycopy(t, 0, payload, 4, t.length);
        System.arraycopy(paramsJson, 0, payload, 4 + t.length, paramsJson.length);
        byte[] reply;
        try {
            reply = call('E', payload);
        } catch (KestroException e) {
            // A COMMIT that fails (a conflict) has rolled the transaction back on the server.
            if (!e.kind().equals("IO") && COMMIT_OR_END.matcher(text).find()) {
                inTransaction = false;
                implicitSavepoint = null;
            }
            throw e;
        }
        SqlResult r = new SqlResult(Json.parse(reply, 1, reply.length - 1));
        track(text);
        if (r.raw().containsKey("lastInsertId")) lastInsertId = r.lastInsertId();
        return r;
    }

    // BEGIN/COMMIT/ROLLBACK (and savepoints) sent as SQL keep inTransaction right.
    private void track(String text) {
        String[] words = text.trim().split("\\s+", 4);
        String w0 = words[0].toUpperCase(Locale.ROOT);
        if (w0.endsWith(";")) w0 = w0.substring(0, w0.length() - 1);
        switch (w0) {
            case "BEGIN": case "START":
                inTransaction = true;
                implicitSavepoint = null;
                break;
            case "COMMIT": case "END":
                inTransaction = false;
                implicitSavepoint = null;
                break;
            case "ROLLBACK":
                // ROLLBACK TO [SAVEPOINT] name keeps the transaction open.
                if (words.length < 2 || !words[1].equalsIgnoreCase("TO")) {
                    inTransaction = false;
                    implicitSavepoint = null;
                }
                break;
            case "SAVEPOINT":
                if (!inTransaction && words.length >= 2) {
                    inTransaction = true;
                    implicitSavepoint = unquote(words[1]);
                }
                break;
            case "RELEASE": {
                String name = words.length >= 3 && words[1].equalsIgnoreCase("SAVEPOINT") ? words[2] : words.length >= 2 ? words[1] : null;
                if (name != null && implicitSavepoint != null && unquote(name).equalsIgnoreCase(implicitSavepoint)) {
                    inTransaction = false;
                    implicitSavepoint = null;
                }
                break;
            }
            default:
        }
    }

    private static String unquote(String w) {
        String s = w.endsWith(";") ? w.substring(0, w.length() - 1) : w;
        if (s.length() >= 2 && (s.charAt(0) == '"' || s.charAt(0) == '`') && s.charAt(s.length() - 1) == s.charAt(0)) s = s.substring(1, s.length() - 1);
        return s;
    }

    private static List<Map<String, Object>> rowsOf(SqlResult r) {
        if (!r.hasRows()) throw new InputException("INPUT query() needs a statement that returns rows; use execute()");
        return r.rows();
    }

    private static Map<String, Object> first(List<Map<String, Object>> rows) { return rows.isEmpty() ? null : rows.get(0); }

    private static Object firstValue(Map<String, Object> row) {
        if (row == null || row.isEmpty()) return null;
        return row.values().iterator().next();
    }

    /** All rows of a SELECT (or another statement that returns rows). */
    public List<Map<String, Object>> query(String text, Object... params) { return rowsOf(sql(text, params)); }

    public List<Map<String, Object>> query(String text, List<?> params) { return rowsOf(sql(text, params)); }

    public List<Map<String, Object>> query(String text, Map<String, ?> params) { return rowsOf(sql(text, params)); }

    /** The first row, or null. */
    public Map<String, Object> queryOne(String text, Object... params) { return first(query(text, params)); }

    public Map<String, Object> queryOne(String text, List<?> params) { return first(query(text, params)); }

    public Map<String, Object> queryOne(String text, Map<String, ?> params) { return first(query(text, params)); }

    /** The first column of the first row, or null. */
    public Object queryValue(String text, Object... params) { return firstValue(queryOne(text, params)); }

    public Object queryValue(String text, List<?> params) { return firstValue(queryOne(text, params)); }

    public Object queryValue(String text, Map<String, ?> params) { return firstValue(queryOne(text, params)); }

    /** INSERT, UPDATE, DELETE or DDL: the number of rows changed. */
    public long execute(String text, Object... params) { return sql(text, params).changes(); }

    public long execute(String text, List<?> params) { return sql(text, params).changes(); }

    public long execute(String text, Map<String, ?> params) { return sql(text, params).changes(); }

    /** A statement to run many times: {@code all/one/value/run}. */
    public Prepared prepare(String text) { return new Prepared(this, text); }

    /** BEGIN. A transaction holds the write gate until COMMIT or ROLLBACK. */
    public synchronized void begin() {
        if (inTransaction) throw new InputException("INPUT a transaction is already open");
        sql("BEGIN");
    }

    /** COMMIT; nothing to do without an open transaction. A CONFLICT here rolled it back. */
    public synchronized void commit() { if (inTransaction) sql("COMMIT"); }

    /** ROLLBACK; nothing to do without an open transaction. */
    public synchronized void rollback() { if (inTransaction) sql("ROLLBACK"); }

    /** Runs body inside BEGIN ... COMMIT; on an exception: ROLLBACK and rethrow. */
    public synchronized void transaction(Consumer<KestroClient> body) {
        transactionResult(db -> { body.accept(db); return null; });
    }

    /** As {@link #transaction(Consumer)}, returning body's result. */
    public synchronized <T> T transactionResult(Function<KestroClient, T> body) {
        begin();
        try {
            T result = body.apply(this);
            commit();
            return result;
        } catch (RuntimeException | Error e) {
            if (inTransaction) {
                try { rollback(); } catch (RuntimeException r) { e.addSuppressed(r); }
            }
            throw e;
        }
    }

    // ------------------------------------------------------------------ key/value

    private static byte[] id(String collection, String key) {
        if (collection == null || !COLLECTION.matcher(collection).matches())
            throw new InputException("INPUT Collection must be a 1-64 character ASCII identifier.");
        if (key == null) throw new InputException("INPUT Key must be a string.");
        byte[] k = Json.utf8(key);
        if (k.length == 0 || k.length > 512) throw new InputException("INPUT Key must contain 1-512 UTF-8 bytes and no NUL.");
        for (byte b : k) if (b == 0) throw new InputException("INPUT Key must contain 1-512 UTF-8 bytes and no NUL.");
        byte[] c = collection.getBytes(StandardCharsets.US_ASCII);
        byte[] out = new byte[c.length + 1 + k.length];
        System.arraycopy(c, 0, out, 0, c.length);
        System.arraycopy(k, 0, out, c.length + 1, k.length);
        return out;
    }

    private static byte[] encodeValue(Object value) {
        byte[] v = Json.toBytes(value);
        if (v.length > MAX_VALUE) throw new LimitException("LIMIT A value may contain at most 256 KiB of JSON.");
        return v;
    }

    /** The value and version of a key, or null when it does not exist. */
    public Versioned get(String collection, String key) {
        byte[] reply = call('G', id(collection, key));
        if (reply[0] == 1) return null;
        if (reply.length < 9) throw new ConnectionException("IO short reply");
        return new Versioned(getLong(reply, 1), Json.parse(reply, 9, reply.length - 9));
    }

    /** Up to 4096 keys read in one consistent snapshot, in order; null where a key is missing. */
    public List<Versioned> getMany(String collection, List<String> keys) {
        if (keys == null) throw new InputException("INPUT keys");
        if (keys.size() > 4096) throw new LimitException("LIMIT getMany count");
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] n = new byte[4];
        putInt(n, 0, keys.size());
        b.write(n, 0, 4);
        for (String k : keys) {
            byte[] i = id(collection, k);
            putInt(n, 0, i.length);
            b.write(n, 0, 4);
            b.write(i, 0, i.length);
        }
        byte[] reply = call('M', b.toByteArray());
        try {
            int count = getInt(reply, 1), p = 5;
            if (count != keys.size()) throw new ConnectionException("IO getMany reply count");
            List<Versioned> out = new ArrayList<>(count);
            for (int i = 0; i < count; i++) {
                if (reply[p++] == 0) { out.add(null); continue; }
                long version = getLong(reply, p);
                int length = getInt(reply, p + 8);
                p += 12;
                out.add(new Versioned(version, Json.parse(reply, p, length)));
                p += length;
            }
            return out;
        } catch (IndexOutOfBoundsException e) {
            throw new ConnectionException("IO malformed getMany reply", e);
        }
    }

    public List<Versioned> getMany(String collection, String... keys) { return getMany(collection, Arrays.asList(keys)); }

    /** Stores a JSON value; returns its new version. */
    public long set(String collection, String key, Object value) { return batch(Op.set(collection, key, value)); }

    /** Stores a value only if the key is at expectedVersion (0: must not exist); else {@link ConflictException}. */
    public long set(String collection, String key, Object value, long expectedVersion) { return batch(Op.set(collection, key, value, expectedVersion)); }

    /** Deletes a key (deleting a missing key is not an error); returns the transaction's version. */
    public long delete(String collection, String key) { return batch(Op.delete(collection, key)); }

    public long delete(String collection, String key, long expectedVersion) { return batch(Op.delete(collection, key, expectedVersion)); }

    public long batch(Op... operations) { return batch(Arrays.asList(operations)); }

    /** All-or-nothing and durable when it returns; the version shared by every operation. */
    public long batch(List<Op> operations) {
        if (operations == null || operations.isEmpty() || operations.size() > 65536) throw new InputException("INPUT operation count");
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] h = new byte[17];
        putInt(h, 0, operations.size());
        b.write(h, 0, 4);
        Set<String> seen = new HashSet<>();
        for (Op op : operations) {
            if (op == null) throw new InputException("INPUT operation type");
            byte[] i = id(op.collection, op.key);
            if (!seen.add(op.collection + '\0' + op.key)) throw new InputException("INPUT duplicate transaction key");
            byte[] v = op.set ? encodeValue(op.value) : NO_BYTES;
            h[0] = (byte) (op.set ? 1 : 0);
            putLong(h, 1, op.expectedVersion < 0 ? -1L : op.expectedVersion);
            putInt(h, 9, i.length);
            putInt(h, 13, v.length);
            b.write(h, 0, 17);
            b.write(i, 0, i.length);
            b.write(v, 0, v.length);
            if (b.size() > MAX_FRAME - 64) throw new LimitException("LIMIT transaction bytes");
        }
        byte[] reply = call('B', b.toByteArray());
        if (reply.length < 9) throw new ConnectionException("IO short reply");
        return getLong(reply, 1);
    }

    // ------------------------------------------------------------------ server

    @SuppressWarnings("unchecked")
    private static Map<String, Object> jsonObject(byte[] reply) {
        Object o = Json.parse(reply, 1, reply.length - 1);
        if (!(o instanceof Map)) throw new ConnectionException("IO the server's reply is not a JSON object");
        return (Map<String, Object>) o;
    }

    /** Server statistics: keys, bytes, txid, connections, tls, user, role, cluster, ... */
    public Map<String, Object> stats() { return jsonObject(call('S', NO_BYTES)); }

    /** Checks every checksum of the database file (admin). */
    public void verify() { call('V', NO_BYTES); }

    /** Online backup to a plain file name in the server's --backup-dir (admin). */
    public void backup(String name) { call('P', Json.utf8(checkName(name))); }

    /** A compacted copy as a plain file name in the server's --backup-dir (admin). */
    public void compactTo(String name) { call('C', Json.utf8(checkName(name))); }

    /** Turns a replica into a writable server (admin); it stops following its leader. */
    public void promote() { call('X', NO_BYTES); }

    /** Online compaction of the server's file (admin): {before, after, keys}. */
    public Map<String, Object> compact() { return jsonObject(call('K', NO_BYTES)); }

    private static String checkName(String name) {
        if (name == null || name.isEmpty()) throw new InputException("INPUT a backup name is a plain file name");
        return name;
    }

    /** Ends the connection (an open transaction is rolled back by the server). */
    @Override
    public synchronized void close() {
        if (closed) return;
        if (conn != null) {
            try { exchange(conn, (byte) 'Q', NO_BYTES); } catch (IOException | RuntimeException ignored) { }
            drop();
        }
        closed = true;
        inTransaction = false;
        implicitSavepoint = null;
    }

    // ------------------------------------------------------------------ bytes (little-endian)

    static void putInt(byte[] b, int at, int v) {
        b[at] = (byte) v; b[at + 1] = (byte) (v >>> 8); b[at + 2] = (byte) (v >>> 16); b[at + 3] = (byte) (v >>> 24);
    }

    static void putLong(byte[] b, int at, long v) {
        for (int i = 0; i < 8; i++) b[at + i] = (byte) (v >>> (8 * i));
    }

    static int getInt(byte[] b, int at) {
        return (b[at] & 0xFF) | (b[at + 1] & 0xFF) << 8 | (b[at + 2] & 0xFF) << 16 | (b[at + 3] & 0xFF) << 24;
    }

    static long getLong(byte[] b, int at) {
        long v = 0;
        for (int i = 7; i >= 0; i--) v = (v << 8) | (b[at + i] & 0xFF);
        return v;
    }

    // ------------------------------------------------------------------ prepared

    /** A SQL statement bound to this client, run with different parameters. */
    public static final class Prepared {
        private final KestroClient db;
        private final String text;

        Prepared(KestroClient db, String text) {
            if (text == null) throw new InputException("INPUT sql text must be a string");
            this.db = db;
            this.text = text;
        }

        public String sql() { return text; }

        public List<Map<String, Object>> all(Object... params) { return db.query(text, params); }

        public List<Map<String, Object>> all(Map<String, ?> params) { return db.query(text, params); }

        public Map<String, Object> one(Object... params) { return db.queryOne(text, params); }

        public Map<String, Object> one(Map<String, ?> params) { return db.queryOne(text, params); }

        public Object value(Object... params) { return db.queryValue(text, params); }

        public Object value(Map<String, ?> params) { return db.queryValue(text, params); }

        public SqlResult run(Object... params) { return db.sql(text, params); }

        public SqlResult run(Map<String, ?> params) { return db.sql(text, params); }
    }

}

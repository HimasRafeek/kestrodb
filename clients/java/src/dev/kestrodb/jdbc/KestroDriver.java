package dev.kestrodb.jdbc;

import dev.kestrodb.KestroClient;
import dev.kestrodb.KestroException;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.sql.Connection;
import java.sql.Driver;
import java.sql.DriverManager;
import java.sql.DriverPropertyInfo;
import java.sql.SQLException;
import java.sql.SQLFeatureNotSupportedException;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import java.util.logging.Logger;

/**
 * JDBC driver for kestro-server (a minimal subset of JDBC 4.2 over {@link KestroClient}).
 *
 * <p>URLs: {@code jdbc:kestrodb://host:port}, {@code jdbc:kestrodb:tcp://host:port},
 * {@code jdbc:kestrodb:tls://host:port}, or several comma-separated members of a cluster:
 * {@code jdbc:kestrodb://h1:7820,h2:7820,h3:7820}. Settings come from the URL's query
 * ({@code ?token=...&caFile=...}) or the Properties: {@code token} (or {@code password}),
 * {@code tls} (true: tls:// for members written without a scheme), {@code caFile},
 * {@code verify}, {@code serverName}, {@code timeout}, {@code connectTimeout},
 * {@code failoverTimeout} (seconds, may be decimal), {@code reconnect}. {@code user} is not
 * needed: the token names the user.
 */
public final class KestroDriver implements Driver {
    public static final String PREFIX = "jdbc:kestrodb:";

    static {
        try {
            DriverManager.registerDriver(new KestroDriver());
        } catch (SQLException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    @Override
    public Connection connect(String url, Properties info) throws SQLException {
        if (!acceptsURL(url)) return null;   // JDBC: not ours
        Properties p = new Properties();
        if (info != null) p.putAll(info);
        String rest = url.substring(PREFIX.length());
        int q = rest.indexOf('?');
        if (q >= 0) {
            for (String pair : rest.substring(q + 1).split("&")) {
                if (pair.isEmpty()) continue;
                int eq = pair.indexOf('=');
                String k = decode(eq < 0 ? pair : pair.substring(0, eq));
                String v = eq < 0 ? "true" : decode(pair.substring(eq + 1));
                p.setProperty(k, v);
            }
            rest = rest.substring(0, q);
        }
        boolean tls = Boolean.parseBoolean(p.getProperty("tls", "false"));
        if (rest.startsWith("//")) rest = rest.substring(2);
        List<String> members = new ArrayList<>();
        for (String part : rest.split(",")) {
            String a = part.trim();
            if (a.isEmpty()) continue;
            if (a.endsWith("/")) a = a.substring(0, a.length() - 1);
            members.add(a.contains("://") ? a : (tls ? "tls://" : "tcp://") + a);
        }
        if (members.isEmpty()) throw new SQLException("no server address in " + url, "08001");
        String token = p.getProperty("token", p.getProperty("password"));
        if (token != null && token.isEmpty()) token = null;
        KestroClient.Options o = new KestroClient.Options();
        if (p.containsKey("timeout")) o.timeout(seconds(p, "timeout"));
        if (p.containsKey("connectTimeout")) o.connectTimeout(seconds(p, "connectTimeout"));
        if (p.containsKey("failoverTimeout")) o.failoverTimeout(seconds(p, "failoverTimeout"));
        if (p.containsKey("reconnect")) o.reconnect(Boolean.parseBoolean(p.getProperty("reconnect")));
        if (p.containsKey("caFile")) o.caFile(p.getProperty("caFile"));
        if (p.containsKey("verify")) o.verify(Boolean.parseBoolean(p.getProperty("verify")));
        if (p.containsKey("serverName")) o.serverName(p.getProperty("serverName"));
        try {
            KestroClient db = KestroClient.connect(members, token, o);
            return JdbcConnection.create(db, url);
        } catch (KestroException e) {
            throw Jdbc.sql(e);
        }
    }

    private static Duration seconds(Properties p, String key) throws SQLException {
        try {
            return Duration.ofMillis(Math.round(Double.parseDouble(p.getProperty(key)) * 1000));
        } catch (NumberFormatException e) {
            throw new SQLException("invalid " + key + ": " + p.getProperty(key), "08001");
        }
    }

    private static String decode(String s) { return URLDecoder.decode(s, StandardCharsets.UTF_8); }

    @Override public boolean acceptsURL(String url) { return url != null && url.startsWith(PREFIX); }

    @Override
    public DriverPropertyInfo[] getPropertyInfo(String url, Properties info) {
        String[][] all = {
            {"token", "access token (also: password)"}, {"tls", "true: tls:// for members without a scheme"},
            {"caFile", "PEM file of the CA certificates to trust (tls)"}, {"verify", "false skips certificate checks (testing only)"},
            {"serverName", "TLS server name to verify"}, {"timeout", "reply timeout, seconds (30)"},
            {"connectTimeout", "connect timeout, seconds"}, {"failoverTimeout", "cluster leader search, seconds (15)"},
            {"reconnect", "reopen idle connections the server closed (true)"},
        };
        DriverPropertyInfo[] out = new DriverPropertyInfo[all.length];
        for (int i = 0; i < all.length; i++) {
            out[i] = new DriverPropertyInfo(all[i][0], info == null ? null : info.getProperty(all[i][0]));
            out[i].description = all[i][1];
        }
        return out;
    }

    @Override public int getMajorVersion() { return 0; }

    @Override public int getMinorVersion() { return 9; }

    /** Not a full JDBC implementation. */
    @Override public boolean jdbcCompliant() { return false; }

    @Override public Logger getParentLogger() throws SQLFeatureNotSupportedException { throw new SQLFeatureNotSupportedException("getParentLogger"); }
}

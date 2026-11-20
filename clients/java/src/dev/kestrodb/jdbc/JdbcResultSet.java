package dev.kestrodb.jdbc;

import dev.kestrodb.Json;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.sql.Date;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLDataException;
import java.sql.SQLException;
import java.sql.SQLWarning;
import java.sql.Statement;
import java.sql.Time;
import java.sql.Timestamp;
import java.sql.Types;
import java.time.Instant;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.LocalTime;
import java.time.OffsetDateTime;
import java.time.format.DateTimeParseException;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * An in-memory, scrollable, read-only java.sql.ResultSet over the rows of one result. Columns
 * are the keys of the rows in order of first appearance (rows are JSON documents, so one may
 * hold a column another lacks: it reads as NULL). Getters convert like other drivers: numbers
 * and numeric text to numbers, ISO text to dates and times, JSON arrays and objects to their
 * JSON text for getString.
 */
final class JdbcResultSet {
    private final Statement statement;
    private final List<Map<String, Object>> rows;
    private final List<String> columns;
    private int pos;          // 0: before the first row; rows.size() + 1: after the last
    private boolean wasNull;
    private boolean closed;
    private int fetchSize;

    private JdbcResultSet(Statement statement, List<Map<String, Object>> rows, List<String> columns) {
        this.statement = statement;
        this.rows = rows;
        this.columns = columns;
    }

    static ResultSet create(Statement statement, List<Map<String, Object>> rows) {
        Set<String> names = new LinkedHashSet<>();
        for (Map<String, Object> r : rows) names.addAll(r.keySet());
        return create(statement, rows, new ArrayList<>(names));
    }

    static ResultSet create(Statement statement, List<Map<String, Object>> rows, List<String> columns) {
        return Jdbc.wrap(ResultSet.class, new JdbcResultSet(statement, rows, columns));
    }

    List<String> columns() { return columns; }

    List<Map<String, Object>> rows() { return rows; }

    private void check() throws SQLException {
        if (closed) throw new SQLException("CLOSED the result set was closed", "HY010");
    }

    // ---------------------------------------------------------------- cursor

    public boolean next() throws SQLException {
        check();
        if (pos <= rows.size()) pos++;
        return pos <= rows.size();
    }

    public boolean previous() throws SQLException {
        check();
        if (pos > 0) pos--;
        return pos >= 1;
    }

    public boolean first() throws SQLException { check(); pos = 1; if (rows.isEmpty()) pos = 0; return !rows.isEmpty(); }

    public boolean last() throws SQLException { check(); pos = rows.size(); return !rows.isEmpty(); }

    public void beforeFirst() throws SQLException { check(); pos = 0; }

    public void afterLast() throws SQLException { check(); pos = rows.size() + 1; }

    public boolean absolute(int row) throws SQLException {
        check();
        int target = row >= 0 ? row : rows.size() + 1 + row;
        pos = Math.max(0, Math.min(rows.size() + 1, target));
        return pos >= 1 && pos <= rows.size();
    }

    public boolean relative(int delta) throws SQLException {
        check();
        return absolute(Math.max(0, Math.min(rows.size() + 1, pos + delta)));
    }

    public boolean isBeforeFirst() throws SQLException { check(); return pos == 0 && !rows.isEmpty(); }

    public boolean isAfterLast() throws SQLException { check(); return pos > rows.size() && !rows.isEmpty(); }

    public boolean isFirst() throws SQLException { check(); return pos == 1 && !rows.isEmpty(); }

    public boolean isLast() throws SQLException { check(); return pos == rows.size() && !rows.isEmpty(); }

    public int getRow() throws SQLException { check(); return pos >= 1 && pos <= rows.size() ? pos : 0; }

    public void close() { closed = true; }

    public boolean isClosed() { return closed; }

    public Statement getStatement() throws SQLException { check(); return statement; }

    public ResultSetMetaData getMetaData() throws SQLException {
        check();
        return Jdbc.wrap(ResultSetMetaData.class, new JdbcResultSetMetaData(columns, rows));
    }

    public int findColumn(String label) throws SQLException {
        check();
        int i = columns.indexOf(label);
        if (i < 0) for (int k = 0; k < columns.size(); k++) if (columns.get(k).equalsIgnoreCase(label)) { i = k; break; }
        if (i < 0) throw new SQLException("no column " + label, "42S22");
        return i + 1;
    }

    public int getType() throws SQLException { check(); return ResultSet.TYPE_SCROLL_INSENSITIVE; }

    public int getConcurrency() throws SQLException { check(); return ResultSet.CONCUR_READ_ONLY; }

    public int getHoldability() throws SQLException { check(); return ResultSet.HOLD_CURSORS_OVER_COMMIT; }

    public void setFetchSize(int rows) throws SQLException { check(); fetchSize = rows; }

    public int getFetchSize() throws SQLException { check(); return fetchSize; }

    public void setFetchDirection(int direction) throws SQLException { check(); }

    public int getFetchDirection() throws SQLException { check(); return ResultSet.FETCH_FORWARD; }

    public SQLWarning getWarnings() throws SQLException { check(); return null; }

    public void clearWarnings() throws SQLException { check(); }

    public boolean wasNull() throws SQLException { check(); return wasNull; }

    // ---------------------------------------------------------------- values

    private Object value(int column) throws SQLException {
        check();
        if (pos < 1 || pos > rows.size()) throw new SQLException("no current row (call next())", "24000");
        if (column < 1 || column > columns.size()) throw new SQLException("column index " + column + " out of range 1.." + columns.size(), "07009");
        Object v = rows.get(pos - 1).get(columns.get(column - 1));
        wasNull = v == null;
        return v;
    }

    private static SQLDataException cannot(Object v, String type) {
        return new SQLDataException("cannot read " + Json.stringify(v) + " as " + type, "22018");
    }

    public Object getObject(int c) throws SQLException { return value(c); }

    public Object getObject(String c) throws SQLException { return getObject(findColumn(c)); }

    public Object getObject(int c, Map<String, Class<?>> map) throws SQLException { return getObject(c); }

    public Object getObject(String c, Map<String, Class<?>> map) throws SQLException { return getObject(findColumn(c)); }

    public String getString(int c) throws SQLException {
        Object v = value(c);
        if (v == null) return null;
        if (v instanceof String) return (String) v;
        if (v instanceof Double) {
            double d = (Double) v;
            if (d == Math.rint(d) && Math.abs(d) < 1e15) return Long.toString((long) d);
            return Double.toString(d);
        }
        if (v instanceof Map || v instanceof List) return Json.stringify(v);
        return v.toString();
    }

    public String getString(String c) throws SQLException { return getString(findColumn(c)); }

    public String getNString(int c) throws SQLException { return getString(c); }

    public String getNString(String c) throws SQLException { return getString(findColumn(c)); }

    public boolean getBoolean(int c) throws SQLException {
        Object v = value(c);
        if (v == null) return false;
        if (v instanceof Boolean) return (Boolean) v;
        if (v instanceof Number) return ((Number) v).doubleValue() != 0;
        if (v instanceof String) {
            String s = ((String) v).trim();
            if (s.equalsIgnoreCase("true") || s.equals("1")) return true;
            if (s.equalsIgnoreCase("false") || s.equals("0")) return false;
        }
        throw cannot(v, "boolean");
    }

    public boolean getBoolean(String c) throws SQLException { return getBoolean(findColumn(c)); }

    private BigDecimal decimal(Object v, String type) throws SQLException {
        if (v instanceof Long) return BigDecimal.valueOf((Long) v);
        if (v instanceof BigInteger) return new BigDecimal((BigInteger) v);
        if (v instanceof Double) return BigDecimal.valueOf((Double) v);
        if (v instanceof Boolean) return (Boolean) v ? BigDecimal.ONE : BigDecimal.ZERO;
        if (v instanceof String) {
            try { return new BigDecimal(((String) v).trim()); } catch (NumberFormatException e) { throw cannot(v, type); }
        }
        throw cannot(v, type);
    }

    public long getLong(int c) throws SQLException {
        Object v = value(c);
        if (v == null) return 0;
        if (v instanceof Long) return (Long) v;
        BigDecimal d = decimal(v, "long");
        try { return d.setScale(0, java.math.RoundingMode.DOWN).longValueExact(); } catch (ArithmeticException e) { throw cannot(v, "long"); }
    }

    public long getLong(String c) throws SQLException { return getLong(findColumn(c)); }

    public int getInt(int c) throws SQLException {
        long l = getLong(c);
        if (l < Integer.MIN_VALUE || l > Integer.MAX_VALUE) throw new SQLDataException(l + " is out of the int range", "22003");
        return (int) l;
    }

    public int getInt(String c) throws SQLException { return getInt(findColumn(c)); }

    public short getShort(int c) throws SQLException {
        long l = getLong(c);
        if (l < Short.MIN_VALUE || l > Short.MAX_VALUE) throw new SQLDataException(l + " is out of the short range", "22003");
        return (short) l;
    }

    public short getShort(String c) throws SQLException { return getShort(findColumn(c)); }

    public byte getByte(int c) throws SQLException {
        long l = getLong(c);
        if (l < Byte.MIN_VALUE || l > Byte.MAX_VALUE) throw new SQLDataException(l + " is out of the byte range", "22003");
        return (byte) l;
    }

    public byte getByte(String c) throws SQLException { return getByte(findColumn(c)); }

    public double getDouble(int c) throws SQLException {
        Object v = value(c);
        if (v == null) return 0;
        if (v instanceof Number) return ((Number) v).doubleValue();
        return decimal(v, "double").doubleValue();
    }

    public double getDouble(String c) throws SQLException { return getDouble(findColumn(c)); }

    public float getFloat(int c) throws SQLException { return (float) getDouble(c); }

    public float getFloat(String c) throws SQLException { return getFloat(findColumn(c)); }

    public BigDecimal getBigDecimal(int c) throws SQLException {
        Object v = value(c);
        return v == null ? null : decimal(v, "BigDecimal");
    }

    public BigDecimal getBigDecimal(String c) throws SQLException { return getBigDecimal(findColumn(c)); }

    public BigDecimal getBigDecimal(int c, int scale) throws SQLException {
        BigDecimal d = getBigDecimal(c);
        return d == null ? null : d.setScale(scale, java.math.RoundingMode.HALF_UP);
    }

    public BigDecimal getBigDecimal(String c, int scale) throws SQLException { return getBigDecimal(findColumn(c), scale); }

    // ISO text: 2026-11-20, 10:15:30, 2026-11-20T10:15:30[.5][Z|+05:30], or with a space for T.
    private static LocalDateTime dateTime(String s) {
        String t = s.trim().replace(' ', 'T');
        try { return LocalDateTime.parse(t); } catch (DateTimeParseException ignored) { }
        try { return OffsetDateTime.parse(t).atZoneSameInstant(java.time.ZoneId.systemDefault()).toLocalDateTime(); } catch (DateTimeParseException ignored) { }
        return LocalDate.parse(t).atStartOfDay();
    }

    public Timestamp getTimestamp(int c) throws SQLException {
        Object v = value(c);
        if (v == null) return null;
        if (v instanceof Long) return new Timestamp((Long) v);
        if (v instanceof String) {
            try { return Timestamp.valueOf(dateTime((String) v)); } catch (DateTimeParseException e) { throw cannot(v, "Timestamp"); }
        }
        throw cannot(v, "Timestamp");
    }

    public Timestamp getTimestamp(String c) throws SQLException { return getTimestamp(findColumn(c)); }

    public Timestamp getTimestamp(int c, Calendar cal) throws SQLException { return getTimestamp(c); }

    public Timestamp getTimestamp(String c, Calendar cal) throws SQLException { return getTimestamp(findColumn(c)); }

    public Date getDate(int c) throws SQLException {
        Object v = value(c);
        if (v == null) return null;
        if (v instanceof Long) return new Date((Long) v);
        if (v instanceof String) {
            try { return Date.valueOf(dateTime((String) v).toLocalDate()); } catch (DateTimeParseException e) { throw cannot(v, "Date"); }
        }
        throw cannot(v, "Date");
    }

    public Date getDate(String c) throws SQLException { return getDate(findColumn(c)); }

    public Date getDate(int c, Calendar cal) throws SQLException { return getDate(c); }

    public Date getDate(String c, Calendar cal) throws SQLException { return getDate(findColumn(c)); }

    public Time getTime(int c) throws SQLException {
        Object v = value(c);
        if (v == null) return null;
        if (v instanceof String) {
            String s = ((String) v).trim();
            try {
                return Time.valueOf(s.length() > 8 && (s.indexOf('T') > 0 || s.indexOf(' ') > 0) ? dateTime(s).toLocalTime() : LocalTime.parse(s));
            } catch (DateTimeParseException e) {
                throw cannot(v, "Time");
            }
        }
        throw cannot(v, "Time");
    }

    public Time getTime(String c) throws SQLException { return getTime(findColumn(c)); }

    public Time getTime(int c, Calendar cal) throws SQLException { return getTime(c); }

    public Time getTime(String c, Calendar cal) throws SQLException { return getTime(findColumn(c)); }

    @SuppressWarnings("unchecked")
    public <T> T getObject(int c, Class<T> type) throws SQLException {
        if (type == null) throw new SQLException("type is null");
        Object v = value(c);
        if (v == null) return null;
        if (type.isInstance(v)) return (T) v;
        if (type == String.class) return (T) getString(c);
        if (type == Long.class || type == long.class) return (T) Long.valueOf(getLong(c));
        if (type == Integer.class || type == int.class) return (T) Integer.valueOf(getInt(c));
        if (type == Short.class || type == short.class) return (T) Short.valueOf(getShort(c));
        if (type == Byte.class || type == byte.class) return (T) Byte.valueOf(getByte(c));
        if (type == Double.class || type == double.class) return (T) Double.valueOf(getDouble(c));
        if (type == Float.class || type == float.class) return (T) Float.valueOf(getFloat(c));
        if (type == Boolean.class || type == boolean.class) return (T) Boolean.valueOf(getBoolean(c));
        if (type == BigDecimal.class) return (T) getBigDecimal(c);
        if (type == BigInteger.class) return (T) decimal(v, "BigInteger").toBigInteger();
        if (type == Timestamp.class) return (T) getTimestamp(c);
        if (type == Date.class) return (T) getDate(c);
        if (type == Time.class) return (T) getTime(c);
        try {
            if (type == LocalDateTime.class) return (T) dateTime(v.toString());
            if (type == LocalDate.class) return (T) dateTime(v.toString()).toLocalDate();
            if (type == LocalTime.class) return (T) getTime(c).toLocalTime();
            if (type == OffsetDateTime.class) return (T) OffsetDateTime.parse(v.toString().trim().replace(' ', 'T'));
            if (type == Instant.class) return (T) (v instanceof Long ? Instant.ofEpochMilli((Long) v) : OffsetDateTime.parse(v.toString().trim().replace(' ', 'T')).toInstant());
        } catch (DateTimeParseException e) {
            throw cannot(v, type.getSimpleName());
        }
        throw cannot(v, type.getName());
    }

    public <T> T getObject(String c, Class<T> type) throws SQLException { return getObject(findColumn(c), type); }

    /** JDBC type of a column, from its first non-null value. */
    static int sqlType(Object v) {
        if (v instanceof Long || v instanceof BigInteger) return Types.BIGINT;
        if (v instanceof Double) return Types.DOUBLE;
        if (v instanceof Boolean) return Types.BOOLEAN;
        if (v instanceof Map || v instanceof List) return Types.OTHER;
        return Types.VARCHAR;
    }
}

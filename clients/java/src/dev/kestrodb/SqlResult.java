package dev.kestrodb;

import java.util.Collections;
import java.util.List;
import java.util.Map;

/**
 * The result of one SQL statement: {@link #rows()} for a statement that returns rows (SELECT,
 * SHOW, EXPLAIN, ... and writes with RETURNING), or {@link #changes()} and
 * {@link #lastInsertId()} for a write.
 */
public final class SqlResult {
    private final Map<String, Object> raw;
    private final List<Map<String, Object>> rows;

    @SuppressWarnings("unchecked")
    SqlResult(Object parsed) {
        if (!(parsed instanceof Map)) throw new ConnectionException("IO the server's SQL result is not a JSON object");
        this.raw = Collections.unmodifiableMap((Map<String, Object>) parsed);
        Object r = raw.get("rows");
        this.rows = r instanceof List ? Collections.unmodifiableList((List<Map<String, Object>>) r) : null;
    }

    /** True when the statement returned rows (even zero of them). */
    public boolean hasRows() { return rows != null; }

    /**
     * The rows, each a map from column name to value in SELECT order (String, Long, Double,
     * Boolean, null, or List/Map for JSON arrays and objects); null for a statement without rows.
     */
    public List<Map<String, Object>> rows() { return rows; }

    /** Rows changed by a write (0 when the result has none). */
    public long changes() {
        Object c = raw.get("changes");
        return c instanceof Number ? ((Number) c).longValue() : 0;
    }

    /** The id of the row an INSERT created (Long, or String for a text id), or null. */
    public Object lastInsertId() { return raw.get("lastInsertId"); }

    /** The whole result as the server sent it (other fields included, such as {@code columns}). */
    public Map<String, Object> raw() { return raw; }

    @Override public String toString() { return Json.stringify(raw); }
}

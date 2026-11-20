package dev.kestrodb.jdbc;

import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.Types;
import java.util.List;
import java.util.Map;

/** Column names in result order; types inferred from each column's first non-null value. */
final class JdbcResultSetMetaData {
    private final List<String> columns;
    private final int[] types;

    JdbcResultSetMetaData(List<String> columns, List<Map<String, Object>> rows) {
        this.columns = columns;
        this.types = new int[columns.size()];
        for (int i = 0; i < types.length; i++) {
            types[i] = Types.VARCHAR;
            for (Map<String, Object> r : rows) {
                Object v = r.get(columns.get(i));
                if (v != null) { types[i] = JdbcResultSet.sqlType(v); break; }
            }
        }
    }

    private int at(int column) throws SQLException {
        if (column < 1 || column > columns.size()) throw new SQLException("column index " + column + " out of range 1.." + columns.size(), "07009");
        return column - 1;
    }

    public int getColumnCount() { return columns.size(); }

    public String getColumnName(int c) throws SQLException { return columns.get(at(c)); }

    public String getColumnLabel(int c) throws SQLException { return columns.get(at(c)); }

    public int getColumnType(int c) throws SQLException { return types[at(c)]; }

    public String getColumnTypeName(int c) throws SQLException {
        switch (types[at(c)]) {
            case Types.BIGINT: return "INTEGER";
            case Types.DOUBLE: return "REAL";
            case Types.BOOLEAN: return "BOOLEAN";
            case Types.OTHER: return "JSON";
            default: return "TEXT";
        }
    }

    public String getColumnClassName(int c) throws SQLException {
        switch (types[at(c)]) {
            case Types.BIGINT: return Long.class.getName();
            case Types.DOUBLE: return Double.class.getName();
            case Types.BOOLEAN: return Boolean.class.getName();
            case Types.OTHER: return Object.class.getName();
            default: return String.class.getName();
        }
    }

    public int isNullable(int c) throws SQLException { at(c); return ResultSetMetaData.columnNullableUnknown; }

    public boolean isAutoIncrement(int c) throws SQLException { return columns.get(at(c)).equals("id"); }

    public boolean isCaseSensitive(int c) throws SQLException { return types[at(c)] == Types.VARCHAR; }

    public boolean isSearchable(int c) throws SQLException { at(c); return true; }

    public boolean isCurrency(int c) throws SQLException { at(c); return false; }

    public boolean isSigned(int c) throws SQLException { int t = types[at(c)]; return t == Types.BIGINT || t == Types.DOUBLE; }

    public int getColumnDisplaySize(int c) throws SQLException { at(c); return 0; }

    public int getPrecision(int c) throws SQLException { at(c); return 0; }

    public int getScale(int c) throws SQLException { at(c); return 0; }

    public String getTableName(int c) throws SQLException { at(c); return ""; }

    public String getSchemaName(int c) throws SQLException { at(c); return ""; }

    public String getCatalogName(int c) throws SQLException { at(c); return ""; }

    public boolean isReadOnly(int c) throws SQLException { at(c); return true; }

    public boolean isWritable(int c) throws SQLException { at(c); return false; }

    public boolean isDefinitelyWritable(int c) throws SQLException { at(c); return false; }
}

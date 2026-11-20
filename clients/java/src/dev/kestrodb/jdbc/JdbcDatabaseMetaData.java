package dev.kestrodb.jdbc;

import dev.kestrodb.KestroClient;
import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.ResultSet;
import java.sql.RowIdLifetime;

/** The DatabaseMetaData calls tools ask first (names, versions, capabilities). */
final class JdbcDatabaseMetaData {
    private final JdbcConnection conn;

    JdbcDatabaseMetaData(JdbcConnection conn) { this.conn = conn; }

    public Connection getConnection() { return conn.self; }

    public String getURL() { return conn.url; }

    public String getUserName() { Object u = conn.db.stats().get("user"); return u == null ? null : u.toString(); }

    public String getDatabaseProductName() { return "KestroDB"; }

    public String getDatabaseProductVersion() { return KestroClient.VERSION; }

    public int getDatabaseMajorVersion() { return 0; }

    public int getDatabaseMinorVersion() { return 9; }

    public String getDriverName() { return "KestroDB JDBC (dev.kestrodb)"; }

    public String getDriverVersion() { return KestroClient.VERSION; }

    public int getDriverMajorVersion() { return 0; }

    public int getDriverMinorVersion() { return 9; }

    public int getJDBCMajorVersion() { return 4; }

    public int getJDBCMinorVersion() { return 2; }

    public boolean isReadOnly() { return false; }

    public String getIdentifierQuoteString() { return "\""; }

    public String getSearchStringEscape() { return "\\"; }

    public String getCatalogSeparator() { return "."; }

    public String getCatalogTerm() { return "catalog"; }

    public String getSchemaTerm() { return "schema"; }

    public String getProcedureTerm() { return "procedure"; }

    public String getExtraNameCharacters() { return ""; }

    public boolean supportsTransactions() { return true; }

    public boolean supportsSavepoints() { return false; }

    public boolean supportsGetGeneratedKeys() { return true; }

    public boolean supportsBatchUpdates() { return true; }

    public boolean supportsMultipleResultSets() { return false; }

    public boolean supportsNamedParameters() { return false; }

    public boolean supportsStoredProcedures() { return false; }

    public boolean supportsResultSetType(int type) { return type != ResultSet.TYPE_SCROLL_SENSITIVE; }

    public boolean supportsResultSetConcurrency(int type, int concurrency) { return type != ResultSet.TYPE_SCROLL_SENSITIVE && concurrency == ResultSet.CONCUR_READ_ONLY; }

    public boolean supportsResultSetHoldability(int holdability) { return holdability == ResultSet.HOLD_CURSORS_OVER_COMMIT; }

    public int getResultSetHoldability() { return ResultSet.HOLD_CURSORS_OVER_COMMIT; }

    public int getDefaultTransactionIsolation() { return Connection.TRANSACTION_SERIALIZABLE; }

    public boolean supportsTransactionIsolationLevel(int level) { return level == Connection.TRANSACTION_SERIALIZABLE; }

    public boolean supportsDataDefinitionAndDataManipulationTransactions() { return false; }

    public boolean supportsDataManipulationTransactionsOnly() { return true; }

    public boolean dataDefinitionCausesTransactionCommit() { return false; }

    public boolean dataDefinitionIgnoredInTransactions() { return false; }

    public boolean supportsMixedCaseIdentifiers() { return true; }

    public boolean storesMixedCaseIdentifiers() { return true; }

    public boolean supportsAlterTableWithAddColumn() { return true; }

    public boolean supportsAlterTableWithDropColumn() { return true; }

    public boolean supportsColumnAliasing() { return true; }

    public boolean nullPlusNonNullIsNull() { return true; }

    public boolean supportsGroupBy() { return true; }

    public boolean supportsOrderByUnrelated() { return true; }

    public boolean supportsLikeEscapeClause() { return true; }

    public boolean supportsOuterJoins() { return true; }

    public boolean supportsUnion() { return true; }

    public boolean supportsUnionAll() { return true; }

    public boolean supportsSubqueriesInExists() { return true; }

    public boolean supportsSubqueriesInIns() { return true; }

    public boolean supportsCatalogsInDataManipulation() { return false; }

    public boolean supportsSchemasInDataManipulation() { return false; }

    public boolean supportsStatementPooling() { return false; }

    public boolean locatorsUpdateCopy() { return false; }

    public boolean generatedKeyAlwaysReturned() { return false; }

    public boolean autoCommitFailureClosesAllResultSets() { return false; }

    public boolean supportsStoredFunctionsUsingCallSyntax() { return false; }

    public RowIdLifetime getRowIdLifetime() { return RowIdLifetime.ROWID_UNSUPPORTED; }

    public int getMaxConnections() { return 0; }

    public int getMaxStatementLength() { return 0; }

    public int getSQLStateType() { return DatabaseMetaData.sqlStateSQL; }
}

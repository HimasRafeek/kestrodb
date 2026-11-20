# SQL in KestroDB

KestroDB speaks the common core of MySQL, PostgreSQL and SQLite. The same statements work
from every client:

```js
db.sql('SELECT * FROM users WHERE age >= ? ORDER BY age DESC LIMIT 10', [18])   // Node (Database, RemoteDB)
```
```php
$db->sql('SELECT * FROM users WHERE email = ?', [$email]);                       // PHP
```
```python
db.sql("UPDATE users SET age = age + 1 WHERE id = ?", [5])                       # Python
```

Always pass values as parameters, never by pasting them into the SQL text. Parameters
can be numbers, strings, booleans, null, or JSON objects and arrays. Placeholders:

| Placeholder | Parameters | Example |
|---|---|---|
| `?` | a list, in order | `WHERE city = ? AND age > ?` with `['Kandy', 30]` |
| `$1`, `?1` | a list, by number (a number may repeat) | `WHERE city = $1 OR home = $1` with `['Kandy']` |
| `:name`, `@name` | a map (object, associative array, dict); keys with or without `:` | `WHERE city = :city` with `{city: 'Kandy'}` |

Named and unnamed placeholders cannot be mixed in one statement; a missing named value is
an `INPUT` error. The client libraries add `query`, `execute`, transactions and prepared
statements on top of this; see [CLIENTS.md](CLIENTS.md).

Results are JSON:

| Statement | Result |
|---|---|
| `SELECT` | `{"rows": [{...}, ...]}`: one object per row |
| `INSERT` | `{"changes": n, "lastInsertId": id}` |
| `UPDATE`, `DELETE` | `{"changes": n}` |
| `CREATE`, `DROP` | `{"changes": 0}` (`DROP TABLE`: rows removed) |

Errors start with a code: `INPUT` (bad SQL or values), `CONFLICT` (duplicate id or unique
value), `LIMIT`, `UNSUPPORTED`, `CORRUPT`.

## Tables and rows

```sql
CREATE TABLE users (
  id INTEGER PRIMARY KEY,
  email TEXT UNIQUE NOT NULL,
  name VARCHAR(100),
  age INT
);
```

- Every table has an **`id`** primary key. Leave it out of `INSERT` and KestroDB assigns
  1, 2, 3, ... (like `AUTO_INCREMENT` / `SERIAL`); `lastInsertId` returns it. You may also
  give your own integer or text id.
- **Declared columns are enforced.** `INSERT` and `UPDATE` (including upserts) check each
  declared column's type and `NOT NULL`, and fill `DEFAULT` values for omitted columns:

  | Declared type | Accepts |
  |---|---|
  | `INT`, `INTEGER`, `BIGINT`, `SMALLINT`, `SERIAL`, ... | whole numbers |
  | `REAL`, `FLOAT`, `DOUBLE`, `DECIMAL`, `NUMERIC` | numbers |
  | `TEXT`, `VARCHAR(n)`, `CHAR(n)`, `UUID`, `DATE`, `TIMESTAMP`, ... | text (dates as ISO text); `VARCHAR(n)` and `CHAR(n)` hold at most n characters, and longer text fails with `INPUT` (`CHAR` is not padded) |
  | `BOOLEAN`, `BOOL` | `TRUE` / `FALSE` |
  | `JSON`, `JSONB`, no type, anything else | any JSON value |

  NULL is accepted unless the column is `NOT NULL`. Values are not converted: `'5'` is not
  an integer. A `DEFAULT` must be a constant.
- **Rows are JSON documents,** so a row may also hold undeclared columns, nested objects
  (`address.city`) and arrays. Tables created without column definitions accept anything.
- **`UNIQUE`** columns get a unique index: duplicate values fail with `CONFLICT` (NULL and
  missing values may repeat, as in SQL).
- Dotted column names reach inside objects: `INSERT INTO t (id, address.city) ...`,
  `SELECT address.city FROM t`, `CREATE INDEX ON t (address.city)`.

## Constraints

`CHECK`, `FOREIGN KEY` and `UNIQUE` over several columns are enforced, written on a column
or as table constraints, named or not:

```sql
CREATE TABLE books (
  id INTEGER PRIMARY KEY,
  author INTEGER REFERENCES authors (id) ON DELETE CASCADE,
  title TEXT NOT NULL,
  price REAL CHECK (price > 0),
  CONSTRAINT one_title UNIQUE (author, title)
);
CREATE TABLE reviews (
  id INTEGER PRIMARY KEY,
  book INTEGER,
  stars INTEGER CHECK (stars BETWEEN 1 AND 5),
  FOREIGN KEY (book) REFERENCES books ON DELETE SET NULL ON UPDATE CASCADE
);
ALTER TABLE orders ADD CONSTRAINT orders_user FOREIGN KEY (user_id) REFERENCES users (id);
ALTER TABLE users ADD CONSTRAINT adult CHECK (age >= 18);
ALTER TABLE users DROP CONSTRAINT adult;      -- also DROP FOREIGN KEY name, DROP CHECK name
```

- They are checked at the end of every `INSERT`, `UPDATE` and `DELETE`, against the rows as
  the statement leaves them (with the transaction's earlier changes), so a statement that
  swaps two values passes. A violation fails the statement with `CONFLICT` and changes
  nothing.
- **`CHECK (expression)`**: an expression of the row's own columns (no subqueries,
  aggregates or parameters). A row fails when it is false; NULL passes, as in SQL.
- **Foreign keys**: the referenced columns must be the `id` (the default when none are
  named), a `UNIQUE` column, or columns with a `UNIQUE` constraint. A row whose foreign key
  columns are all set must have its parent; any NULL skips the check. When a parent is
  deleted or its referenced columns change, the children follow the action: `NO ACTION` /
  `RESTRICT` (the default: the statement fails), `CASCADE` (children are deleted, or take
  the new values), `SET NULL`, `SET DEFAULT`. Actions chain through further foreign keys,
  including a table that references itself. `DEFERRABLE` and `MATCH` are accepted; checks
  are never deferred to `COMMIT`.
- **`UNIQUE (a, b)`**: no two rows with the same values in all of the columns; rows with a
  NULL in any of them may repeat.
- KestroDB indexes the columns of every foreign key and of every multi-column `UNIQUE` (one
  index over all of them, unless an index already starts with those columns), so a parent's
  delete finds its children quickly. A foreign key may reference the columns of a composite
  `UNIQUE` index.
- `ALTER TABLE ... ADD CONSTRAINT` (and `ADD COLUMN` with `CHECK` or `REFERENCES`) checks the
  existing rows first. A table that another table's foreign key references cannot be
  dropped; a column a constraint uses cannot be dropped (drop the constraint first);
  `RENAME COLUMN` and `RENAME TO` carry constraints along.
- Constraints apply to SQL statements; key/value writes to a table's rows bypass them.

## Statements

```sql
-- Insert one or many rows
INSERT INTO users (email, name, age) VALUES (?, ?, ?), (?, ?, ?);

-- Upsert (any of these)
INSERT INTO users (id, email, name) VALUES (?, ?, ?) ON CONFLICT (id) DO UPDATE SET name = excluded.name;
INSERT INTO users (id, email, name) VALUES (?, ?, ?) ON DUPLICATE KEY UPDATE name = VALUES(name);
INSERT INTO users (id, email) VALUES (?, ?) ON CONFLICT DO NOTHING;   -- or INSERT IGNORE
REPLACE INTO users (id, email, name) VALUES (?, ?, ?);                  -- or INSERT OR REPLACE

-- Query
SELECT * FROM users WHERE id = ?;
SELECT id, name, age * 2 AS double_age FROM users
  WHERE (city = 'Colombo' OR city = 'Kandy') AND age BETWEEN 18 AND 65 AND name LIKE 'Him%'
  ORDER BY age DESC, name LIMIT 20 OFFSET 40;
SELECT city, COUNT(*) AS n, AVG(age) AS avg_age FROM users GROUP BY city HAVING COUNT(*) > 10 ORDER BY n DESC;
SELECT DISTINCT city FROM users;
SELECT id, CASE WHEN age < 18 THEN 'minor' WHEN age < 65 THEN 'adult' ELSE 'senior' END AS band FROM users;
SELECT city, SUM(CASE status WHEN 'paid' THEN total ELSE 0 END) AS paid FROM orders GROUP BY city;

-- Window functions
SELECT name, city, RANK() OVER (PARTITION BY city ORDER BY age DESC) AS rank_in_city FROM users;
SELECT id, total, SUM(total) OVER (ORDER BY id) AS running_total FROM orders;
SELECT id, LAG(total) OVER (ORDER BY id) AS previous FROM orders;

-- UNION / INTERSECT / EXCEPT
SELECT email FROM users UNION SELECT email FROM invites ORDER BY email;

-- Change and remove
UPDATE users SET age = age + 1, name = ? WHERE id = ?;
DELETE FROM users WHERE last_seen < ?;

-- Joins (INNER, LEFT, RIGHT, FULL, CROSS, comma) with aliases
SELECT u.name, o.total FROM users u JOIN orders o ON o.user_id = u.id WHERE u.city = ?;
SELECT d.title, u.name FROM users u RIGHT JOIN depts d USING (dept_id);
SELECT u.name, COUNT(o.id) AS orders FROM users u LEFT JOIN orders o ON o.user_id = u.id GROUP BY u.name;
SELECT o.* FROM orders o JOIN users u ON u.id = o.user_id WHERE u.email = ?;

-- Subqueries, also correlated (naming the outer table)
SELECT * FROM users WHERE id IN (SELECT user_id FROM orders WHERE total > ?);
SELECT * FROM orders WHERE total = (SELECT MAX(total) FROM orders);
SELECT u.name FROM users u WHERE NOT EXISTS (SELECT 1 FROM orders o WHERE o.user_id = u.id);
SELECT u.name, (SELECT COUNT(*) FROM orders o WHERE o.user_id = u.id) AS orders FROM users u;

-- Transactions
BEGIN;
UPDATE accounts SET balance = balance - ? WHERE id = ?;
UPDATE accounts SET balance = balance + ? WHERE id = ?;
COMMIT;            -- or ROLLBACK
-- Savepoints: undo part of a transaction
SAVEPOINT before_items;
ROLLBACK TO SAVEPOINT before_items;   -- or RELEASE SAVEPOINT before_items

-- Indexes
CREATE INDEX ON users (city);
CREATE UNIQUE INDEX users_email ON users (email);
CREATE INDEX ON events (user_id, day);                      -- composite: up to 8 columns
CREATE UNIQUE INDEX seat ON tickets (show_id, row, number);
DROP INDEX users_email ON users;

-- Change a table
ALTER TABLE users ADD COLUMN plan TEXT NOT NULL DEFAULT 'free';   -- existing rows get 'free'
ALTER TABLE users RENAME COLUMN plan TO tier;                     -- indexes follow
ALTER TABLE users DROP COLUMN tier;                               -- removed from every row
ALTER TABLE users RENAME TO members;                              -- rows, indexes, constraints move
ALTER TABLE users ALTER COLUMN plan SET DEFAULT 'free';           -- later INSERTs get 'free'
ALTER TABLE users ALTER COLUMN plan DROP DEFAULT;
ALTER TABLE users ALTER COLUMN email SET NOT NULL;                -- fails if a row has NULL
ALTER TABLE users ALTER COLUMN email DROP NOT NULL;
ALTER TABLE users ALTER COLUMN zip TYPE INTEGER;                  -- also SET DATA TYPE; converts '123' to 123
ALTER TABLE users MODIFY name VARCHAR(50) NOT NULL DEFAULT '';    -- MySQL: the whole new definition
DROP TABLE users;
```

**Expressions:** `= <> != < <= > >=`, `AND OR NOT`, `IN (...)`, `BETWEEN`, `LIKE` /
`ILIKE` (`%`, `_`), `IS [NOT] NULL`, `+ - * / %`, `||` (concatenate), `CASE WHEN ... THEN ... ELSE
... END` and `CASE x WHEN value THEN ... END`, and the functions
`LOWER UPPER LENGTH ABS ROUND COALESCE IFNULL`. Aggregates: `COUNT(*)`, `COUNT(x)`,
`COUNT(DISTINCT x)`, `SUM AVG MIN MAX`. `ORDER BY` accepts columns, expressions, aliases
and positions; NULLs sort first.

**Types follow PostgreSQL's strictness:** a number never equals a string, and comparing with
NULL gives NULL (so `WHERE x = NULL` matches nothing; use `IS NULL`). Division by zero
gives NULL.

## Transactions

`BEGIN` ... `COMMIT` (or `ROLLBACK`) groups statements into one atomic, durable commit.
Transactions belong to a connection: a Node `Database` or `RemoteDB`, a PHP or Python
client, or a C library handle.

- Inside a transaction, every statement sees the transaction's own changes (and
  `SELECT`, aggregates and joins include them). Other connections see nothing until
  `COMMIT`, then all of it at once.
- Unique constraints are checked by each statement, against the table and the
  transaction's earlier changes, so a duplicate fails at the `INSERT`, not at `COMMIT`.
- **One writing transaction at a time** (like SQLite): from `BEGIN` to `COMMIT` a
  transaction holds the write gate, so other connections' writes wait for it and the
  transaction is serializable. Reads never wait. Keep transactions short.
- **`BEGIN CONCURRENT`**: optimistic transactions that run side by side. One holds no gate
  until `COMMIT`, so other writers go on meanwhile. Its statements read the database as it
  was at `BEGIN` (snapshot isolation) plus its own changes. `COMMIT` succeeds when no
  other commit since `BEGIN` changed a row it writes (first committer wins); otherwise it
  fails with `CONFLICT ... run it again` and the transaction is rolled back, so retry it from
  `BEGIN`. Foreign keys and unique constraints are checked again at `COMMIT` against the
  rows as they are then (a parent deleted meanwhile fails it). Automatic ids are reserved
  for it when it inserts: a transaction that rolls back leaves a gap.
- **`BEGIN CONCURRENT` is serializable** (2026-11-20). Each transaction records what it read:
  rows read by id or by a unique value, the index ranges it scanned, or the whole table when
  no key or index narrows a scan (a scan that stops at its `LIMIT` covers only the rows up
  to where it stopped). `COMMIT` also fails with `CONFLICT ... run it again` if a commit
  after its snapshot changed a row it read or added a row to a range it read (a phantom).
  This prevents write skew, such as two doctors each going off call because the other is
  still on call. A transaction that wrote nothing always commits. Checks are by range, not
  by the full `WHERE`: index the columns you filter by to avoid needless conflicts. Past
  about 1 MB of recorded ranges, the tables involved count as read whole (more conflicts,
  never fewer). `BEGIN CONCURRENT ISOLATION LEVEL SNAPSHOT` (or `REPEATABLE READ` /
  `READ COMMITTED`) skips the read checks: write skew is then possible, with fewer retries.
  `ISOLATION LEVEL SERIALIZABLE` is accepted and is the default; a plain `BEGIN` is
  serializable at any level.
- `SAVEPOINT name` marks a point inside the transaction; `ROLLBACK TO [SAVEPOINT] name` undoes
  what came after it (the savepoint stays), `RELEASE [SAVEPOINT] name` forgets it and later
  ones. Outside a transaction, `SAVEPOINT` begins one that releasing that savepoint commits
  (as in SQLite).
- A connection that closes, or a server connection that drops, rolls its open
  transaction back. A failed `COMMIT` (for example a disk error) ends the transaction.
- Inside a transaction, key/value writes on the same connection are refused (they would
  wait for the transaction itself), and `CREATE`/`DROP` must run outside transactions.
- `ALTER TABLE` rewrites the rows it changes in commits of up to 50,000 rows, so on a larger
  table it is not atomic as a whole (rerun an interrupted `RENAME TO`). `ADD COLUMN ... NOT NULL` needs a `DEFAULT` when the
  table has rows.
- `ALTER COLUMN ... SET DEFAULT` changes only later `INSERT`s; existing rows keep their values.
  The default must be a constant of the column's type.
- `SET NOT NULL`, `TYPE` and `MODIFY` check every row first: a row with NULL (for `NOT NULL`)
  or a value that does not convert fails the statement with `CONFLICT`, names the row, and
  changes nothing. `TYPE` and `MODIFY` convert existing values: text holding an integer or a
  number becomes `INTEGER` or `REAL`, numbers and booleans become text, `'true'`/`'t'`/
  `'yes'`/`'on'`/`'1'` and `'false'`/`'f'`/`'no'`/`'off'`/`'0'` become `BOOLEAN`; a number with
  a fraction does not become `INTEGER`, and text longer than a new `VARCHAR(n)` fails. Unique
  indexes, `CHECK` and `UNIQUE` constraints are checked against the converted values. `TYPE`
  keeps `NOT NULL` and converts the default; `MODIFY` replaces the whole definition (type,
  `NOT NULL`, `DEFAULT`), as in MySQL, and `UNIQUE` in it adds a unique index. `USING` is not
  supported.
- A column used by a foreign key keeps its type class (`INTEGER` to `BIGINT` is allowed;
  `INTEGER` to `TEXT` is not).
- `RENAME COLUMN` renames the field in every row (in place), in its indexes (a named index
  keeps its name; composite indexes too), in `CHECK`, `UNIQUE` and `FOREIGN KEY` constraints
  (including other tables' foreign keys that reference it), and in the schema. `DROP COLUMN`
  drops the composite indexes that use the column.
- Tables created before 0.9.0 have no stored `VARCHAR` lengths: their limits apply once an
  `ALTER COLUMN ... TYPE` or `MODIFY` sets them.
- A transaction can change up to 65,536 rows. Without `BEGIN`, every statement is its own
  transaction.

## WITH and derived tables

- `WITH name [(columns)] AS (SELECT ...) [, name2 AS (...)] SELECT ...`: each WITH table is
  computed once per statement and read like a table, in `FROM`, joins and subqueries. A WITH
  table can use the others of its `WITH` (in any order) and be used several times. Only the
  WITH tables the statement uses are computed. A column list renames the columns by position.
- Derived tables: `FROM (SELECT ...) [AS] alias`, also in joins and nested; a derived table may
  start with its own `WITH`.
- `SELECT` without `FROM` (`SELECT 1`, `SELECT ? * 2`) returns one row.
- **Recursive**: a WITH table that names itself (`WITH RECURSIVE`; the word `RECURSIVE` is
  optional) is base `SELECT`s, then `UNION [ALL]` recursive `SELECT`s that read it. Each round
  runs the recursive `SELECT`s over the rows the round before added, until a round adds none.
  `UNION` drops rows seen before (so cycles end), `UNION ALL` keeps them. A `LIMIT` (and
  `OFFSET`) on the body stops it early:
  `WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 100) SELECT SUM(n) FROM c`.
  A recursive `SELECT` names its table once, in its `FROM` (not in a subquery), without
  aggregates or window functions; `ORDER BY` in a recursive body is not supported.
- A recursive WITH table stops with `LIMIT` past 1,000,000 rows or 64 MB, so runaway recursion
  fails cleanly. A WITH table used before it is computed (`WITH t AS (SELECT * FROM t)`, or
  two that use each other) fails with `INPUT ... circular reference`.
- The whole statement reads one committed state. A table-limited server user needs access to
  every table read inside WITH bodies and derived tables; WITH names themselves are not tables.
- WITH tables and derived tables are kept in memory; joins to them compare against their rows
  one by one (no index), and they cannot name columns of an outer query. `WITH` is for `SELECT`
  only (not before `INSERT`, `UPDATE` or `DELETE`) and not inside `IN (...)` or a single-value
  subquery.

## Joins and subqueries

- `JOIN` / `INNER JOIN`, `LEFT`, `RIGHT` and `FULL [OUTER] JOIN`, `CROSS JOIN` and comma
  joins, `ON` or `USING (columns)`, with table aliases (an aliased table is known only by its
  alias), `alias.*`, and up to 16 tables. In a join, qualify columns (`u.name`); an
  unqualified column is taken from the first table that has it. Unnamed result columns are
  named after the column without its table, so give duplicates an alias (`u.id AS user_id`).
- A join whose `ON` compares a column of the joined table with earlier tables
  (`o.user_id = u.id`) looks matching rows up by `id` or by an index on that column;
  index the join columns. Other joins compare every pair of rows. `WHERE` conditions on the
  first table alone narrow its scan.
- Subqueries in `IN (SELECT ...)`, `EXISTS (SELECT ...)` and as a single value
  `(SELECT ...)`, in `SELECT`, `UPDATE` and `DELETE`. One that names no outer column runs once,
  before the statement. A **correlated** one names an outer column by the outer table's name
  or alias (`o.user_id = u.id`; an unqualified column is the subquery's own): it runs for each
  outer row with those values, once per distinct value, so index the columns it filters on.
- **Window functions**: `ROW_NUMBER`, `RANK`, `DENSE_RANK`, `PERCENT_RANK`, `CUME_DIST`,
  `NTILE(n)`, `LAG`/`LEAD(x [, offset [, default]])`, `FIRST_VALUE`, `LAST_VALUE`,
  `NTH_VALUE(x, n)`, and `COUNT SUM AVG MIN MAX` with `OVER ([PARTITION BY ...] [ORDER BY ...]
  [frame])`. They work over plain rows, over joins, and over the result of `GROUP BY`/`HAVING`:
  there, window arguments, `PARTITION BY` and `ORDER BY` may use aggregates and group columns,
  e.g. `SELECT city, COUNT(*) AS n, RANK() OVER (ORDER BY COUNT(*) DESC) AS r, SUM(COUNT(*))
  OVER () AS total FROM t GROUP BY city`. Frames follow SQLite: `ROWS`, `RANGE` or `GROUPS`
  `BETWEEN` start `AND` end, each bound `UNBOUNDED PRECEDING`, `n PRECEDING`, `CURRENT ROW`,
  `n FOLLOWING` or `UNBOUNDED FOLLOWING` (or the short form `ROWS n PRECEDING`). The default
  frame runs from the start of the partition to the current row's last peer (the whole
  partition without `ORDER BY`). Offsets are constants or parameters: non-negative integers
  for `ROWS` and `GROUPS`, non-negative numbers for `RANGE`; `RANGE` with an offset needs
  exactly one `ORDER BY` expression and takes the rows whose value is a number within the
  offset (a row whose value is NULL or not a number gets its peers). `LAG` and `LEAD` ignore
  the frame. Named windows: `WINDOW w AS (...)` after `HAVING`, used as `OVER w` or
  `OVER (w ORDER BY ... frame)`. Aggregates take `FILTER (WHERE condition)`, in `GROUP BY`
  queries and in windows. Sliding `COUNT`/`SUM`/`AVG`/`MIN`/`MAX` cost about the same per row
  whatever the frame size. Not supported: `EXCLUDE` clauses and `DISTINCT` in window
  aggregates.
- `UNION [ALL]`, `INTERSECT [ALL]`, `EXCEPT [ALL]`, applied left to right; `ORDER BY` (names or
  positions) and `LIMIT` apply to the whole result.

## Consistency and speed

- Each statement is **atomic**: an `UPDATE` or `DELETE` of 1,000 rows commits all of them
  or none, and is durable before it returns. A `SELECT` sees one committed state.
- **Long reads do not hold up writes** (MVCC). A query that has run for half a millisecond
  lets a waiting commit through and goes on reading the state it started with: rows changed
  since are read as they were (from the references their changes replaced, kept while such
  a query runs). Writes wait at most about a millisecond behind a long query. This costs
  nothing for short statements. Memory: about 100 bytes per row changed while a long query
  (or a `BEGIN CONCURRENT` transaction) runs, freed the moment the last one ends
  (`stats().snapshotMemory`); no query fails for it. A `CREATE`/`DROP`/`ALTER` or a compaction while
  it runs makes it fail with `CONFLICT` (run it again). `stats().snapshotYields` counts the
  commits let through.
- Queries use an index when the `WHERE` has `=`, `IN`, ranges, `BETWEEN` or `LIKE 'prefix%'`
  on an indexed column (or `id`), and when `ORDER BY` an indexed column with a `LIMIT`
  can stop early. Other queries scan the table.
- **Composite indexes** (`CREATE [UNIQUE] INDEX ON t (a, b, ...)`, up to 8 columns) are used
  for `=` or `IN` on their leading columns, then a range, `BETWEEN` or `LIKE 'prefix%'` on the
  next one: an index on `(a, b, c)` serves `a = ?`, `a = ? AND b = ?`, `a = ? AND b > ?`,
  `a IN (...) AND b IN (...)` and `a = ? AND b = ? AND c BETWEEN ? AND ?`, but not `b = ?`
  alone. It also gives `ORDER BY` order: `WHERE a = ? ORDER BY b, c LIMIT 10` or `ORDER BY a, b
  LIMIT 10` read only the rows they return (all `ORDER BY` columns in one direction; a last
  `id` is allowed). Leading columns with no condition, or mixed `ASC`/`DESC`, mean a scan or a
  sort. A composite `UNIQUE` index refuses two rows with the same values in all of its
  columns (`CONFLICT`); a row with NULL (or no value) in any of them is exempt, as in SQL.
  `SHOW INDEX FROM t` lists each index's `columns`.
- Statements are parsed once and cached by their text, so reuse the same SQL with
  different parameters (as you would with prepared statements).

- Aggregate queries that cannot use an index (`COUNT`, `SUM`, `GROUP BY` ... over a whole
  table) read a **column cache**: the columns they need, kept in memory per table and
  updated by every commit. It is built on the first such query (from checksum-verified
  rows) and capped at about 32 million cells in total; beyond that, queries read rows.
- Large statements maintain indexes on several cores: index entries, unique checks and
  index updates for a 500-row `INSERT` run in parallel.

Measured against SQLite (WAL, `synchronous=FULL`, prepared statements), in-process from
Node, 20,000 rows, a unique and two normal indexes, Windows i5-14400, 2026-11-20 (0.9.0 with
MVCC), median of 5 runs (`node bench/sql-compare.mjs --runs 5`):

| Statement | KestroDB | SQLite | |
|---|---:|---:|:--|
| `INSERT` 500 rows per statement | 297,800 rows/s | 235,600 rows/s | KestroDB 1.26× |
| `INSERT` one row, durable | 19,700/s | 3,700/s | KestroDB 5.4× |
| `UPDATE ... WHERE id = ?`, durable | 14,200/s | 3,800/s | KestroDB 3.7× |
| `SELECT * WHERE id = ?` | 597,000/s | 310,600/s | KestroDB 1.92× |
| `SELECT * WHERE email = ?` (unique) | 425,600/s | 262,500/s | KestroDB 1.62× |
| `WHERE city = ? ORDER BY age DESC LIMIT 10` (indexed) | 4,900/s | 1,800/s | KestroDB 2.75× |
| `COUNT(*) WHERE name LIKE ?` (full scan) | 2,000/s | 1,600/s | KestroDB 1.26× |
| `GROUP BY` with `COUNT`/`AVG` (full scan) | 954/s | 382/s | KestroDB 2.50× |

The durable `UPDATE` row moves with Windows' flush timing: 14,200 to 20,700/s across runs that day.

`SELECT ... WHERE id = ?` and `WHERE col = ?` on a `UNIQUE` column read that one row
directly, without planning a query.

On Linux (Ubuntu 24.04 under WSL2, same machine, median of 5 runs) KestroDB is also first
in all eight: `SELECT` by id 794,000 against 475,400/s (1.67×), by a unique column 610,600
against 420,600/s (1.45×), indexed `ORDER BY ... LIMIT` 3.42×, `GROUP BY` 3.25×, the scan 1.56×,
bulk `INSERT` 1.25×, and durable `INSERT`/`UPDATE` 1.13× and 1.06× (the virtual disk makes
every flush slow there).
These come from one machine; SQLite's numbers moved by about 10% between runs there.
`SELECT` queries that return whole rows from a full scan without an index (no aggregate)
still read and parse every row; index the columns you filter on.

## Limits

So that no statement can exhaust the stack (a crash of the server), a statement may nest
at most 64 levels of parentheses, function calls, `CASE`, `NOT` and unary minus, and 16
levels of subqueries, and an expression may have at most 500 levels of operators (write
long `OR` chains as `IN (...)`, which has no limit). A statement beyond them fails with
`INPUT`. A statement changes at most 65,536 rows, a transaction 65,536 rows.

## Full-text search

```sql
CREATE FULLTEXT INDEX posts_text ON posts (title, body);           -- or: CREATE TABLE ... (..., FULLTEXT (title, body))
CREATE FULLTEXT INDEX tags_ft ON posts (tags) WITH (min_token_length = 3);
ALTER TABLE posts ADD FULLTEXT INDEX notes_ft (notes);
DROP INDEX posts_text;

SELECT id, title, MATCH (title, body) AGAINST ('database tuning') AS score
FROM posts WHERE MATCH (title, body) AGAINST ('database tuning')
ORDER BY score DESC LIMIT 10;

SELECT id FROM posts
WHERE MATCH (title, body) AGAINST ('+kestro -mysql "write ahead" index*' IN BOOLEAN MODE);
```

`MATCH (columns) AGAINST (text [IN NATURAL LANGUAGE MODE | IN BOOLEAN MODE])` needs a
FULLTEXT index on exactly those columns (in any order). The text is a constant: a string or a
parameter. MATCH works in single-table SELECT, UPDATE and DELETE: in WHERE it keeps matching
rows, and anywhere else (select list, ORDER BY, GROUP BY, HAVING, aggregates) it is the row's
relevance, a BM25 score (k1 = 1.2, b = 0.75) above 0 for matching rows and 0 for the others.
It is not supported in joins (use `WHERE id IN (SELECT id FROM t WHERE MATCH ...)`), INSERT
or CHECK.

- **Natural language mode** (the default): rows containing any of the words; relevance favours
  rare words, more occurrences and shorter rows.
- **Boolean mode**: items separated by spaces. `word` is optional (adds relevance), `+word` is
  required, `-word` is excluded, `word*` matches words starting with `word`, `"a phrase"`
  matches the words in a row (within one column). Several words joined by punctuation inside
  one item (`e-mail`, `x-ray`) form a phrase. Without a `+` item a row needs at least one
  optional item; a query with only `-` items matches nothing. MySQL's `< > ~ ( ) @` operators
  and `WITH QUERY EXPANSION` are not supported.

Tokens are runs of letters and digits, lower-cased: ASCII, Latin-1, Latin Extended-A, Greek and
Cyrillic letters are case-folded; accents are kept (`café` is not `cafe`). Spaces, ASCII and
Latin-1 punctuation and symbols, general punctuation (U+2000–U+206F) and ideographic
punctuation separate words; every other character is part of a word, so Chinese, Japanese and
Thai text without spaces is one token per run (no word segmentation). There is no stemming and
no stop-word list. Only text values are indexed. Words shorter than `min_token_length`
(characters, default 1, at most 64) and longer than 64 bytes are ignored in rows and queries
alike (a prefix may be shorter). A query has at most 64 words or phrases; an index at most 8
columns.

The index is a secondary index kept by every commit, like the others: it is built on the first
query after open and needs nothing on disk, so it is right after crashes, compaction and
replication. ROLLBACK leaves it untouched, and rows a transaction changed are matched from
their text. ALTER TABLE RENAME COLUMN and RENAME TO move it; DROP COLUMN removes the column
from it (and the index with its last column). `SHOW INDEX` lists it with `"type": "FULLTEXT"`.
In a serializable `BEGIN CONCURRENT` transaction a MATCH counts as reading the whole table.
On 100,000 rows a selective word takes well under a millisecond (0.03–0.08 ms, against
73–150 ms for `LIKE '%word%'`); a word in most rows costs about 2 µs per matching row, so
`LIKE` over the column cache is faster for very common words.

## Change notifications (LISTEN / NOTIFY)

```sql
LISTEN orders;                       -- or LISTEN "any text", LISTEN 'any text'
NOTIFY orders, 'order 42 paid';      -- payload: text up to 8000 bytes (a ? parameter works)
SELECT pg_notify('orders', ?);       -- the same, as a function
UNLISTEN orders;  UNLISTEN *;
LISTEN "table_changes:users";        -- every insert/update/delete of users
```

A NOTIFY inside a transaction is sent only when it commits (none after ROLLBACK, or after
`ROLLBACK TO` a savepoint taken before it); each distinct channel and payload once per
transaction. Outside a transaction it is sent at once. Listeners receive notifications in
commit order, each with the `txid` of the commit it followed. The channel
`table_changes:<table>` carries `{"op":"insert"|"update"|"delete","table":"users","key":<id>}`
for every row or key a commit changes (SQL, key/value writes, async writes, and commits a
follower replicates). Channel names are case-sensitive, 1–255 bytes. On the server, LISTEN and
NOTIFY count as reads, and watching `table_changes:T` needs read access to T.

```js
// In-process
const stop = db.listen('orders', n => console.log(n.channel, n.payload, n.txid));
const unwatch = db.watch('users', c => console.log(c.op, c.key));   // {op, table, key, txid}
db.notify('orders', 'paid');           // inside a transaction: sent at COMMIT
stop(); unwatch();                     // a listening Database keeps the process alive until then (or close())

// Server
const sub = await remote.listen('orders', n => console.log(n.payload));   // also remote.watch(), remote.on('notification')
await other.notify('orders', 'paid');
await sub();
```

Callbacks run asynchronously on the JS thread after the commit, never inside it; with no
listener a commit pays one atomic load. In-process, up to 64 MB may wait undelivered (beyond
that they are dropped and counted in a `'notificationsLost'` event). On kestro-server a
listening connection receives notification frames (status byte 2: u64 txid, u32 channel
length, channel, payload) between replies; clients that never LISTEN are unaffected. Each
connection may fall `--notify-queue` MB (default 16) behind; a slower one is disconnected
(`stats().listenersDropped`) and commits never wait for it. RemoteDB then emits
`'disconnect'`, reconnects, listens again and emits `'reconnect'` (notifications sent in
between are lost). Limits: LISTEN and UNLISTEN take effect at once, even inside a
transaction; no LISTEN over `tls://` yet; NOTIFY is not sent to followers; other processes
writing the same file (the worker, the C library) are not seen.

## Not supported yet

`NATURAL` joins, deferred constraints, `ALTER COLUMN ... TYPE ... USING`, MySQL `CHANGE
COLUMN`, several changes in one `ALTER TABLE`, `WITH` before `INSERT`/`UPDATE`/`DELETE`,
window `EXCLUDE` clauses, `MATCH` inside joins, stemming in full-text search, and `LISTEN`
over `tls://`.
In-process readers (`readFrom`, `read_from`) answer `get`/`getMany`; SQL from those clients
runs on the server.

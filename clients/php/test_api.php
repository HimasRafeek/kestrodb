<?php
// php test_api.php api ADDRESS TOKEN [EMBEDDED_PATH]   the SQL API, server and embedded
// php test_api.php reconnect ADDRESS TOKEN              survives a server restart (reads a line from stdin first)
// Exits non-zero on the first failure.
declare(strict_types=1);
require __DIR__ . '/KestroDB.php';

use KestroDB\{Client, Embedded, Exception, ConflictException, ConnectionException, InputException};

$fail = function (string $what) { fwrite(STDERR, "FAIL $what\n"); exit(1); };
$eq = function ($a, $b, string $what) use ($fail) { if ($a !== $b) $fail("$what: got " . json_encode($a) . ' expected ' . json_encode($b)); };
$mode = $argv[1] ?? '';

if ($mode === 'reconnect') {
    $db = new Client($argv[2], $argv[3]);
    $db->execute('CREATE TABLE IF NOT EXISTS rc (id INTEGER PRIMARY KEY, v TEXT)');
    $db->execute('INSERT INTO rc (v) VALUES (?)', ['before']);
    echo "READY\n";
    fgets(STDIN);            // the server restarts meanwhile
    sleep(2);                // idle long enough for the check before the next request
    $db->execute('INSERT INTO rc (v) VALUES (?)', ['after']);
    $eq($db->queryValue('SELECT COUNT(*) FROM rc'), 2, 'rows after reconnect');
    echo "ok php reconnect\n";
    exit(0);
}

// The same checks against any connection that has the SQL API.
$check = function ($db, string $name) use ($fail, $eq) {
    $db->execute('CREATE TABLE IF NOT EXISTS people (id INTEGER PRIMARY KEY, email TEXT UNIQUE, name TEXT, age INTEGER)');
    $db->execute('DELETE FROM people');
    // Named, positional and numbered parameters; lastInsertId.
    $eq($db->execute('INSERT INTO people (email, name, age) VALUES (:email, :name, :age)', ['email' => 'a@x.org', ':name' => 'Ann', 'age' => 31]), 1, "$name insert named");
    $a = $db->lastInsertId();
    $db->execute('INSERT INTO people (email, name, age) VALUES (?, ?, ?)', ['b@x.org', 'Bob', 25]);
    $db->execute('INSERT INTO people (email, name, age) VALUES ($2, $1, $3)', ['Cy', 'c@x.org', 41]);
    $eq($db->queryValue('SELECT name FROM people WHERE id = ?', [$a]), 'Ann', "$name lastInsertId");
    $eq(array_column($db->query('SELECT name FROM people WHERE age > :min ORDER BY age', ['min' => 26]), 'name'), ['Ann', 'Cy'], "$name query");
    $eq($db->queryOne('SELECT name, age FROM people WHERE email = @e', ['e' => 'b@x.org']), ['name' => 'Bob', 'age' => 25], "$name queryOne");
    $eq($db->queryOne('SELECT * FROM people WHERE id = ?', [999]), null, "$name queryOne none");
    $eq($db->queryValue('SELECT COUNT(*) FROM people'), 3, "$name queryValue");
    // Unicode and types round-trip.
    $db->execute('INSERT INTO people (email, name, age) VALUES (?, ?, ?)', ['d@x.org', 'Dévi ✓ 日本', null]);
    $eq($db->queryOne('SELECT name, age FROM people WHERE email = ?', ['d@x.org']), ['name' => 'Dévi ✓ 日本', 'age' => null], "$name unicode/null");
    // Errors map to classes.
    try { $db->execute('INSERT INTO people (email) VALUES (?)', ['a@x.org']); $fail("$name duplicate accepted"); } catch (ConflictException $e) { $eq($e->kind, 'CONFLICT', "$name conflict kind"); }
    try { $db->query('SELEC nonsense'); $fail("$name bad SQL accepted"); } catch (InputException $e) {}
    try { $db->query('SELECT * FROM people WHERE id = :id', []); $fail("$name missing param accepted"); } catch (InputException $e) {}
    try { $db->query('SELECT * FROM people WHERE id = :id AND age = ?', [1, 2]); $fail("$name mixed params accepted"); } catch (InputException $e) {}
    // Prepared statements, PDO style.
    $st = $db->prepare('SELECT name FROM people WHERE age >= ? ORDER BY age');
    $st->execute([30]);
    $eq($st->fetchColumn(), 'Ann', "$name fetchColumn");
    $eq($st->fetch(), ['name' => 'Cy'], "$name fetch");
    $eq($st->fetch(), false, "$name fetch end");
    $st->execute([0]);
    $eq($st->rowCount(), 3, "$name rowCount select");
    $eq(count($st->fetchAll()), 3, "$name fetchAll");
    $up = $db->prepare('UPDATE people SET age = age + 1 WHERE age >= :min');
    $up->bindValue(':min', 30);
    $up->execute();
    $eq($up->rowCount(), 2, "$name rowCount update");
    $names = [];
    $st->execute([0]);
    foreach ($st as $row) $names[] = $row['name'];
    $eq(count($names), 3, "$name iterate");
    // Transactions: commit, explicit rollback, rollback on exception.
    $db->transaction(function ($db) { $db->execute('UPDATE people SET age = 50 WHERE email = ?', ['b@x.org']); });
    $eq($db->queryValue('SELECT age FROM people WHERE email = ?', ['b@x.org']), 50, "$name transaction commit");
    $db->beginTransaction();
    $eq($db->inTransaction(), true, "$name inTransaction");
    $db->execute('DELETE FROM people');
    $eq($db->queryValue('SELECT COUNT(*) FROM people'), 0, "$name read your writes");
    $db->rollBack();
    $eq($db->queryValue('SELECT COUNT(*) FROM people'), 4, "$name rollback");
    try {
        $db->transaction(function ($db) { $db->execute('DELETE FROM people'); throw new \RuntimeException('boom'); });
        $fail("$name exception swallowed");
    } catch (\RuntimeException $e) { $eq($e->getMessage(), 'boom', "$name exception rethrown"); }
    $eq($db->inTransaction(), false, "$name no transaction after rollback");
    $eq($db->queryValue('SELECT COUNT(*) FROM people'), 4, "$name rolled back on exception");
    $eq($db->transaction(fn($db) => $db->queryValue('SELECT COUNT(*) FROM people')), 4, "$name transaction result");
    try { $db->commit(); $fail("$name commit without transaction"); } catch (InputException $e) {}
    // Key/value API next to SQL.
    $v = $db->set('kv', 'k1', ['n' => 1]);
    $eq($db->get('kv', 'k1'), ['version' => $v, 'value' => ['n' => 1]], "$name kv");
    try { $db->set('kv', 'k1', 2, 0); $fail("$name cas accepted"); } catch (ConflictException $e) {}
    $eq($db->getMany('kv', ['k1', 'nope'])[1], null, "$name getMany");
    $db->delete('kv', 'k1');
    $eq($db->get('kv', 'k1'), null, "$name delete");
};

if ($mode === 'api') {
    $db = new Client($argv[2], $argv[3]);
    $check($db, 'server');
    // Errors from the connection itself.
    try { new Client('tcp://127.0.0.1:1', null, 1.0); $fail('connect to a closed port'); } catch (ConnectionException $e) {}
    try { new Client($argv[2], 'wrong-token-0123456789'); $fail('wrong token accepted'); } catch (Exception $e) { $eq($e->kind, 'AUTH', 'auth kind'); }
    $c = $db->compact();
    $eq($c['after'] <= $c['before'], true, 'server compact');
    $eq($db->queryValue('SELECT COUNT(*) FROM people'), 4, 'rows after compact');
    // Persistent connection: a transaction left open is rolled back when it is reused.
    $p = new Client($argv[2], $argv[3], 5.0, ['persistent' => true]);
    $p->beginTransaction();
    $p->execute('DELETE FROM people');
    unset($p);  // like a request that ends mid-transaction
    $p2 = new Client($argv[2], $argv[3], 5.0, ['persistent' => true]);
    $eq($p2->queryValue('SELECT COUNT(*) FROM people'), 4, 'persistent: leftover transaction rolled back');
    $db->close();
    try { $db->query('SELECT 1 FROM people'); $fail('closed client used'); } catch (ConnectionException $e) {}
    if (isset($argv[4])) {
        $e = Embedded::open($argv[4], true);
        $check($e, 'embedded');
        $eq($e->count() > 0, true, 'embedded count');
        $e->close();
        $e2 = Embedded::open($argv[4]);
        $eq($e2->queryValue('SELECT COUNT(*) FROM people'), 4, 'embedded reopen');
        // Backups, archive and point-in-time restore, embedded.
        $dir = dirname($argv[4]);
        $eq($e2->compact()['after'] > 0, true, 'embedded compact');
        $tx1 = $e2->archive("$dir/php-archive");
        $e2->execute('DELETE FROM people');
        $tx2 = $e2->archive("$dir/php-archive");
        $eq($tx2 > $tx1, true, 'archive advanced');
        $e2->backup("$dir/php-backup.kdb");
        $eq(Embedded::restore("$dir/php-archive/archive.kdb", "$dir/php-restored.kdb", $tx1), $tx1, 'restore txid');
        $e2->close();
        $r = Embedded::open("$dir/php-restored.kdb");
        $eq($r->queryValue('SELECT COUNT(*) FROM people'), 4, 'restored before the delete');
        $r->close();
        $b = Embedded::open("$dir/php-backup.kdb");
        $eq($b->queryValue('SELECT COUNT(*) FROM people'), 0, 'backup after the delete');
        $b->close();
        $e2 = Embedded::open($argv[4]);
        $e2->close();
    }
    echo "ok php api\n";
    exit(0);
}
$fail('usage');

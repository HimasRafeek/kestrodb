<?php
// php test.php ADDRESS TOKEN WORKER_ID COUNT [DB_PATH]  (DB_PATH: read in-process through FFI)
// Exercises the PHP client against a running kestro-server. Run several at once to test
// concurrent connections; exits non-zero on any failure.
declare(strict_types=1);
require __DIR__ . '/KestroDB.php';

[$_, $address, $token, $worker, $count] = $argv;
$count = (int) $count;
$fail = function (string $what) { fwrite(STDERR, "FAIL $what\n"); exit(1); };
$db = new KestroDB\Client($address, $token, 5.0, isset($argv[5]) ? ['readFrom' => $argv[5]] : []);
if (isset($argv[5])) {
    // Reads come from this process; check they see a write acknowledged a moment ago.
    $v = $db->set('php', "fresh-$worker", 'x');
    if ($db->get('php', "fresh-$worker")['version'] !== $v) $fail('in-process read missed an acknowledged write');
    $db->delete('php', "fresh-$worker");
    if ($db->get('php', "fresh-$worker") !== null) $fail('in-process read missed an acknowledged delete');
}

// Writes, each durable when it returns; then read them back.
for ($i = 0; $i < $count; $i++) {
    $db->set('php', "w$worker-$i", ['worker' => (int) $worker, 'i' => $i, 'text' => "héllo ✓ $i"]);
}
for ($i = 0; $i < $count; $i++) {
    $r = $db->get('php', "w$worker-$i");
    if ($r === null || $r['value']['i'] !== $i || $r['value']['text'] !== "héllo ✓ $i") $fail("read back $i");
}
$many = $db->getMany('php', ["w$worker-0", 'missing', "w$worker-0"]);
if ($many[1] !== null || $many[0]['value']['i'] !== 0 || $many[2]['version'] !== $many[0]['version']) $fail('getMany');

// Conditional update and conflict.
$key = "cas-$worker";
$v = $db->set('php', $key, 1, 0);
$v2 = $db->set('php', $key, 2, $v);
try { $db->set('php', $key, 3, $v); $fail('stale expectedVersion accepted'); } catch (KestroDB\Exception $e) { if ($e->kind !== 'CONFLICT') $fail('conflict kind ' . $e->kind); }
if ($db->get('php', $key)['value'] !== 2) $fail('cas value');

// Atomic batch and delete.
$version = $db->batch([
    ['type' => 'set', 'collection' => 'php', 'key' => "b$worker-1", 'value' => [1, 2, 3]],
    ['type' => 'set', 'collection' => 'php', 'key' => "b$worker-2", 'value' => ['nested' => ['ok' => true]]],
]);
if ($db->get('php', "b$worker-1")['version'] !== $version || $db->get('php', "b$worker-2")['version'] !== $version) $fail('batch version');
$db->delete('php', "b$worker-1");
if ($db->get('php', "b$worker-1") !== null) $fail('delete');

// Input checks never reach the server.
foreach ([['bad name', 'k'], ['php', ''], ['php', "a\0b"]] as [$c, $k]) {
    try { $db->set($c, $k, 1); $fail("accepted $c/$k"); } catch (KestroDB\Exception $e) { if ($e->kind !== 'INPUT') $fail('input kind'); }
}
if (!$db->stats()['server']) $fail('stats');

// SQL from several processes at once: auto ids, unique constraint, parameters.
$db->sql('CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, sku TEXT UNIQUE, qty INT)');
for ($i = 0; $i < 20; $i++) $db->sql('INSERT INTO items (sku, qty) VALUES (?, ?)', ["php-$worker-$i", $i]);
$r = $db->sql('SELECT sku, qty FROM items WHERE sku = ?', ["php-$worker-7"]);
if ($r['rows'] !== [['sku' => "php-$worker-7", 'qty' => 7]]) $fail('sql select ' . json_encode($r));
try { $db->sql('INSERT INTO items (sku) VALUES (?)', ["php-$worker-7"]); $fail('sql unique accepted'); } catch (KestroDB\Exception $e) { if ($e->kind !== 'CONFLICT') $fail('sql unique kind ' . $e->kind); }
$u = $db->sql('UPDATE items SET qty = qty + 100 WHERE sku LIKE ?', ["php-$worker-%"]);
if ($u['changes'] !== 20) $fail('sql update ' . json_encode($u));
// A transaction: invisible until COMMIT, gone after ROLLBACK.
$db->sql('BEGIN');
$db->sql('UPDATE items SET qty = 0 WHERE sku LIKE ?', ["php-$worker-%"]);
if ($db->sql('SELECT SUM(qty) AS s FROM items WHERE sku LIKE ?', ["php-$worker-%"])['rows'][0]['s'] !== 0) $fail('sql transaction read-your-writes');
$db->sql('ROLLBACK');
$c = $db->sql('SELECT COUNT(*) AS n, MIN(qty) AS lo FROM items WHERE sku LIKE ?', ["php-$worker-%"]);
if ($c['rows'][0] !== ['n' => 20, 'lo' => 100]) $fail('sql aggregate ' . json_encode($c));
$db->close();
echo "ok php worker $worker\n";

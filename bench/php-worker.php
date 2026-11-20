<?php
// php php-worker.php ENGINE TARGET TOKEN MODE WORKER COUNT KEYS
//   ENGINE  kestrodb (TARGET = server address) | sqlite (TARGET = database file)
//           | mysql (TARGET = PDO DSN)
//   MODE    write: COUNT durable single writes | read: COUNT random point reads of KEYS keys
//           | seed: write KEYS keys in batches of 1000
// Prints {"start":seconds,"end":seconds,"ops":n} measured around the work loop only.
declare(strict_types=1);
[$_, $engine, $target, $token, $mode, $worker, $count, $keys] = $argv;
$count = (int) $count; $keys = (int) $keys;
$doc = fn(int $i) => ['id' => $i, 'name' => "user-$i", 'email' => "user$i@example.com", 'tags' => ['a', 'b', 'c'], 'score' => $i * 7 % 1000, 'bio' => str_repeat('x', 120)];

if ($engine === 'kestrodb') {
    // TARGET = server address, or "address|database path" for in-process reads (FFI)
    require __DIR__ . '/../clients/php/KestroDB.php';
    [$address, $path] = array_pad(explode('|', $target, 2), 2, null);
    $db = new KestroDB\Client($address, $token === '-' ? null : $token, 5.0, $path === null ? [] : ['readFrom' => $path]);
    $set = fn(string $k, $v) => $db->set('c', $k, $v);
    $get = fn(string $k) => $db->get('c', $k)['value'];
    $seed = function (int $from, int $to) use ($db, $doc) { $ops = []; for ($i = $from; $i < $to; $i++) $ops[] = ['type' => 'set', 'collection' => 'c', 'key' => "k$i", 'value' => $doc($i)]; $db->batch($ops); };
} else {
    if ($engine === 'mysql') {
        // TARGET = PDO DSN, e.g. mysql:host=127.0.0.1;port=3306;dbname=bench; login from
        // KESTRO_MYSQL_USER / KESTRO_MYSQL_PASSWORD (default root, no password)
        $pdo = new PDO($target, getenv('KESTRO_MYSQL_USER') ?: 'root', getenv('KESTRO_MYSQL_PASSWORD') ?: '', [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::ATTR_EMULATE_PREPARES => false]);
        if ($mode === 'seed') $pdo->exec('DROP TABLE IF EXISTS kv');
        $pdo->exec('CREATE TABLE IF NOT EXISTS kv (k VARBINARY(255) PRIMARY KEY, v LONGTEXT NOT NULL, version BIGINT NOT NULL) ENGINE=InnoDB');
        $put = $pdo->prepare('INSERT INTO kv (k, v, version) VALUES (?, ?, 1) ON DUPLICATE KEY UPDATE v = VALUES(v), version = version + 1');
    } else {
        $pdo = new PDO("sqlite:$target", null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
        $pdo->exec('PRAGMA busy_timeout=30000; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT NOT NULL, version INTEGER NOT NULL) WITHOUT ROWID;');
        $put = $pdo->prepare('INSERT INTO kv (k, v, version) VALUES (?, ?, 1) ON CONFLICT(k) DO UPDATE SET v = excluded.v, version = version + 1');
    }
    $sel = $pdo->prepare('SELECT v FROM kv WHERE k = ?');
    $set = function (string $k, $v) use ($put) { $put->execute([$k, json_encode($v)]); };
    $get = function (string $k) use ($sel) { $sel->execute([$k]); $r = $sel->fetchColumn(); $sel->closeCursor(); return json_decode($r, true); };
    $seed = function (int $from, int $to) use ($pdo, $put, $doc) { $pdo->beginTransaction(); for ($i = $from; $i < $to; $i++) $put->execute(["k$i", json_encode($doc($i))]); $pdo->commit(); };
}

$start = microtime(true);
if ($mode === 'seed') {
    for ($i = 0; $i < $keys; $i += 1000) $seed($i, min($i + 1000, $keys));
    $ops = $keys;
} elseif ($mode === 'write') {
    for ($i = 0; $i < $count; $i++) $set("w$worker-$i", $doc($i));
    $ops = $count;
} else {
    mt_srand((int) $worker);
    for ($i = 0; $i < $count; $i++) { $n = mt_rand(0, $keys - 1); if ($get("k$n")['id'] !== $n) { fwrite(STDERR, "bad read\n"); exit(1); } }
    $ops = $count;
}
echo json_encode(['start' => $start, 'end' => microtime(true), 'ops' => $ops]), "\n";

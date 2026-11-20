<?php
/**
 * KestroDB client for PHP 7.4+ (64-bit).
 *
 * Server (TCP, TLS or a Unix socket):
 *
 *   require 'KestroDB.php';
 *   $db = new KestroDB\Client('tcp://127.0.0.1:7820', getenv('KESTRODB_TOKEN') ?: null);
 *   $db->execute('INSERT INTO users (email, name) VALUES (:email, :name)', ['email' => 'a@example.com', 'name' => 'A']);
 *   $id    = $db->lastInsertId();
 *   $rows  = $db->query('SELECT * FROM users WHERE city = ?', ['Kandy']);
 *   $user  = $db->queryOne('SELECT * FROM users WHERE id = ?', [$id]);
 *   $count = $db->queryValue('SELECT COUNT(*) FROM users');
 *   $db->transaction(function (KestroDB\Client $db) { ... });   // COMMIT, or ROLLBACK on an exception
 *   $st = $db->prepare('SELECT * FROM users WHERE id = ?'); $st->execute([1]); $row = $st->fetch();
 *
 * In the same process, no server (PHP CLI, scripts, tests):
 *
 *   $db = KestroDB\Embedded::open('/data/app.kdb', true);   // the same SQL and key/value API
 *
 * Key/value API on both: get, getMany, set, delete, batch. Values are stored as JSON.
 *
 * Client options (4th constructor argument):
 *   'readFrom'   => '/path/server.kdb'  get()/getMany() read the file in-process (PHP FFI)
 *   'library'    => path to kestrodb.dll / libkestrodb.so (default: KESTRODB_LIBRARY or ../../bin)
 *   'tls'        => PHP ssl context options for tls:// addresses, e.g. ['cafile' => '/etc/ca.pem'];
 *                   a client certificate (a server with --tls-client-ca): 'local_cert' => PEM path,
 *                   'local_pk' => key path
 *   'persistent' => true   PHP-FPM: keep the connection open across requests (a transaction
 *                          left open by a request that died is rolled back on reuse)
 *   'reconnect'  => true   (default) reconnect when the server closed an idle connection
 *                          (never inside a transaction, never for a request already sent)
 *
 * Errors are KestroDB\Exception (->kind: INPUT, CONFLICT, LIMIT, AUTH, BUSY, IO, CORRUPT, ...)
 * or one of its subclasses: ConnectionException, ConflictException, InputException,
 * AuthException, LimitException, BusyException, CorruptException.
 */
declare(strict_types=1);

namespace KestroDB;

class Exception extends \RuntimeException
{
    /** The leading error code: INPUT, LIMIT, CONFLICT, CORRUPT, AUTH, BUSY, IO, CLOSED, ... */
    public string $kind;

    public function __construct(string $message, ?\Throwable $previous = null)
    {
        parent::__construct($message, 0, $previous);
        $this->kind = strtok($message, ' ') ?: 'ERROR';
    }

    /** The subclass for an error message from the engine or the server. */
    public static function from(string $message): self
    {
        $kind = strtok($message, ' ') ?: 'ERROR';
        switch ($kind) {
            case 'IO': case 'CLOSED': case 'UNKNOWN_COMMIT': return new ConnectionException($message);
            case 'CONFLICT': return new ConflictException($message);
            case 'INPUT': return new InputException($message);
            case 'AUTH': return new AuthException($message);
            case 'LIMIT': return new LimitException($message);
            case 'BUSY': return new BusyException($message);
            case 'CORRUPT': return new CorruptException($message);
            default: return new self($message);
        }
    }
}
/** The connection failed or was lost; a write without a reply has an unknown outcome. */
class ConnectionException extends Exception {}
/** A unique constraint, a duplicate id or a version check failed. */
class ConflictException extends Exception {}
/** Invalid SQL, parameters, names or values. */
class InputException extends Exception {}
class AuthException extends Exception {}
class LimitException extends Exception {}
class BusyException extends Exception {}
class CorruptException extends Exception {}

/**
 * SQL conveniences shared by Client and Embedded. Parameters: a list for ?, ?N or $N
 * placeholders, or an associative array for :name / @name placeholders.
 */
trait SqlMethods
{
    private $lastInsertId = null;
    private bool $inTransaction = false;

    /** Runs one statement; returns the engine's result: ['rows' => [...]] or ['changes' => n, 'lastInsertId' => id]. */
    abstract public function sql(string $sql, array $params = []): array;

    /** All rows of a SELECT, as associative arrays. */
    public function query(string $sql, array $params = []): array
    {
        $r = $this->sql($sql, $params);
        if (!array_key_exists('rows', $r)) throw new InputException('INPUT query() needs a statement that returns rows; use execute()');
        return $r['rows'];
    }

    /** The first row, or null. */
    public function queryOne(string $sql, array $params = []): ?array
    {
        $rows = $this->query($sql, $params);
        return $rows[0] ?? null;
    }

    /** The first column of the first row, or null. */
    public function queryValue(string $sql, array $params = [])
    {
        $row = $this->queryOne($sql, $params);
        if ($row === null) return null;
        foreach ($row as $v) return $v;
        return null;
    }

    /** INSERT, UPDATE, DELETE or DDL; returns the number of rows changed. */
    public function execute(string $sql, array $params = []): int
    {
        $r = $this->sql($sql, $params);
        if (array_key_exists('lastInsertId', $r)) $this->lastInsertId = $r['lastInsertId'];
        return (int) ($r['changes'] ?? 0);
    }

    /** The id of the last row INSERT created through this connection (null before any). */
    public function lastInsertId()
    {
        return $this->lastInsertId;
    }

    public function beginTransaction(): void
    {
        if ($this->inTransaction) throw new InputException('INPUT a transaction is already open');
        $this->sql('BEGIN');
        $this->inTransaction = true;
    }

    public function commit(): void
    {
        if (!$this->inTransaction) throw new InputException('INPUT no transaction is open');
        $this->inTransaction = false;
        $this->sql('COMMIT');
    }

    public function rollBack(): void
    {
        if (!$this->inTransaction) throw new InputException('INPUT no transaction is open');
        $this->inTransaction = false;
        $this->sql('ROLLBACK');
    }

    public function inTransaction(): bool
    {
        return $this->inTransaction;
    }

    /**
     * Runs $fn($this) inside a transaction: COMMIT when it returns, ROLLBACK (and rethrow)
     * when it throws. Returns what $fn returns.
     */
    public function transaction(callable $fn)
    {
        $this->beginTransaction();
        try {
            $result = $fn($this);
        } catch (\Throwable $e) {
            if ($this->inTransaction) {
                try { $this->rollBack(); } catch (\Throwable $ignored) { $this->inTransaction = false; }
            }
            throw $e;
        }
        $this->commit();
        return $result;
    }

    /** A prepared statement (PDO style): execute(), fetch(), fetchAll(), fetchColumn(), rowCount(). */
    public function prepare(string $sql): Statement
    {
        return new Statement($this, $sql);
    }

    private static function paramsJson(array $params): string
    {
        if ($params === []) return '[]';
        $flags = JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRESERVE_ZERO_FRACTION;
        $i = 0; $list = true;
        foreach ($params as $k => $_) { if ($k !== $i++) { $list = false; break; } }
        if ($list) {
            $json = json_encode($params, $flags);
        } else {
            $named = [];
            foreach ($params as $k => $v) $named[ltrim((string) $k, ':@')] = $v;
            $json = json_encode((object) $named, $flags);
        }
        if ($json === false) throw new InputException('INPUT SQL parameters are not JSON-encodable: ' . json_last_error_msg());
        return $json;
    }
}

/** A prepared statement, PDO style. Iterating it yields the rows of the last execute(). */
final class Statement implements \IteratorAggregate, \Countable
{
    private $db;
    private string $sql;
    private array $bound = [];
    private array $rows = [];
    private int $pos = 0;
    private int $changes = 0;

    /** @param Client|Embedded $db */
    public function __construct($db, string $sql)
    {
        $this->db = $db;
        $this->sql = $sql;
    }

    /** $param: 1-based position for ?, or a name (with or without ':') for :name. */
    public function bindValue($param, $value): bool
    {
        $this->bound[is_int($param) ? $param - 1 : ltrim((string) $param, ':@')] = $value;
        return true;
    }

    /** Runs the statement with $params (or the bound values). */
    public function execute(?array $params = null): bool
    {
        if ($params === null) {
            $params = $this->bound;
            if ($params && array_keys($params) === range(0, count($params) - 1)) ksort($params);
        }
        $r = $this->db->sql($this->sql, $params);
        $this->rows = $r['rows'] ?? [];
        $this->pos = 0;
        $this->changes = isset($r['rows']) ? count($r['rows']) : (int) ($r['changes'] ?? 0);
        return true;
    }

    /** The next row (associative array), or false at the end, as PDO::fetch(). */
    public function fetch()
    {
        return $this->rows[$this->pos++] ?? false;
    }

    public function fetchAll(): array
    {
        $rest = array_slice($this->rows, $this->pos);
        $this->pos = count($this->rows);
        return $rest;
    }

    /** A column (0-based position or name) of the next row, or false at the end. */
    public function fetchColumn($column = 0)
    {
        $row = $this->fetch();
        if ($row === false) return false;
        if (is_string($column)) return $row[$column] ?? null;
        $values = array_values($row);
        return $values[$column] ?? null;
    }

    /** Rows changed by INSERT/UPDATE/DELETE, or rows returned by SELECT. */
    public function rowCount(): int
    {
        return $this->changes;
    }

    public function columnCount(): int
    {
        return isset($this->rows[0]) ? count($this->rows[0]) : 0;
    }

    public function closeCursor(): bool
    {
        $this->rows = [];
        $this->pos = 0;
        return true;
    }

    public function getIterator(): \Iterator
    {
        return new \ArrayIterator(array_slice($this->rows, $this->pos));
    }

    public function count(): int
    {
        return count($this->rows);
    }
}

/** FFI bindings to the KestroDB C library, loaded once per process. */
final class Native
{
    private static ?\FFI $ffi = null;

    public static function ffi(?string $library = null): \FFI
    {
        if (self::$ffi !== null) return self::$ffi;
        if (!extension_loaded('ffi')) {
            throw new Exception('UNSUPPORTED this needs the PHP FFI extension (extension=ffi; for PHP-FPM also ffi.enable=true)');
        }
        $library = $library ?? (getenv('KESTRODB_LIBRARY') ?: self::defaultLibrary());
        return self::$ffi = \FFI::cdef('typedef struct kestro_db kestro_db;
            kestro_db* kestro_open(const char* path, int mode, char* err, size_t errlen);
            int kestro_get(kestro_db* db, const char* key, size_t klen, uint64_t* version, const char** value, size_t* vlen, char* err, size_t errlen);
            int kestro_get_many(kestro_db* db, const char* payload, size_t n, const unsigned char** reply, size_t* rlen, char* err, size_t errlen);
            int kestro_batch(kestro_db* db, const char* payload, size_t n, uint64_t* version, char* err, size_t errlen);
            int kestro_sql(kestro_db* db, const char* sql, size_t sqllen, const char* params, size_t plen, const char** result, size_t* rlen, char* err, size_t errlen);
            int64_t kestro_count(kestro_db* db);
            int kestro_backup(kestro_db* db, const char* dest, uint64_t* txid, char* err, size_t errlen);
            int kestro_compact(kestro_db* db, uint64_t* before, uint64_t* after, char* err, size_t errlen);
            int kestro_archive(kestro_db* db, const char* dir, uint64_t* txid, char* err, size_t errlen);
            int kestro_restore(const char* source, const char* dest, uint64_t txid, int64_t at_ms, uint64_t* restored_txid, char* err, size_t errlen);
            void kestro_close(kestro_db* db);', $library);
    }

    /**
     * The library the release package for Composer bundles (native/<os>-<arch>/ beside this
     * file: windows-x64, linux-x64, linux-arm64), else a repository checkout's or release
     * archive's bin/. KESTRODB_LIBRARY or the $library argument override both.
     */
    public static function defaultLibrary(): string
    {
        $name = PHP_OS_FAMILY === 'Windows' ? 'kestrodb.dll' : (PHP_OS_FAMILY === 'Darwin' ? 'libkestrodb.dylib' : 'libkestrodb.so');
        $machine = strtolower(php_uname('m'));
        $arch = in_array($machine, ['x86_64', 'amd64', 'x64'], true) ? 'x64' : (in_array($machine, ['aarch64', 'arm64'], true) ? 'arm64' : $machine);
        $bundled = __DIR__ . '/native/' . strtolower(PHP_OS_FAMILY) . '-' . $arch . '/' . $name;
        return is_file($bundled) ? $bundled : dirname(__DIR__, 2) . '/bin/' . $name;
    }
}

/** Shared key/value encoding (the server's batch and getMany layouts). */
trait KeyValueCodec
{
    private static function id(string $collection, string $key): string
    {
        if (!preg_match('/^[A-Za-z_][A-Za-z0-9_]{0,63}$/', $collection)) {
            throw new InputException('INPUT Collection must be a 1-64 character ASCII identifier.');
        }
        if ($key === '' || strlen($key) > 512 || strpos($key, "\0") !== false || !preg_match('//u', $key)) {
            throw new InputException('INPUT Key must contain 1-512 UTF-8 bytes, valid UTF-8, and no NUL.');
        }
        return $collection . "\0" . $key;
    }

    private static function encode($value): string
    {
        $json = json_encode($value, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRESERVE_ZERO_FRACTION | JSON_THROW_ON_ERROR);
        if (strlen($json) > 256 * 1024) {
            throw new LimitException('LIMIT A value may contain at most 256 KiB of JSON.');
        }
        return $json;
    }

    private static function decode(string $json)
    {
        return json_decode($json, true, 512, JSON_THROW_ON_ERROR | JSON_BIGINT_AS_STRING);
    }

    private static function u64(string $bytes, int $offset): int
    {
        return unpack('P', $bytes, $offset)[1];
    }

    private static function manyPayload(string $collection, array $keys): string
    {
        if (count($keys) > 4096) throw new InputException('INPUT getMany count');
        $payload = pack('V', count($keys));
        foreach ($keys as $key) {
            $id = self::id($collection, (string) $key);
            $payload .= pack('V', strlen($id)) . $id;
        }
        return $payload;
    }

    private static function manyReply(string $reply): array
    {
        $count = unpack('V', $reply, 1)[1];
        $p = 5;
        $values = [];
        for ($i = 0; $i < $count; $i++) {
            if ($reply[$p++] === "\x00") { $values[] = null; continue; }
            $version = self::u64($reply, $p);
            $length = unpack('V', $reply, $p + 8)[1];
            $p += 12;
            $values[] = ['version' => $version, 'value' => self::decode(substr($reply, $p, $length))];
            $p += $length;
        }
        return $values;
    }

    private static function batchPayload(array $operations): string
    {
        if (!$operations || count($operations) > 65536) throw new InputException('INPUT operation count');
        $payload = pack('V', count($operations));
        $seen = [];
        foreach ($operations as $op) {
            $type = $op['type'] ?? null;
            if ($type !== 'set' && $type !== 'delete') throw new InputException('INPUT operation type');
            $id = self::id((string) ($op['collection'] ?? ''), (string) ($op['key'] ?? ''));
            if (isset($seen[$id])) throw new InputException('INPUT duplicate transaction key');
            $seen[$id] = true;
            $value = $type === 'set' ? self::encode($op['value'] ?? null) : '';
            $expected = $op['expectedVersion'] ?? null;
            if ($expected !== null && (!is_int($expected) || $expected < 0)) throw new InputException('INPUT expectedVersion');
            $payload .= ($type === 'set' ? "\x01" : "\x00")
                . ($expected === null ? "\xff\xff\xff\xff\xff\xff\xff\xff" : pack('P', $expected))
                . pack('VV', strlen($id), strlen($value)) . $id . $value;
        }
        if (strlen($payload) > 64 * 1024 * 1024 - 64) throw new LimitException('LIMIT transaction bytes');
        return $payload;
    }

    /** @return int the new version */
    public function set(string $collection, string $key, $value, ?int $expectedVersion = null): int
    {
        return $this->batch([['type' => 'set', 'collection' => $collection, 'key' => $key, 'value' => $value, 'expectedVersion' => $expectedVersion]]);
    }

    /** @return int the new version */
    public function delete(string $collection, string $key, ?int $expectedVersion = null): int
    {
        return $this->batch([['type' => 'delete', 'collection' => $collection, 'key' => $key, 'expectedVersion' => $expectedVersion]]);
    }
}

final class Client
{
    use SqlMethods;
    use KeyValueCodec;

    private const MAX_FRAME = 64 * 1024 * 1024;
    /** @var resource|null */
    private $socket;
    private string $address;
    /** @var string[] a cluster's members (one address otherwise) */
    private array $addresses = [];
    private float $failoverTimeout = 15.0;
    private ?string $token;
    private float $timeout;
    private array $tls = [];
    private bool $persistent = false;
    private bool $reconnect = true;
    private bool $closed = false;
    private float $lastUse = 0.0;
    /** In-process reader (FFI handle) and reusable out-parameters. */
    private $reader = null;
    private $outVersion, $outValue, $outLength, $outError, $pVersion, $pValue, $pLength;

    /**
     * @param string $address 'tcp://127.0.0.1:7820', 'tls://db.example.com:7820' or 'unix:///run/kestrodb.sock'.
     *   A cluster: its members separated by commas. The client connects to any member; a member
     *   that follows answers a write with READONLY naming the leader (nothing was applied), and
     *   the client moves there and sends it again, also during an election, for up to
     *   'failoverTimeout' seconds (15).
     * @param string|null $token access token, when the server requires one
     * @param array{readFrom?:string, library?:string, tls?:array<string,mixed>, persistent?:bool, reconnect?:bool, failoverTimeout?:float} $options
     */
    public function __construct(string $address = 'tcp://127.0.0.1:7820', ?string $token = null, float $timeout = 5.0, array $options = [])
    {
        if (PHP_INT_SIZE !== 8) {
            throw new Exception('UNSUPPORTED KestroDB needs 64-bit PHP');
        }
        $this->addresses = array_values(array_filter(array_map('trim', explode(',', $address)), 'strlen'));
        if (!$this->addresses) throw new InputException('INPUT no server address');
        $this->address = $this->addresses[0];
        $this->failoverTimeout = (float) ($options['failoverTimeout'] ?? 15.0);
        $this->token = $token;
        $this->timeout = $timeout;
        $this->tls = $options['tls'] ?? [];
        $this->persistent = (bool) ($options['persistent'] ?? false);
        $this->reconnect = (bool) ($options['reconnect'] ?? true);
        if (isset($options['readFrom'])) {
            $this->openReader((string) $options['readFrom'], $options['library'] ?? null);
            return;  // connect on the first request that needs the server
        }
        $this->connect();
    }

    /** The member this connection uses now. */
    public function address(): string
    {
        return $this->address;
    }

    /** The current address first, then the other members. */
    private function connect(): void
    {
        $order = array_values(array_unique(array_merge([$this->address], $this->addresses)));
        foreach ($order as $i => $a) {
            $this->address = $a;
            try {
                $this->connectOne();
                return;
            } catch (Exception $e) {
                $this->drop();
                if ($e instanceof AuthException || $i === count($order) - 1) throw $e;
            }
        }
    }

    private function connectOne(): void
    {
        $ssl = ['verify_peer' => true, 'verify_peer_name' => true, 'crypto_method' => STREAM_CRYPTO_METHOD_TLSv1_2_CLIENT | (defined('STREAM_CRYPTO_METHOD_TLSv1_3_CLIENT') ? STREAM_CRYPTO_METHOD_TLSv1_3_CLIENT : 0)];
        foreach ($this->tls as $k => $v) $ssl[$k] = $v;
        $context = stream_context_create(['socket' => ['tcp_nodelay' => true], 'ssl' => $ssl]);
        $flags = STREAM_CLIENT_CONNECT | ($this->persistent ? STREAM_CLIENT_PERSISTENT : 0);
        $socket = @stream_socket_client($this->address, $errno, $error, $this->timeout, $flags, $context);
        if ($socket === false) {
            throw new ConnectionException("IO cannot connect to {$this->address}: $error");
        }
        stream_set_timeout($socket, (int) ceil(max($this->timeout, 30.0)));
        $this->socket = $socket;
        $this->lastUse = microtime(true);
        if ($this->persistent && !$this->usable()) {
            // A reused persistent connection with unread bytes (its last request died mid-reply).
            @fclose($socket);
            $socket = @stream_socket_client($this->address, $errno, $error, $this->timeout, STREAM_CLIENT_CONNECT, $context);
            if ($socket === false) throw new ConnectionException("IO cannot connect to {$this->address}: $error");
            $this->socket = $socket;
        }
        if ($this->token !== null) {
            $this->call('A', $this->token);
        }
        if ($this->persistent) {
            // The connection may come from an earlier request that left a transaction open.
            $this->call('E', pack('V', 8) . 'ROLLBACK[]');
        }
    }

    /** False when the server closed the connection, or bytes are pending outside a reply. */
    private function usable(): bool
    {
        stream_set_blocking($this->socket, false);
        $byte = @fread($this->socket, 1);
        $eof = feof($this->socket);
        stream_set_blocking($this->socket, true);
        return ($byte === '' || $byte === false) && !$eof;
    }

    public function __destruct()
    {
        $this->close();
    }

    /** @return array{version:int,value:mixed}|null */
    public function get(string $collection, string $key)
    {
        if ($this->reader !== null) {
            $f = Native::ffi();
            $id = self::id($collection, $key);
            $r = $f->kestro_get($this->reader, $id, strlen($id), $this->pVersion, $this->pValue, $this->pLength, $this->outError, 512);
            if ($r === 1) {
                return ['version' => $this->outVersion->cdata, 'value' => self::decode(\FFI::string($this->outValue, $this->outLength->cdata))];
            }
            if ($r === 0) return null;
            throw Exception::from(\FFI::string($this->outError));
        }
        $reply = $this->call('G', self::id($collection, $key));
        if ($reply[0] === "\x01") return null;
        return ['version' => self::u64($reply, 1), 'value' => self::decode(substr($reply, 9))];
    }

    /**
     * One consistent snapshot of up to 4096 keys, in order; null where missing.
     * @return array<int, array{version:int,value:mixed}|null>
     */
    public function getMany(string $collection, array $keys): array
    {
        $payload = self::manyPayload($collection, $keys);
        if ($this->reader !== null) {
            $f = Native::ffi();
            $reply = $f->new('const unsigned char*');
            if ($f->kestro_get_many($this->reader, $payload, strlen($payload), \FFI::addr($reply), $this->pLength, $this->outError, 512) !== 0) {
                throw Exception::from(\FFI::string($this->outError));
            }
            return self::manyReply(\FFI::string($reply, $this->outLength->cdata));
        }
        return self::manyReply($this->call('M', $payload));
    }

    /**
     * All-or-nothing: ['type' => 'set'|'delete', 'collection', 'key', 'value'?, 'expectedVersion'?]
     * @return int the version shared by every operation
     */
    public function batch(array $operations): int
    {
        return self::u64($this->call('B', self::batchPayload($operations)), 1);
    }

    public function sql(string $sql, array $params = []): array
    {
        $head = strtoupper(substr(ltrim($sql), 0, 8));
        try {
            $reply = $this->call('E', pack('V', strlen($sql)) . $sql . self::paramsJson($params));
        } catch (Exception $e) {
            // A COMMIT that fails (a conflict) has rolled the transaction back on the server.
            if (!($e instanceof ConnectionException) && (strncmp($head, 'COMMIT', 6) === 0 || strncmp($head, 'END', 3) === 0)) $this->inTransaction = false;
            throw $e;
        }
        $r = json_decode(substr($reply, 1), true, 512, JSON_THROW_ON_ERROR | JSON_BIGINT_AS_STRING);
        // Keep transaction state right when BEGIN/COMMIT/ROLLBACK arrive as plain SQL.
        if (strncmp($head, 'BEGIN', 5) === 0 || strncmp($head, 'START', 5) === 0) $this->inTransaction = true;
        elseif (strncmp($head, 'COMMIT', 6) === 0 || strncmp($head, 'ROLLBACK', 8) === 0 || strncmp($head, 'END', 3) === 0) $this->inTransaction = false;
        return $r;
    }

    /** @return array<string, mixed> */
    public function stats(): array
    {
        return json_decode(substr($this->call('S', ''), 1), true, 512, JSON_THROW_ON_ERROR);
    }

    public function verify(): void
    {
        $this->call('V', '');
    }

    /** Writes a verified copy (a path on the server's machine). */
    public function backup(string $path): void
    {
        $this->call('P', $path);
    }

    /** Writes a copy holding only current values (a path on the server's machine). */
    public function compactTo(string $path): void
    {
        $this->call('C', $path);
    }

    /** Turns a replica into a writable server (admin); it stops following its leader. */
    public function promote(): void
    {
        $this->call('X', '');
    }

    /**
     * Online compaction of the server's file (admin): reclaims the space of old versions
     * while the server keeps serving. Returns ['before' => bytes, 'after' => bytes, 'keys' => n].
     */
    public function compact(): array
    {
        return json_decode(substr($this->call('K', ''), 1), true, 512, JSON_THROW_ON_ERROR);
    }

    public function close(): void
    {
        if ($this->socket) {
            if (!$this->persistent) {
                try { $this->call('Q', ''); } catch (\Throwable $e) {}
                if ($this->socket) fclose($this->socket);
            } elseif ($this->inTransaction) {
                try { $this->sql('ROLLBACK'); } catch (\Throwable $e) {}
            }
            $this->socket = null;
        }
        $this->inTransaction = false;
        $this->reader = null;  // the process keeps the shared reader open for the next Client
        $this->closed = true;
    }

    private function openReader(string $path, ?string $library): void
    {
        $f = Native::ffi($library);
        $this->outError = $f->new('char[512]');
        $reader = $f->kestro_open($path, 0, $this->outError, 512);
        if ($reader === null) throw Exception::from(\FFI::string($this->outError));
        $this->reader = $reader;
        $this->outVersion = $f->new('uint64_t');
        $this->outValue = $f->new('const char*');
        $this->outLength = $f->new('size_t');
        $this->pVersion = \FFI::addr($this->outVersion);
        $this->pValue = \FFI::addr($this->outValue);
        $this->pLength = \FFI::addr($this->outLength);
    }

    /** Follows the cluster's leader (see __construct); one server: as callOnce. */
    private function call(string $command, string $payload): string
    {
        if (count($this->addresses) < 2 || $command === 'A' || $command === 'Q') return $this->callOnce($command, $payload);
        $deadline = microtime(true) + $this->failoverTimeout;
        for ($wait = 0.05;; $wait = min($wait * 2, 0.5)) {
            try {
                return $this->callOnce($command, $payload);
            } catch (Exception $e) {
                $m = $e->getMessage();
                $moved = strncmp($m, 'READONLY', 8) === 0;
                if ((!$moved && strncmp($m, 'IO cannot connect', 17) !== 0) || $this->inTransaction || microtime(true) > $deadline) throw $e;
                if (preg_match('/the leader is (\S+)/', $m, $named) && in_array($named[1], $this->addresses, true)) $target = $named[1];
                else {
                    usleep((int) ($wait * 1e6));
                    $target = $this->addresses[(array_search($this->address, $this->addresses, true) + 1) % count($this->addresses)];
                }
                $this->drop();
                $this->address = $target;
            }
        }
    }

    private function callOnce(string $command, string $payload): string
    {
        if ($this->closed) throw new ConnectionException('CLOSED the connection was closed');
        // A connection idle for a while may have been closed by the server (restart, idle
        // timeout): reconnect before sending, outside transactions only.
        if ($this->socket && $this->reconnect && !$this->inTransaction && $command !== 'A'
            && microtime(true) - $this->lastUse > 1.0 && !$this->usable()) {
            $this->drop();
        }
        if (!$this->socket) {
            if ($this->inTransaction) {
                $this->inTransaction = false;
                throw new ConnectionException('IO connection lost inside a transaction; it was rolled back');
            }
            if ($command === 'Q') return "\x00";
            $this->connect();
        }
        $frame = $command . $payload;
        $this->write(pack('V', strlen($frame)) . $frame);
        $length = unpack('V', $this->read(4))[1];
        if ($length < 1 || $length > self::MAX_FRAME) {
            $this->drop();
            throw new ConnectionException('IO invalid response length');
        }
        $reply = $this->read($length);
        $this->lastUse = microtime(true);
        if ($reply[0] === "\xff") throw Exception::from(substr($reply, 1));
        return $reply;
    }

    private function write(string $bytes): void
    {
        for ($done = 0, $n = strlen($bytes); $done < $n; $done += $put) {
            $put = @fwrite($this->socket, $done ? substr($bytes, $done) : $bytes);
            if ($put === false || $put === 0) {
                $this->drop();
                throw new ConnectionException('IO connection lost while sending; a write may have an unknown outcome');
            }
        }
    }

    private function read(int $n): string
    {
        $out = '';
        while (strlen($out) < $n) {
            $chunk = @fread($this->socket, $n - strlen($out));
            if ($chunk === false || $chunk === '') {
                $this->drop();
                $tx = $this->inTransaction; $this->inTransaction = false;
                throw new ConnectionException($tx ? 'IO connection lost inside a transaction; it was rolled back' : 'IO connection lost; a write without a reply has an unknown outcome');
            }
            $out .= $chunk;
        }
        return $out;
    }

    private function drop(): void
    {
        if ($this->socket) @fclose($this->socket);
        $this->socket = null;
    }
}

/**
 * A database file opened in this process through the C library (PHP FFI): no server.
 * One owner per file at a time (another process opening it gets LOCKED); use the server
 * when several processes write.
 */
final class Embedded
{
    use SqlMethods;
    use KeyValueCodec;

    private $db;
    private $err, $outVersion, $outValue, $outLength, $pVersion, $pValue, $pLength, $outText, $pText;

    public static function open(string $path, bool $create = false, ?string $library = null): self
    {
        return new self($path, $create, $library);
    }

    public function __construct(string $path, bool $create = false, ?string $library = null)
    {
        if (PHP_INT_SIZE !== 8) throw new Exception('UNSUPPORTED KestroDB needs 64-bit PHP');
        $f = Native::ffi($library);
        $this->err = $f->new('char[512]');
        $db = $f->kestro_open($path, $create ? 3 : 1, $this->err, 512);  // owner (1), plus create (2)
        if ($db === null) throw Exception::from(\FFI::string($this->err));
        $this->db = $db;
        $this->outVersion = $f->new('uint64_t');
        $this->outValue = $f->new('const char*');
        $this->outLength = $f->new('size_t');
        $this->outText = $f->new('const char*');
        $this->pVersion = \FFI::addr($this->outVersion);
        $this->pValue = \FFI::addr($this->outValue);
        $this->pLength = \FFI::addr($this->outLength);
        $this->pText = \FFI::addr($this->outText);
    }

    public function __destruct()
    {
        $this->close();
    }

    private function live()
    {
        if ($this->db === null) throw new ConnectionException('CLOSED the database was closed');
        return $this->db;
    }

    public function get(string $collection, string $key)
    {
        $id = self::id($collection, $key);
        $r = Native::ffi()->kestro_get($this->live(), $id, strlen($id), $this->pVersion, $this->pValue, $this->pLength, $this->err, 512);
        if ($r === 1) return ['version' => $this->outVersion->cdata, 'value' => self::decode(\FFI::string($this->outValue, $this->outLength->cdata))];
        if ($r === 0) return null;
        throw Exception::from(\FFI::string($this->err));
    }

    public function getMany(string $collection, array $keys): array
    {
        $payload = self::manyPayload($collection, $keys);
        $f = Native::ffi();
        $reply = $f->new('const unsigned char*');
        if ($f->kestro_get_many($this->live(), $payload, strlen($payload), \FFI::addr($reply), $this->pLength, $this->err, 512) !== 0) {
            throw Exception::from(\FFI::string($this->err));
        }
        return self::manyReply(\FFI::string($reply, $this->outLength->cdata));
    }

    public function batch(array $operations): int
    {
        $payload = self::batchPayload($operations);
        if (Native::ffi()->kestro_batch($this->live(), $payload, strlen($payload), $this->pVersion, $this->err, 512) !== 0) {
            throw Exception::from(\FFI::string($this->err));
        }
        return $this->outVersion->cdata;
    }

    public function sql(string $sql, array $params = []): array
    {
        $json = self::paramsJson($params);
        if (Native::ffi()->kestro_sql($this->live(), $sql, strlen($sql), $json, strlen($json), $this->pText, $this->pLength, $this->err, 512) !== 0) {
            throw Exception::from(\FFI::string($this->err));
        }
        $r = json_decode(\FFI::string($this->outText, $this->outLength->cdata), true, 512, JSON_THROW_ON_ERROR | JSON_BIGINT_AS_STRING);
        $head = strtoupper(substr(ltrim($sql), 0, 8));
        if (strncmp($head, 'BEGIN', 5) === 0 || strncmp($head, 'START', 5) === 0) $this->inTransaction = true;
        elseif (strncmp($head, 'COMMIT', 6) === 0 || strncmp($head, 'ROLLBACK', 8) === 0 || strncmp($head, 'END', 3) === 0) $this->inTransaction = false;
        return $r;
    }

    public function count(): int
    {
        return Native::ffi()->kestro_count($this->live());
    }

    /** Online compaction (see Client::compact()). */
    public function compact(): array
    {
        $f = Native::ffi(); $before = $f->new('uint64_t'); $after = $f->new('uint64_t');
        if ($f->kestro_compact($this->live(), \FFI::addr($before), \FFI::addr($after), $this->err, 512) !== 0) throw Exception::from(\FFI::string($this->err));
        return ['before' => $before->cdata, 'after' => $after->cdata];
    }

    /** Online backup to $path (a new file). Returns the transaction it holds. */
    public function backup(string $path): int
    {
        if (Native::ffi()->kestro_backup($this->live(), $path, $this->pVersion, $this->err, 512) !== 0) throw Exception::from(\FFI::string($this->err));
        return $this->outVersion->cdata;
    }

    /** Continuous archive in $dir (archive.kdb + timeline): appends what was committed since the last call. */
    public function archive(string $dir): int
    {
        if (Native::ffi()->kestro_archive($this->live(), $dir, $this->pVersion, $this->err, 512) !== 0) throw Exception::from(\FFI::string($this->err));
        return $this->outVersion->cdata;
    }

    /**
     * A new database at $dest from $source (a database, backup or archive.kdb), cut after
     * transaction $txid (0: everything), or at the last archived point at or before $at
     * (a DateTimeInterface or unix milliseconds). Returns the transaction restored.
     */
    public static function restore(string $source, string $dest, int $txid = 0, $at = null, ?string $library = null): int
    {
        $f = Native::ffi($library);
        $err = $f->new('char[512]'); $out = $f->new('uint64_t');
        $ms = $at === null ? -1 : ($at instanceof \DateTimeInterface ? (int) $at->format('Uv') : (int) $at);
        if ($f->kestro_restore($source, $dest, $txid, $ms, \FFI::addr($out), $err, 512) !== 0) throw Exception::from(\FFI::string($err));
        return $out->cdata;
    }

    public function close(): void
    {
        if ($this->db !== null) {
            Native::ffi()->kestro_close($this->db);  // an open transaction is rolled back
            $this->db = null;
        }
        $this->inTransaction = false;
    }
}

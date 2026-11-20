"""KestroDB client for Python 3.8+.

Server (many processes, many machines):

    import kestrodb
    db = kestrodb.connect("tcp://127.0.0.1:7820", token=os.environ.get("KESTRODB_TOKEN"))
    db.set("users", "kestro", {"name": "Kestro"})
    db.get("users", "kestro")        # {"version": 1, "value": {"name": "Kestro"}}
    db.close()

Fastest reads: on the server's machine, connect(..., read_from="/path/to/server.kdb") reads
the database file in this process through the KestroDB C library (ctypes), with no round
trip, and still sees every write acknowledged before the read. Writes go to the server,
which is then connected only when first needed.

Embedded (one process owns the file, like sqlite3): db = kestrodb.open("app.kdb", create=True)
has the same methods, all in-process.

SQL (both): db.query(sql, params) -> list of dicts, db.query_one, db.query_value,
db.execute(sql, params) -> rows changed (db.last_insert_id), "with db.transaction():"
(COMMIT, or ROLLBACK on an exception). Parameters: a list/tuple for ?, ?N and $N, or a
dict for :name and @name.

DB-API 2.0 (PEP 249): cur = db.cursor(); cur.execute(sql, params); cur.fetchone(),
fetchmany(), fetchall(), description, rowcount, lastrowid; db.commit()/db.rollback();
exceptions IntegrityError, ProgrammingError, OperationalError, ... (all KestroDBError).
Connections start in autocommit mode (each statement commits alone; KestroDB transactions
hold the write lock, so they are opened explicitly); set db.autocommit = False for the
PEP 249 behaviour where the first statement opens a transaction that commit() ends.

One connection per Client; use one per thread or process. Values are stored as JSON.
The C library is kestrodb.dll / libkestrodb.so / libkestrodb.dylib: pass library=PATH, set
KESTRODB_LIBRARY, or keep it in ../../bin relative to this file.
"""
import ctypes
import json
import os
import re
import socket
import struct
import sys
import time

__all__ = ["Client", "Local", "Cursor", "KestroDBError", "connect", "open", "restore",
           "Warning", "Error", "InterfaceError", "DatabaseError", "DataError", "OperationalError",
           "IntegrityError", "InternalError", "ProgrammingError", "NotSupportedError",
           "apilevel", "threadsafety", "paramstyle"]

apilevel = "2.0"
threadsafety = 1  # threads may share the module, not connections
paramstyle = "qmark"  # also numeric ($1, ?1) and named (:name, @name)

_ANY = b"\xff" * 8
_MAX_FRAME = 64 * 1024 * 1024
_COLLECTION = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,63}$")


class Warning(Exception):  # noqa: A001 - PEP 249 name
    pass


class KestroDBError(Exception):
    """kind is the leading error code: INPUT, LIMIT, CONFLICT, CORRUPT, AUTH, BUSY, IO, STALE, ..."""

    def __init__(self, message):
        super().__init__(message)
        self.kind = message.split(" ", 1)[0] if message else "ERROR"


# PEP 249 exception hierarchy; every class is a KestroDBError with .kind.
Error = KestroDBError


class InterfaceError(Error):
    pass


class DatabaseError(Error):
    pass


class DataError(DatabaseError):
    pass


class OperationalError(DatabaseError):
    pass


class IntegrityError(DatabaseError):
    pass


class InternalError(DatabaseError):
    pass


class ProgrammingError(DatabaseError):
    pass


class NotSupportedError(DatabaseError):
    pass


_KINDS = {"CONFLICT": IntegrityError, "INPUT": ProgrammingError, "LIMIT": DataError, "IO": OperationalError,
          "CLOSED": InterfaceError, "BUSY": OperationalError, "AUTH": OperationalError, "UNKNOWN_COMMIT": OperationalError,
          "CORRUPT": InternalError, "LOCKED": OperationalError, "UNSUPPORTED": NotSupportedError}


def _error(message):
    """The exception class for an error message from the engine or the server."""
    kind = message.split(" ", 1)[0] if message else "ERROR"
    return _KINDS.get(kind, DatabaseError)(message)


def _id(collection, key):
    if not isinstance(collection, str) or not _COLLECTION.match(collection):
        raise ProgrammingError("INPUT Collection must be a 1-64 character ASCII identifier.")
    if not isinstance(key, str):
        raise ProgrammingError("INPUT Key must be a string.")
    try:
        raw = key.encode("utf-8")
    except UnicodeEncodeError:
        raise ProgrammingError("INPUT Key must be valid Unicode.") from None
    if not raw or len(raw) > 512 or b"\0" in raw:
        raise ProgrammingError("INPUT Key must contain 1-512 UTF-8 bytes and no NUL.")
    return collection.encode("ascii") + b"\0" + raw


def _encode(value):
    try:
        text = json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
    except (TypeError, ValueError) as e:
        raise ProgrammingError("INPUT " + str(e)) from None
    raw = text.encode("utf-8")
    if len(raw) > 256 * 1024:
        raise DataError("LIMIT A value may contain at most 256 KiB of JSON.")
    return raw


def _many_payload(collection, keys):
    if len(keys) > 4096:
        raise ProgrammingError("INPUT getMany count")
    parts = [struct.pack("<I", len(keys))]
    for k in keys:
        i = _id(collection, k)
        parts += [struct.pack("<I", len(i)), i]
    return b"".join(parts)


def _parse_many(reply):
    (count,) = struct.unpack_from("<I", reply, 1)
    p, values = 5, []
    for _ in range(count):
        found = reply[p]
        p += 1
        if not found:
            values.append(None)
            continue
        version, length = struct.unpack_from("<QI", reply, p)
        p += 12
        values.append({"version": version, "value": json.loads(reply[p:p + length].decode("utf-8"))})
        p += length
    return values


def _batch_payload(operations):
    if not operations or len(operations) > 65536:
        raise ProgrammingError("INPUT operation count")
    parts, seen = [struct.pack("<I", len(operations))], set()
    for op in operations:
        kind = op.get("type")
        if kind not in ("set", "delete"):
            raise ProgrammingError("INPUT operation type")
        i = _id(op.get("collection"), op.get("key"))
        if i in seen:
            raise ProgrammingError("INPUT duplicate transaction key")
        seen.add(i)
        value = _encode(op.get("value")) if kind == "set" else b""
        expected = op.get("expectedVersion")
        if expected is not None and (not isinstance(expected, int) or expected < 0):
            raise ProgrammingError("INPUT expectedVersion")
        parts += [b"\x01" if kind == "set" else b"\x00", _ANY if expected is None else struct.pack("<Q", expected),
                  struct.pack("<II", len(i), len(value)), i, value]
    payload = b"".join(parts)
    if len(payload) > _MAX_FRAME - 64:
        raise DataError("LIMIT transaction bytes")
    return payload


def _sql_params(params):
    """A list/tuple for ?, ?N and $N; a dict (keys with or without : or @) for :name."""
    try:
        if params is None:
            return b"[]"
        if isinstance(params, dict):
            value = {str(k).lstrip(":@"): v for k, v in params.items()}
        elif isinstance(params, (str, bytes)):
            raise TypeError("parameters must be a list, a tuple or a dict")
        else:
            value = list(params)
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")
    except (TypeError, ValueError) as e:
        raise ProgrammingError("INPUT SQL parameters: " + str(e)) from None


class Cursor:
    """PEP 249 cursor. Rows are tuples in SELECT order (description names them)."""

    arraysize = 1

    def __init__(self, connection):
        self.connection = connection
        self.description = None
        self.rowcount = -1
        self.lastrowid = None
        self._rows, self._pos = [], 0

    def _check(self):
        if self.connection is None:
            raise InterfaceError("CLOSED the cursor was closed")

    def execute(self, operation, parameters=()):
        self._check()
        conn = self.connection
        conn._implicit_begin(operation)
        result = conn.sql(operation, parameters)
        self._pos = 0
        if "rows" in result:
            rows = result["rows"]
            names = list(rows[0].keys()) if rows else []
            self.description = tuple((n, None, None, None, None, None, None) for n in names) if rows else None
            self._rows = [tuple(r.values()) for r in rows]
            self.rowcount = len(rows)
        else:
            self.description, self._rows = None, []
            self.rowcount = result.get("changes", -1)
            if "lastInsertId" in result:
                self.lastrowid = result["lastInsertId"]
        return self

    def executemany(self, operation, seq_of_parameters):
        """Runs the statement once per parameter set, all in one transaction (one commit)."""
        self._check()
        conn, total, own = self.connection, 0, False
        if not conn.in_transaction:
            conn.begin()
            own = True
        try:
            for params in seq_of_parameters:
                self.execute(operation, params)
                total += max(self.rowcount, 0)
        except BaseException:
            if own and conn.in_transaction:
                conn.rollback()
            raise
        if own:
            conn.commit()
        self.rowcount = total
        return self

    def fetchone(self):
        self._check()
        if self._pos >= len(self._rows):
            return None
        row = self._rows[self._pos]
        self._pos += 1
        return row

    def fetchmany(self, size=None):
        self._check()
        n = self.arraysize if size is None else size
        rows = self._rows[self._pos:self._pos + n]
        self._pos += len(rows)
        return rows

    def fetchall(self):
        self._check()
        rows = self._rows[self._pos:]
        self._pos = len(self._rows)
        return rows

    def __iter__(self):
        while True:
            row = self.fetchone()
            if row is None:
                return
            yield row

    def close(self):
        self.connection, self._rows = None, []

    def setinputsizes(self, sizes):
        pass

    def setoutputsize(self, size, column=None):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class _Transaction:
    def __init__(self, db):
        self.db = db

    def __enter__(self):
        self.db.begin()
        return self.db

    def __exit__(self, kind, value, tb):
        if not self.db.in_transaction:
            return False
        if kind is None:
            self.db.commit()
        else:
            try:
                self.db.rollback()
            except Exception:
                pass
        return False


class _SqlApi:
    """SQL conveniences and PEP 249 connection methods, on Client and Local."""

    in_transaction = False
    autocommit = True
    last_insert_id = None

    def _track(self, text, result):
        head = text.lstrip()[:8].upper()
        if head.startswith(("BEGIN", "START")):
            self.in_transaction = True
        elif head.startswith(("COMMIT", "ROLLBACK", "END")):
            self.in_transaction = False
        if isinstance(result, dict) and "lastInsertId" in result:
            self.last_insert_id = result["lastInsertId"]
        return result

    def _implicit_begin(self, text):
        if not self.autocommit and not self.in_transaction:
            head = text.lstrip()[:8].upper()
            if not head.startswith(("BEGIN", "START", "COMMIT", "ROLLBACK", "END", "CREATE", "DROP", "ALTER")):
                self.begin()

    def query(self, text, params=()):
        """All rows of a SELECT, as dicts."""
        r = self.sql(text, params)
        if "rows" not in r:
            raise ProgrammingError("INPUT query() needs a statement that returns rows; use execute()")
        return r["rows"]

    def query_one(self, text, params=()):
        rows = self.query(text, params)
        return rows[0] if rows else None

    def query_value(self, text, params=()):
        row = self.query_one(text, params)
        return next(iter(row.values()), None) if row else None

    def execute(self, text, params=()):
        """INSERT, UPDATE, DELETE or DDL; returns the number of rows changed."""
        return self.sql(text, params).get("changes", 0)

    def begin(self):
        if self.in_transaction:
            raise ProgrammingError("INPUT a transaction is already open")
        self.sql("BEGIN")

    def commit(self):
        """Commits an open transaction (PEP 249: nothing to do without one)."""
        if self.in_transaction:
            self.sql("COMMIT")

    def rollback(self):
        if self.in_transaction:
            self.sql("ROLLBACK")

    def transaction(self):
        """with db.transaction(): ...  commits, or rolls back if the block raises."""
        return _Transaction(self)

    def cursor(self):
        return Cursor(self)


_lib = None


def _library(path=None):
    global _lib
    if _lib is None:
        if path is None:
            # KESTRODB_LIBRARY, else the library a release wheel bundles (kestrodb_native/ beside
            # this module), else a repository checkout's or release archive's bin/.
            name = {"win32": "kestrodb.dll", "darwin": "libkestrodb.dylib"}.get(sys.platform, "libkestrodb.so")
            here = os.path.dirname(os.path.abspath(__file__))
            bundled = os.path.join(here, "kestrodb_native", name)
            path = os.environ.get("KESTRODB_LIBRARY") or (bundled if os.path.exists(bundled) else os.path.join(here, "..", "..", "bin", name))
        c = ctypes
        lib = c.CDLL(os.path.abspath(path))
        lib.kestro_open.restype = c.c_void_p
        lib.kestro_open.argtypes = [c.c_char_p, c.c_int, c.c_char_p, c.c_size_t]
        lib.kestro_get.restype = c.c_int
        lib.kestro_get.argtypes = [c.c_void_p, c.c_char_p, c.c_size_t, c.POINTER(c.c_uint64), c.POINTER(c.c_void_p), c.POINTER(c.c_size_t), c.c_char_p, c.c_size_t]
        lib.kestro_get_many.restype = c.c_int
        lib.kestro_get_many.argtypes = [c.c_void_p, c.c_char_p, c.c_size_t, c.POINTER(c.c_void_p), c.POINTER(c.c_size_t), c.c_char_p, c.c_size_t]
        lib.kestro_batch.restype = c.c_int
        lib.kestro_batch.argtypes = [c.c_void_p, c.c_char_p, c.c_size_t, c.POINTER(c.c_uint64), c.c_char_p, c.c_size_t]
        lib.kestro_count.restype = c.c_int64
        lib.kestro_count.argtypes = [c.c_void_p]
        lib.kestro_sql.restype = c.c_int
        lib.kestro_sql.argtypes = [c.c_void_p, c.c_char_p, c.c_size_t, c.c_char_p, c.c_size_t, c.POINTER(c.c_void_p), c.POINTER(c.c_size_t), c.c_char_p, c.c_size_t]
        lib.kestro_compact.restype = c.c_int
        lib.kestro_compact.argtypes = [c.c_void_p, c.POINTER(c.c_uint64), c.POINTER(c.c_uint64), c.c_char_p, c.c_size_t]
        lib.kestro_backup.restype = c.c_int
        lib.kestro_backup.argtypes = [c.c_void_p, c.c_char_p, c.POINTER(c.c_uint64), c.c_char_p, c.c_size_t]
        lib.kestro_archive.restype = c.c_int
        lib.kestro_archive.argtypes = [c.c_void_p, c.c_char_p, c.POINTER(c.c_uint64), c.c_char_p, c.c_size_t]
        lib.kestro_restore.restype = c.c_int
        lib.kestro_restore.argtypes = [c.c_char_p, c.c_char_p, c.c_uint64, c.c_int64, c.POINTER(c.c_uint64), c.c_char_p, c.c_size_t]
        lib.kestro_close.restype = None
        lib.kestro_close.argtypes = [c.c_void_p]
        _lib = lib
    return _lib


class Local(_SqlApi):
    """A database read (and, as owner, written) in this process through the C library.

    owner=True: this process is the only writer (embedded). owner=False: read alongside
    kestro-server or another owner; any number of processes can do this at once."""

    def __init__(self, path, create=False, owner=True, library=None):
        lib = _library(library)
        self._lib, self._owner, self._h = lib, owner, None
        self._err = ctypes.create_string_buffer(512)
        mode = (3 if create else 1) if owner else 0
        handle = lib.kestro_open(os.fsencode(os.fspath(path)), mode, self._err, 512)
        if not handle:
            self._fail()
        self._h = handle
        self._ver, self._ptr, self._len = ctypes.c_uint64(), ctypes.c_void_p(), ctypes.c_size_t()
        self._pver, self._pptr, self._plen = ctypes.byref(self._ver), ctypes.byref(self._ptr), ctypes.byref(self._len)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _fail(self):
        raise _error(self._err.value.decode("utf-8", "replace"))

    def _handle(self):
        if self._h is None:
            raise InterfaceError("CLOSED the database was closed")
        return self._h

    def get(self, collection, key):
        i = _id(collection, key)
        r = self._lib.kestro_get(self._handle(), i, len(i), self._pver, self._pptr, self._plen, self._err, 512)
        if r == 1:
            return {"version": self._ver.value, "value": json.loads(ctypes.string_at(self._ptr.value, self._len.value))}
        if r == 0:
            return None
        self._fail()

    def get_many(self, collection, keys):
        """One consistent snapshot of up to 4096 keys, in order; None where missing."""
        payload = _many_payload(collection, keys)
        if self._lib.kestro_get_many(self._handle(), payload, len(payload), self._pptr, self._plen, self._err, 512):
            self._fail()
        return _parse_many(ctypes.string_at(self._ptr.value, self._len.value))

    def set(self, collection, key, value, expected_version=None):
        """Returns the new version. expected_version makes it conditional (0 = must not exist)."""
        return self.batch([{"type": "set", "collection": collection, "key": key, "value": value, "expectedVersion": expected_version}])

    def delete(self, collection, key, expected_version=None):
        return self.batch([{"type": "delete", "collection": collection, "key": key, "expectedVersion": expected_version}])

    def batch(self, operations):
        """All-or-nothing and durable when it returns (owner handles only)."""
        payload = _batch_payload(operations)
        if self._lib.kestro_batch(self._handle(), payload, len(payload), self._pver, self._err, 512):
            self._fail()
        return self._ver.value

    def count(self):
        return self._lib.kestro_count(self._handle())

    def compact(self):
        """Online compaction: reclaims the space of old versions while reads and writes
        continue. Returns {"before": bytes, "after": bytes}."""
        before, after = ctypes.c_uint64(), ctypes.c_uint64()
        if self._lib.kestro_compact(self._handle(), ctypes.byref(before), ctypes.byref(after), self._err, 512):
            self._fail()
        return {"before": before.value, "after": after.value}

    def backup(self, path):
        """Online backup to path (a new file); returns the transaction it holds."""
        if self._lib.kestro_backup(self._handle(), os.fsencode(os.fspath(path)), self._pver, self._err, 512):
            self._fail()
        return self._ver.value

    def archive(self, directory):
        """Continuous archive in directory (archive.kdb + timeline): appends what was
        committed since the last call. Call it on a timer."""
        if self._lib.kestro_archive(self._handle(), os.fsencode(os.fspath(directory)), self._pver, self._err, 512):
            self._fail()
        return self._ver.value

    def sql(self, text, params=()):
        """One SQL statement in this process (owner handles); see SQL.md."""
        t, p = text.encode("utf-8"), _sql_params(params)
        if self._lib.kestro_sql(self._handle(), t, len(t), p, len(p), self._pptr, self._plen, self._err, 512):
            self._fail()
        return self._track(text, json.loads(ctypes.string_at(self._ptr.value, self._len.value)))

    def close(self):
        # Reader handles are shared by the whole process and stay open for the next user.
        if self._h is not None and self._owner:
            self._lib.kestro_close(self._h)  # an open transaction is rolled back
        self._h = None
        self.in_transaction = False


def restore(source, dest, txid=0, at=None, library=None):
    """A new database at dest from source (a database, backup or archive.kdb), cut after
    transaction txid (0: everything), or at the last archived point at or before at (a
    datetime or unix milliseconds). Returns the transaction restored."""
    lib = _library(library)
    err, out = ctypes.create_string_buffer(512), ctypes.c_uint64()
    if at is None:
        ms = -1
    elif hasattr(at, "timestamp"):
        ms = int(at.timestamp() * 1000)
    else:
        ms = int(at)
    if lib.kestro_restore(os.fsencode(os.fspath(source)), os.fsencode(os.fspath(dest)), txid, ms, ctypes.byref(out), err, 512):
        raise _error(err.value.decode("utf-8", "replace"))
    return out.value


def open(path, create=False, library=None):  # noqa: A001 - mirrors sqlite3.connect
    """Embedded database owned by this process."""
    return Local(path, create=create, owner=True, library=library)


class Client(_SqlApi):
    def __init__(self, address="tcp://127.0.0.1:7820", token=None, timeout=30.0, read_from=None, library=None, tls=None, reconnect=True, failover_timeout=15.0):
        """tls://host:port uses TLS. The server certificate is verified against the system
        CAs, or tls={"cafile": path}; tls={"verify": False} skips verification (testing only).
        A client certificate (a server with --tls-client-ca): tls={"certfile": path,
        "keyfile": path, "password": optional}.
        reconnect: reopen a connection the server closed while it was idle (never inside a
        transaction, never for a request already sent).
        A cluster: several addresses (a list, or separated by commas). The client connects to
        any member; a member that follows answers a write with READONLY naming the leader
        (nothing was applied), and the client moves there and sends it again, also during an
        election, for up to failover_timeout seconds."""
        self._tls = tls or {}
        self._sock, self._local = None, None
        addresses = address if isinstance(address, (list, tuple)) else str(address).split(",")
        self._addresses = [a.strip() for a in addresses if a.strip()]
        if not self._addresses:
            raise InterfaceError("INPUT no server address")
        self._failover_timeout = failover_timeout
        self._address, self._token, self._timeout = self._addresses[0], token, timeout
        self._reconnect, self._closed, self._last_use = reconnect, False, 0.0
        if read_from is not None:
            self._local = Local(read_from, owner=False, library=library)
            return  # connect on the first write
        self._connect()

    @property
    def address(self):
        """The member this connection uses now."""
        return self._address

    def _connect(self):
        """The current address first, then the other members."""
        order = [self._address] + [a for a in self._addresses if a != self._address]
        for i, a in enumerate(order):
            try:
                self._connect_one(a)
                self._address = a
                return
            except (OSError, KestroDBError) as e:
                if self._sock is not None:
                    self._sock.close()
                    self._sock = None
                if (isinstance(e, KestroDBError) and e.kind == "AUTH") or i == len(order) - 1:
                    raise

    def _connect_one(self, address):
        token, timeout = self._token, self._timeout
        if address.startswith("unix://"):
            self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._sock.settimeout(timeout)
            self._sock.connect(address[7:])
        else:
            host_port = address[6:] if address.startswith(("tcp://", "tls://")) else address
            host, _, port = host_port.rpartition(":")
            host = host.strip("[]") or "127.0.0.1"
            self._sock = socket.create_connection((host, int(port)), timeout=timeout)
            if address.startswith("tls://"):
                import ssl
                ctx = ssl.create_default_context(cafile=self._tls.get("cafile"))
                ctx.minimum_version = ssl.TLSVersion.TLSv1_2
                if self._tls.get("verify") is False:
                    ctx.check_hostname = False
                    ctx.verify_mode = ssl.CERT_NONE
                if self._tls.get("certfile"):
                    ctx.load_cert_chain(self._tls["certfile"], self._tls.get("keyfile"), self._tls.get("password"))
                self._sock = ctx.wrap_socket(self._sock, server_hostname=self._tls.get("server_hostname", host))
            self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._last_use = time.monotonic()
        if token is not None:
            self._call(b"A", token.encode("utf-8"))

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def get(self, collection, key):
        if self._local is not None:
            return self._local.get(collection, key)
        reply = self._call(b"G", _id(collection, key))
        if reply[0] == 1:
            return None
        return {"version": struct.unpack_from("<Q", reply, 1)[0], "value": json.loads(reply[9:].decode("utf-8"))}

    def get_many(self, collection, keys):
        """One consistent snapshot of up to 4096 keys, in order; None where missing."""
        if self._local is not None:
            return self._local.get_many(collection, keys)
        return _parse_many(self._call(b"M", _many_payload(collection, keys)))

    def set(self, collection, key, value, expected_version=None):
        """Returns the new version. expected_version makes it conditional (0 = must not exist)."""
        return self.batch([{"type": "set", "collection": collection, "key": key, "value": value, "expectedVersion": expected_version}])

    def delete(self, collection, key, expected_version=None):
        return self.batch([{"type": "delete", "collection": collection, "key": key, "expectedVersion": expected_version}])

    def batch(self, operations):
        """All-or-nothing; returns the version shared by every operation."""
        return struct.unpack_from("<Q", self._call(b"B", _batch_payload(operations)), 1)[0]

    def sql(self, text, params=()):
        """One SQL statement on the server (see SQL.md); params fill the ? placeholders.
        SELECT returns {"rows": [...]}; writes return {"changes": n, "lastInsertId": id}."""
        t = text.encode("utf-8")
        try:
            reply = self._call(b"E", struct.pack("<I", len(t)) + t + _sql_params(params))
        except KestroDBError as e:
            # A COMMIT that fails (a conflict) has rolled the transaction back on the server.
            if e.kind != "IO" and re.match(r"\s*(COMMIT|END)\b", text, re.I):
                self.in_transaction = False
            raise
        return self._track(text, json.loads(reply[1:].decode("utf-8")))

    def stats(self):
        return json.loads(self._call(b"S", b"")[1:].decode("utf-8"))

    def verify(self):
        self._call(b"V", b"")

    def backup(self, path):
        self._call(b"P", path.encode("utf-8"))

    def compact_to(self, path):
        self._call(b"C", path.encode("utf-8"))

    def promote(self):
        """Turns a replica into a writable server (admin); it stops following its leader."""
        self._call(b"X", b"")

    def compact(self):
        """Online compaction of the server's file (admin): {"before", "after", "keys"}."""
        return json.loads(self._call(b"K", b"")[1:].decode("utf-8"))

    def close(self):
        if self._sock is not None:
            try:
                self._call(b"Q", b"")
            except Exception:
                pass
            if self._sock is not None:
                self._sock.close()
            self._sock = None
        self._local, self._closed, self.in_transaction = None, True, False

    def _alive(self):
        """False when the server closed this idle connection (or sent bytes outside a reply)."""
        if hasattr(self._sock, "pending"):
            return True  # TLS: no raw peek; a closed connection shows up as an IO error instead
        try:
            self._sock.setblocking(False)
            try:
                return self._sock.recv(1, socket.MSG_PEEK) is None
            finally:
                self._sock.settimeout(self._timeout)
        except (BlockingIOError, InterruptedError):
            return True
        except (OSError, ValueError):
            return False

    def _call(self, command, payload):
        """Follows the cluster's leader (see __init__); one server: as _call_once."""
        if len(self._addresses) < 2 or command in (b"A", b"Q"):
            return self._call_once(command, payload)
        deadline, wait = time.monotonic() + self._failover_timeout, 0.05
        while True:
            try:
                return self._call_once(command, payload)
            except KestroDBError as e:
                message = str(e)
                if (e.kind != "READONLY" and not message.startswith("IO cannot connect")) or self.in_transaction or time.monotonic() > deadline:
                    raise
                named = re.search(r"the leader is (\S+)", message)
                if named and named.group(1) in self._addresses:
                    target = named.group(1)
                else:
                    time.sleep(wait)
                    wait = min(wait * 2, 0.5)
                    target = self._addresses[(self._addresses.index(self._address) + 1) % len(self._addresses)]
                if self._sock is not None:
                    self._sock.close()
                    self._sock = None
                self._address = target
            except OSError as e:
                if time.monotonic() > deadline:
                    raise OperationalError("IO cannot connect to any cluster member: %s" % e) from e
                time.sleep(wait)
                wait = min(wait * 2, 0.5)

    def _call_once(self, command, payload):
        if self._closed:
            raise InterfaceError("CLOSED the connection was closed")
        if (self._sock is not None and self._reconnect and not self.in_transaction and command != b"A"
                and time.monotonic() - self._last_use > 1.0 and not self._alive()):
            self._sock.close()
            self._sock = None
        if self._sock is None:
            if self.in_transaction:
                self.in_transaction = False
                raise OperationalError("IO connection lost inside a transaction; it was rolled back")
            if command == b"Q":
                return b"\x00"
            self._connect()
        frame = command + payload
        try:
            self._sock.sendall(struct.pack("<I", len(frame)) + frame)
            (length,) = struct.unpack("<I", self._read(4))
            if not 0 < length <= _MAX_FRAME:
                raise OperationalError("IO invalid response length")
            reply = self._read(length)
        except (OSError, KestroDBError) as e:
            if isinstance(e, KestroDBError) and e.kind != "IO":
                raise
            self._sock.close()
            self._sock = None
            tx, self.in_transaction = self.in_transaction, False
            raise OperationalError("IO connection lost inside a transaction; it was rolled back" if tx else
                                   "IO connection lost; a write without a reply has an unknown outcome") from e
        self._last_use = time.monotonic()
        if reply[0] == 255:
            raise _error(reply[1:].decode("utf-8", "replace"))
        return reply

    def _read(self, n):
        chunks, got = [], 0
        while got < n:
            chunk = self._sock.recv(n - got)
            if not chunk:
                raise OSError("connection closed")
            chunks.append(chunk)
            got += len(chunk)
        return b"".join(chunks)


def connect(address="tcp://127.0.0.1:7820", token=None, timeout=30.0, read_from=None, library=None, tls=None, reconnect=True, failover_timeout=15.0):
    """A connection to kestro-server (PEP 249 connect); address may list a cluster's members."""
    return Client(address, token, timeout, read_from, library, tls, reconnect, failover_timeout)

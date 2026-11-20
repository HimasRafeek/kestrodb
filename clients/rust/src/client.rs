//! The connection to kestro-server.

use crate::conn::{self, Conn, Idle, Stream};
use crate::error::{Error, Result};
use crate::params::Params;
use crate::types::{Op, Row, SqlResult, Versioned};
use crate::wire::{self, Address, TxState};
use serde::de::DeserializeOwned;
use serde::Serialize;
use serde_json::{Map, Value};
use std::fmt;
use std::panic::{self, AssertUnwindSafe};
use std::path::PathBuf;
use std::time::{Duration, Instant};

/// Connection settings. `Options::default()` (or [`Options::new`]) is: no token, 30 s
/// timeout, reconnect on, 15 s failover timeout, TLS verified against the system's CAs.
#[derive(Clone, Debug)]
pub struct Options {
    /// The access token (`--token-file` / `--users` on the server); `None` for none.
    pub token: Option<String>,
    /// Timeout for every reply, and for connecting unless `connect_timeout` is set
    /// (default 30 s; `Duration::ZERO`: none).
    pub timeout: Duration,
    /// Time allowed to open the TCP connection (default: `timeout`).
    pub connect_timeout: Option<Duration>,
    /// Reopen a connection the server closed while idle (default true). Never inside a
    /// transaction, never for a request already sent.
    pub reconnect: bool,
    /// How long a cluster client keeps looking for the leader (default 15 s).
    pub failover_timeout: Duration,
    /// tls://: trust only the CA certificates in this PEM file (default: the system's CAs).
    pub tls_ca_file: Option<PathBuf>,
    /// tls://: false skips certificate and host name checks (testing only; default true).
    pub tls_verify: bool,
    /// tls://: the name to send (SNI) and check the certificate against, instead of the
    /// address's host.
    pub tls_server_name: Option<String>,
    /// tls://: your own rustls configuration (client certificates, roots), instead of
    /// `tls_ca_file` / `tls_verify`.
    #[cfg(feature = "tls")]
    pub tls_config: Option<std::sync::Arc<rustls::ClientConfig>>,
}

impl Default for Options {
    fn default() -> Options {
        Options {
            token: None,
            timeout: Duration::from_secs(30),
            connect_timeout: None,
            reconnect: true,
            failover_timeout: Duration::from_secs(15),
            tls_ca_file: None,
            tls_verify: true,
            tls_server_name: None,
            #[cfg(feature = "tls")]
            tls_config: None,
        }
    }
}

impl Options {
    /// The defaults.
    pub fn new() -> Options {
        Options::default()
    }

    /// The access token.
    pub fn token(mut self, token: impl Into<String>) -> Options {
        self.token = Some(token.into());
        self
    }

    /// Reply (and connect) timeout; `Duration::ZERO` for none.
    pub fn timeout(mut self, timeout: Duration) -> Options {
        self.timeout = timeout;
        self
    }

    /// Time allowed to open the TCP connection.
    pub fn connect_timeout(mut self, timeout: Duration) -> Options {
        self.connect_timeout = Some(timeout);
        self
    }

    /// Reopen a connection the server closed while idle (default true).
    pub fn reconnect(mut self, on: bool) -> Options {
        self.reconnect = on;
        self
    }

    /// How long a cluster client keeps looking for the leader (default 15 s).
    pub fn failover_timeout(mut self, timeout: Duration) -> Options {
        self.failover_timeout = timeout;
        self
    }

    /// tls://: trust only the CA certificates in this PEM file.
    pub fn tls_ca_file(mut self, path: impl Into<PathBuf>) -> Options {
        self.tls_ca_file = Some(path.into());
        self
    }

    /// tls://: false skips certificate and host name checks (testing only).
    pub fn tls_verify(mut self, on: bool) -> Options {
        self.tls_verify = on;
        self
    }

    /// tls://: the name to check the certificate against.
    pub fn tls_server_name(mut self, name: impl Into<String>) -> Options {
        self.tls_server_name = Some(name.into());
        self
    }

    /// tls://: your own rustls configuration.
    #[cfg(feature = "tls")]
    pub fn tls_config(mut self, config: std::sync::Arc<rustls::ClientConfig>) -> Options {
        self.tls_config = Some(config);
        self
    }

    fn io_timeout(&self) -> Option<Duration> {
        Some(self.timeout).filter(|t| !t.is_zero())
    }
}

/// One server address, or a cluster's members: a `&str` (several separated by commas), a
/// `String`, or a slice, array or `Vec` of them.
pub trait IntoAddresses {
    /// The addresses, untrimmed.
    fn into_addresses(self) -> Vec<String>;
}

impl IntoAddresses for &str {
    fn into_addresses(self) -> Vec<String> {
        self.split(',').map(str::to_string).collect()
    }
}

impl IntoAddresses for String {
    fn into_addresses(self) -> Vec<String> {
        self.as_str().into_addresses()
    }
}

impl IntoAddresses for &String {
    fn into_addresses(self) -> Vec<String> {
        self.as_str().into_addresses()
    }
}

impl<S: AsRef<str>> IntoAddresses for &[S] {
    fn into_addresses(self) -> Vec<String> {
        self.iter().map(|s| s.as_ref().to_string()).collect()
    }
}

impl<S: AsRef<str>> IntoAddresses for Vec<S> {
    fn into_addresses(self) -> Vec<String> {
        self.as_slice().into_addresses()
    }
}

impl<S: AsRef<str>, const N: usize> IntoAddresses for [S; N] {
    fn into_addresses(self) -> Vec<String> {
        self.as_slice().into_addresses()
    }
}

/// A connection to kestro-server.
///
/// One connection, used by one thread at a time (`Client` is `Send`; share it behind a
/// `Mutex`, or better open one per thread, since a transaction belongs to its connection).
/// Dropping it closes the connection (an open transaction is rolled back by the server).
///
/// **Reconnect.** A connection the server closed while idle (a restart, the network) is
/// reopened before the next request, except inside a transaction: that call fails with
/// `IO connection lost inside a transaction; it was rolled back`, and the one after
/// reconnects. A request already sent when the connection broke fails with [`Error::Io`]
/// (for a write: unknown outcome).
///
/// **Clusters.** Given several members, the client connects to any of them. A follower
/// answers a write with `READONLY` naming the leader (nothing was applied); the client moves
/// there and sends the request again, also during an election, for up to
/// [`Options::failover_timeout`]. Never inside a transaction: its connection went with its
/// leader.
pub struct Client {
    addresses: Vec<Address>,
    current: usize,
    options: Options,
    conn: Option<Conn>,
    closed: bool,
    lost: bool,
    used: bool,
    last_use: Instant,
    tx: TxState,
    last_insert_id: Option<Value>,
    opens: u64,
    frame: Vec<u8>,
    #[cfg(feature = "tls")]
    tls: Option<std::sync::Arc<rustls::ClientConfig>>,
}

impl fmt::Debug for Client {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Client")
            .field("address", &self.address())
            .field("connected", &self.conn.is_some())
            .field("in_transaction", &self.tx.open)
            .field("closed", &self.closed)
            .finish()
    }
}

const IDLE_CHECK: Duration = Duration::from_secs(1);
const NO_BYTES: &[u8] = &[];

impl Client {
    /// Connects to `addresses`: `tcp://host:port`, `tls://host:port`, `unix:///path` or
    /// `host:port`; several (comma-separated, or a list) are a cluster's members, tried in
    /// order. Logs in with `options.token`.
    pub fn connect(addresses: impl IntoAddresses, options: Options) -> Result<Client> {
        let mut parsed = Vec::new();
        for a in addresses.into_addresses() {
            let t = a.trim();
            if !t.is_empty() {
                parsed.push(Address::parse(t)?);
            }
        }
        if parsed.is_empty() {
            return Err(Error::Input("INPUT no server address".into()));
        }
        #[cfg(not(feature = "tls"))]
        if let Some(a) = parsed.iter().find(|a| a.tls) {
            return Err(Error::Input(format!(
                "INPUT {} needs TLS: build the kestrodb crate with its \"tls\" feature",
                a.text
            )));
        }
        let mut client = Client {
            addresses: parsed,
            current: 0,
            options,
            conn: None,
            closed: false,
            lost: false,
            used: false,
            last_use: Instant::now(),
            tx: TxState::default(),
            last_insert_id: None,
            opens: 0,
            frame: Vec::new(),
            #[cfg(feature = "tls")]
            tls: None,
        };
        client.open()?;
        Ok(client)
    }

    /// The member this client uses now, as written in the address list.
    pub fn address(&self) -> &str {
        &self.addresses[self.current].text
    }

    /// True between BEGIN (or START TRANSACTION, or a SAVEPOINT outside one) and its
    /// COMMIT/ROLLBACK.
    pub fn in_transaction(&self) -> bool {
        self.tx.open
    }

    /// The id of the last row an INSERT on this client created (an integer, or a string for
    /// text ids).
    pub fn last_insert_id(&self) -> Option<&Value> {
        self.last_insert_id.as_ref()
    }

    /// True after [`Client::close`].
    pub fn is_closed(&self) -> bool {
        self.closed
    }

    /// How many connections this client has opened (1 + reconnects + cluster moves).
    pub fn connections_opened(&self) -> u64 {
        self.opens
    }

    // ------------------------------------------------------------------ connecting

    /// The current member first, then the others; a wrong token stops at once.
    fn open(&mut self) -> Result<()> {
        let n = self.addresses.len();
        let mut last = None;
        for i in 0..n {
            let index = (self.current + i) % n;
            match self.open_one(index) {
                Ok(c) => {
                    self.conn = Some(c);
                    self.current = index;
                    self.lost = false;
                    self.used = false;
                    self.opens += 1;
                    self.last_use = Instant::now();
                    return Ok(());
                }
                Err(e @ Error::Auth(_)) => return Err(e),
                Err(e) => last = Some(e),
            }
        }
        Err(last.unwrap_or_else(|| Error::Input("INPUT no server address".into())))
    }

    fn open_one(&mut self, index: usize) -> Result<Conn> {
        let a = self.addresses[index].clone();
        let cannot = |why: String| Error::Io(format!("IO cannot connect to {}: {why}", a.text));
        let timeout = self.options.io_timeout();
        let stream = if let Some(path) = &a.unix_path {
            Self::open_unix(path, timeout).map_err(|e| cannot(conn::describe(&e)))?
        } else {
            let ct = self
                .options
                .connect_timeout
                .filter(|t| !t.is_zero())
                .or(timeout);
            let sock =
                conn::connect_tcp(&a.host, a.port, ct).map_err(|e| cannot(conn::describe(&e)))?;
            let _ = sock.set_nodelay(true);
            sock.set_read_timeout(timeout)
                .and_then(|_| sock.set_write_timeout(timeout))
                .map_err(|e| cannot(e.to_string()))?;
            if a.tls {
                self.open_tls(sock, &a).map_err(cannot)?
            } else {
                Stream::Tcp(sock)
            }
        };
        let mut c = Conn::new(stream);
        if let Some(token) = self.options.token.clone() {
            // BUSY already here: report it, do not log in.
            match c.check_idle() {
                Idle::Open => {}
                Idle::Unsolicited(e) => return Err(e),
                Idle::Closed => {
                    return Err(cannot(
                        "the server closed the connection before login".into(),
                    ))
                }
            }
            let frame = frame(b'A', token.as_bytes());
            match c.exchange(&frame) {
                Ok(reply) if reply.first() == Some(&255) => {
                    c.close();
                    return Err(Error::from_message(
                        String::from_utf8_lossy(&reply[1..]).into_owned(),
                    ));
                }
                Ok(_) => {}
                Err(e) => {
                    c.close();
                    // A server at its connection limit sends BUSY and closes at once; the
                    // reset that follows can discard the BUSY reply before it is read.
                    return Err(cannot(format!(
                        "{} (the server closed the connection before login; it may be at its connection limit)",
                        conn::describe(&e)
                    )));
                }
            }
        }
        Ok(c)
    }

    #[cfg(unix)]
    fn open_unix(path: &str, timeout: Option<Duration>) -> std::io::Result<Stream> {
        let s = std::os::unix::net::UnixStream::connect(path)?;
        s.set_read_timeout(timeout)?;
        s.set_write_timeout(timeout)?;
        Ok(Stream::Unix(s))
    }

    #[cfg(not(unix))]
    fn open_unix(_path: &str, _timeout: Option<Duration>) -> std::io::Result<Stream> {
        Err(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "unix:// addresses need a Unix platform",
        ))
    }

    #[cfg(feature = "tls")]
    fn open_tls(
        &mut self,
        sock: std::net::TcpStream,
        a: &Address,
    ) -> std::result::Result<Stream, String> {
        let config = match &self.tls {
            Some(c) => c.clone(),
            None => {
                let c = crate::tls::config(&self.options)?;
                self.tls = Some(c.clone());
                c
            }
        };
        let name = self
            .options
            .tls_server_name
            .clone()
            .unwrap_or_else(|| a.host.clone());
        Ok(Stream::Tls(Box::new(crate::tls::handshake(
            sock, &name, config,
        )?)))
    }

    #[cfg(not(feature = "tls"))]
    fn open_tls(
        &mut self,
        _sock: std::net::TcpStream,
        _a: &Address,
    ) -> std::result::Result<Stream, String> {
        Err("tls:// needs the kestrodb crate's \"tls\" feature".into())
    }

    fn drop_conn(&mut self) {
        if let Some(c) = self.conn.take() {
            c.close();
        }
    }

    // ------------------------------------------------------------------ requests

    /// Follows the cluster's leader (see the type's documentation); one server: one try.
    fn call(&mut self, cmd: u8, payload: &[u8]) -> Result<Vec<u8>> {
        if self.addresses.len() < 2 || cmd == b'A' || cmd == b'Q' {
            return self.call_once(cmd, payload);
        }
        let deadline = Instant::now() + self.options.failover_timeout;
        let mut wait = Duration::from_millis(50);
        loop {
            let e = match self.call_once(cmd, payload) {
                Ok(reply) => return Ok(reply),
                Err(e) => e,
            };
            let moved = matches!(e, Error::ReadOnly(_));
            let unreachable = matches!(&e, Error::Io(m) if m.starts_with("IO cannot connect"));
            if (!moved && !unreachable) || self.tx.open || self.closed || Instant::now() > deadline
            {
                return Err(e);
            }
            let named = wire::leader_named(e.message()).and_then(|name| self.member(name));
            let target = match named {
                Some(t) => t,
                None => {
                    std::thread::sleep(wait);
                    wait = (wait * 2).min(Duration::from_millis(500));
                    (self.current + 1) % self.addresses.len()
                }
            };
            if target != self.current || self.conn.is_none() {
                self.drop_conn();
                self.lost = false; // a move, not a failure: connect even with reconnect off
                self.current = target;
            }
        }
    }

    /// The listed member a server named (as written in its --cluster list).
    fn member(&self, name: &str) -> Option<usize> {
        if let Some(i) = self.addresses.iter().position(|a| a.text == name) {
            return Some(i);
        }
        let key = Address::parse(name).ok()?.key();
        self.addresses.iter().position(|a| a.key() == key)
    }

    fn call_once(&mut self, cmd: u8, payload: &[u8]) -> Result<Vec<u8>> {
        if self.closed {
            return Err(Error::Closed("CLOSED the connection was closed".into()));
        }
        if payload.len() > wire::MAX_FRAME - 1 {
            return Err(Error::Limit("LIMIT request too large".into()));
        }
        // An idle connection (or one not used yet): did the server close it, or send BUSY?
        if cmd != b'A' && (!self.used || self.last_use.elapsed() > IDLE_CHECK) {
            if let Some(c) = self.conn.as_mut() {
                match c.check_idle() {
                    Idle::Open => {}
                    Idle::Closed => {
                        self.drop_conn();
                        self.lost = true;
                    }
                    Idle::Unsolicited(e) => {
                        self.drop_conn();
                        self.lost = true;
                        if matches!(e, Error::Busy(_)) && !self.tx.open {
                            return Err(e);
                        }
                    }
                }
            }
        }
        if self.conn.is_none() {
            if self.tx.open {
                self.tx.end();
                return Err(Error::Io(
                    "IO connection lost inside a transaction; it was rolled back".into(),
                ));
            }
            if cmd == b'Q' {
                return Ok(vec![0]);
            }
            if self.lost && !self.options.reconnect {
                return Err(Error::Io(
                    "IO the connection was lost (reconnect is off)".into(),
                ));
            }
            self.open()?;
        }
        self.frame.clear();
        self.frame.reserve(5 + payload.len());
        self.frame
            .extend_from_slice(&(1 + payload.len() as u32).to_le_bytes());
        self.frame.push(cmd);
        self.frame.extend_from_slice(payload);
        let result = match self.conn.as_mut() {
            Some(c) => c.exchange(&self.frame),
            None => unreachable!("connected above"),
        };
        if self.frame.capacity() > 1 << 20 {
            self.frame = Vec::new();
        }
        self.last_use = Instant::now();
        self.used = true;
        let reply = match result {
            Ok(r) => r,
            Err(e) => {
                self.drop_conn();
                self.lost = true;
                let in_tx = self.tx.open;
                self.tx.end();
                return Err(Error::Io(if in_tx {
                    format!(
                        "IO connection lost inside a transaction; it was rolled back ({})",
                        conn::describe(&e)
                    )
                } else {
                    format!(
                        "IO connection lost; a write without a reply has an unknown outcome ({})",
                        conn::describe(&e)
                    )
                }));
            }
        };
        if reply[0] == 255 {
            return Err(Error::from_message(
                String::from_utf8_lossy(&reply[1..]).into_owned(),
            ));
        }
        Ok(reply)
    }

    // ------------------------------------------------------------------ SQL

    /// Runs one SQL statement (see SQL.md) with its parameters: `()`, [`params!`](crate::params!),
    /// [`named_params!`](crate::named_params!), a [`Params`], or a `serde_json` array/object.
    pub fn sql(&mut self, text: &str, params: impl Into<Params>) -> Result<SqlResult> {
        let payload = wire::sql_payload(text, &params.into().to_json()?)?;
        let reply = match self.call(b'E', &payload) {
            Ok(r) => r,
            Err(e) => {
                // A COMMIT that fails (a conflict) has rolled the transaction back on the server.
                if !matches!(e, Error::Io(_)) && wire::is_commit(text) {
                    self.tx.end();
                }
                return Err(e);
            }
        };
        let (result, has_id) = SqlResult::from_json(wire::parse_json(&reply[1..])?)?;
        self.tx.track(text);
        if has_id {
            self.last_insert_id = result.last_insert_id.clone();
        }
        Ok(result)
    }

    /// All rows of a SELECT (or another statement that returns rows).
    pub fn query(&mut self, text: &str, params: impl Into<Params>) -> Result<Vec<Row>> {
        self.sql(text, params)?.into_rows()
    }

    /// The first row, or `None`.
    pub fn query_one(&mut self, text: &str, params: impl Into<Params>) -> Result<Option<Row>> {
        Ok(self.query(text, params)?.into_iter().next())
    }

    /// The first column of the first row (`None` without rows; a null column is
    /// `Some(Value::Null)`).
    pub fn query_value(&mut self, text: &str, params: impl Into<Params>) -> Result<Option<Value>> {
        Ok(self
            .query_one(text, params)?
            .and_then(|row| row.into_iter().next().map(|(_, v)| v)))
    }

    /// All rows, each converted to `T` (any [`DeserializeOwned`] type: a struct with
    /// `#[derive(Deserialize)]`, a map, ...).
    pub fn query_as<T: DeserializeOwned>(
        &mut self,
        text: &str,
        params: impl Into<Params>,
    ) -> Result<Vec<T>> {
        self.query(text, params)?
            .into_iter()
            .map(|row| {
                serde_json::from_value(Value::Object(row))
                    .map_err(|e| Error::Input(format!("INPUT cannot convert a row: {e}")))
            })
            .collect()
    }

    /// INSERT, UPDATE, DELETE or DDL: the number of rows changed.
    pub fn execute(&mut self, text: &str, params: impl Into<Params>) -> Result<u64> {
        Ok(self.sql(text, params)?.changes)
    }

    /// BEGIN. A transaction holds the write gate until COMMIT or ROLLBACK.
    pub fn begin(&mut self) -> Result<()> {
        if self.tx.open {
            return Err(Error::Input("INPUT a transaction is already open".into()));
        }
        self.sql("BEGIN", ()).map(|_| ())
    }

    /// COMMIT; nothing to do without an open transaction. A `CONFLICT` here (BEGIN
    /// CONCURRENT) has rolled it back.
    pub fn commit(&mut self) -> Result<()> {
        if self.tx.open {
            self.sql("COMMIT", ())?;
        }
        Ok(())
    }

    /// ROLLBACK; nothing to do without an open transaction.
    pub fn rollback(&mut self) -> Result<()> {
        if self.tx.open {
            self.sql("ROLLBACK", ())?;
        }
        Ok(())
    }

    /// Runs `body` inside BEGIN ... COMMIT and returns its result. When `body` returns an
    /// error (or panics), the transaction is rolled back and the error returned (the panic
    /// resumed). `E` is any error type that a [`kestrodb::Error`](Error) converts into.
    pub fn transaction<T, E, F>(&mut self, body: F) -> std::result::Result<T, E>
    where
        F: FnOnce(&mut Client) -> std::result::Result<T, E>,
        E: From<Error>,
    {
        self.begin()?;
        match panic::catch_unwind(AssertUnwindSafe(|| body(self))) {
            Ok(Ok(value)) => {
                self.commit()?;
                Ok(value)
            }
            Ok(Err(e)) => {
                let _ = self.rollback();
                Err(e)
            }
            Err(p) => {
                let _ = self.rollback();
                panic::resume_unwind(p)
            }
        }
    }

    // ------------------------------------------------------------------ key/value

    /// The value and version of a key, or `None` when it does not exist.
    pub fn get(&mut self, collection: &str, key: &str) -> Result<Option<Versioned>> {
        let id = wire::key_id(collection, key)?;
        wire::parse_get(&self.call(b'G', &id)?)
    }

    /// As [`Client::get`], the value converted to `T`.
    pub fn get_as<T: DeserializeOwned>(
        &mut self,
        collection: &str,
        key: &str,
    ) -> Result<Option<Versioned<T>>> {
        self.get(collection, key)?
            .map(Versioned::into_typed)
            .transpose()
    }

    /// Up to 4096 keys read in one consistent snapshot, in order; `None` where a key is
    /// missing.
    pub fn get_many<K: AsRef<str>>(
        &mut self,
        collection: &str,
        keys: &[K],
    ) -> Result<Vec<Option<Versioned>>> {
        let payload = wire::many_payload(collection, keys)?;
        wire::parse_many(&self.call(b'M', &payload)?, keys.len())
    }

    /// Stores a value (any [`Serialize`] type, as JSON); returns its new version.
    pub fn set(&mut self, collection: &str, key: &str, value: impl Serialize) -> Result<u64> {
        self.batch(&[Op::set(collection, key, value)])
    }

    /// Stores a value only if the key is at `expected_version` (0: it must not exist);
    /// otherwise [`Error::Conflict`].
    pub fn set_if(
        &mut self,
        collection: &str,
        key: &str,
        value: impl Serialize,
        expected_version: u64,
    ) -> Result<u64> {
        self.batch(&[Op::set(collection, key, value).if_version(expected_version)])
    }

    /// Deletes a key (deleting a missing key is not an error); returns the version.
    pub fn delete(&mut self, collection: &str, key: &str) -> Result<u64> {
        self.batch(&[Op::delete(collection, key)])
    }

    /// Deletes a key only if it is at `expected_version`; otherwise [`Error::Conflict`].
    pub fn delete_if(&mut self, collection: &str, key: &str, expected_version: u64) -> Result<u64> {
        self.batch(&[Op::delete(collection, key).if_version(expected_version)])
    }

    /// Up to 65536 operations, all or nothing, durable when it returns; the version shared
    /// by every operation.
    pub fn batch(&mut self, operations: &[Op]) -> Result<u64> {
        let payload = wire::batch_payload(operations)?;
        wire::get_u64(&self.call(b'B', &payload)?, 1)
    }

    // ------------------------------------------------------------------ server

    fn json_object(reply: &[u8]) -> Result<Map<String, Value>> {
        match wire::parse_json(&reply[1..])? {
            Value::Object(m) => Ok(m),
            _ => Err(Error::Io(
                "IO the server's reply is not a JSON object".into(),
            )),
        }
    }

    /// Server statistics: keys, bytes, txid, connections, tls, user, role, cluster, ...
    pub fn stats(&mut self) -> Result<Map<String, Value>> {
        Self::json_object(&self.call(b'S', NO_BYTES)?)
    }

    /// Checks every checksum of the database file (admin).
    pub fn verify(&mut self) -> Result<()> {
        self.call(b'V', NO_BYTES).map(|_| ())
    }

    /// Online backup to a plain file name in the server's `--backup-dir` (admin).
    pub fn backup(&mut self, name: &str) -> Result<()> {
        self.call(b'P', name.as_bytes()).map(|_| ())
    }

    /// A compacted copy as a plain file name in the server's `--backup-dir` (admin).
    pub fn compact_to(&mut self, name: &str) -> Result<()> {
        self.call(b'C', name.as_bytes()).map(|_| ())
    }

    /// Online compaction of the server's file (admin): `{before, after, keys}`.
    pub fn compact(&mut self) -> Result<Map<String, Value>> {
        Self::json_object(&self.call(b'K', NO_BYTES)?)
    }

    /// Turns a replica into a writable server (admin); it stops following its leader.
    pub fn promote(&mut self) -> Result<()> {
        self.call(b'X', NO_BYTES).map(|_| ())
    }

    /// Ends the connection (an open transaction is rolled back by the server). Later calls
    /// fail with [`Error::Closed`]; closing again does nothing. Dropping the client closes it.
    pub fn close(&mut self) {
        if self.closed {
            return;
        }
        if let Some(mut c) = self.conn.take() {
            let _ = c.exchange(&frame(b'Q', NO_BYTES));
            c.close();
        }
        self.closed = true;
        self.tx.end();
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        self.close();
    }
}

fn frame(cmd: u8, payload: &[u8]) -> Vec<u8> {
    let mut f = Vec::with_capacity(5 + payload.len());
    f.extend_from_slice(&(1 + payload.len() as u32).to_le_bytes());
    f.push(cmd);
    f.extend_from_slice(payload);
    f
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::thread;

    /// A fake server: answers each frame with `reply(cmd, payload)`; `None` closes.
    fn fake<F>(reply: F) -> (String, thread::JoinHandle<()>)
    where
        F: Fn(u8, &[u8]) -> Option<Vec<u8>> + Send + 'static,
    {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = format!("tcp://{}", l.local_addr().unwrap());
        let h = thread::spawn(move || {
            let (mut s, _) = l.accept().unwrap();
            loop {
                let mut head = [0u8; 4];
                if s.read_exact(&mut head).is_err() {
                    return;
                }
                let mut f = vec![0u8; u32::from_le_bytes(head) as usize];
                s.read_exact(&mut f).unwrap();
                match reply(f[0], &f[1..]) {
                    Some(r) => {
                        s.write_all(&(r.len() as u32).to_le_bytes()).unwrap();
                        s.write_all(&r).unwrap();
                    }
                    None => return,
                }
            }
        });
        (addr, h)
    }

    fn ok_json(json: &str) -> Vec<u8> {
        [&[0u8][..], json.as_bytes()].concat()
    }

    fn err(message: &str) -> Vec<u8> {
        [&[255u8][..], message.as_bytes()].concat()
    }

    #[test]
    fn sql_round_trip_with_a_fake_server() {
        let (addr, h) = fake(|cmd, payload| match cmd {
            b'A' => Some(if payload == b"secret" {
                vec![0]
            } else {
                err("AUTH invalid token")
            }),
            b'E' => {
                let n = u32::from_le_bytes(payload[..4].try_into().unwrap()) as usize;
                let text = std::str::from_utf8(&payload[4..4 + n]).unwrap();
                let params = std::str::from_utf8(&payload[4 + n..]).unwrap();
                Some(match text {
                    "SELECT" => ok_json(&format!(r#"{{"rows":[{{"b":{params},"a":1}}]}}"#)),
                    "INSERT" => ok_json(r#"{"changes":1,"lastInsertId":42}"#),
                    "BEGIN" | "ROLLBACK" => ok_json(r#"{"changes":0}"#),
                    "COMMIT" => err("CONFLICT another transaction committed first"),
                    _ => err("INPUT unknown statement"),
                })
            }
            b'G' => Some(if payload == b"kv\0k" {
                [&[0u8][..], &5u64.to_le_bytes(), br#"{"x":true}"#].concat()
            } else {
                vec![1]
            }),
            b'B' => Some([&[0u8][..], &9u64.to_le_bytes()].concat()),
            b'S' => Some(ok_json(r#"{"server":true}"#)),
            b'Q' => Some(vec![0]),
            _ => Some(err("INPUT unknown command")),
        });
        let mut db = Client::connect(addr.as_str(), Options::new().token("secret")).unwrap();
        assert_eq!(db.address(), addr);
        let rows = db
            .query("SELECT", crate::named_params! { ":p" => "v" })
            .unwrap();
        assert_eq!(rows[0].keys().collect::<Vec<_>>(), vec!["b", "a"]);
        assert_eq!(rows[0]["b"], serde_json::json!({"p": "v"}));
        assert_eq!(
            db.query_value("SELECT", ()).unwrap(),
            Some(serde_json::json!([]))
        );
        assert_eq!(db.execute("INSERT", crate::params![1]).unwrap(), 1);
        assert_eq!(db.last_insert_id(), Some(&serde_json::json!(42)));
        assert_eq!(db.query("INSERT", ()).unwrap_err().code(), "INPUT");
        assert_eq!(db.execute("nope", ()).unwrap_err().code(), "INPUT");
        db.begin().unwrap();
        assert!(db.in_transaction());
        assert_eq!(db.begin().unwrap_err().code(), "INPUT");
        assert!(matches!(db.commit(), Err(Error::Conflict(_))));
        assert!(!db.in_transaction(), "a failed COMMIT ends the transaction");
        let r: std::result::Result<(), Error> =
            db.transaction(|tx| tx.execute("nope", ()).map(|_| ()));
        assert_eq!(r.unwrap_err().code(), "INPUT");
        assert!(!db.in_transaction());
        assert_eq!(
            db.get("kv", "k").unwrap(),
            Some(Versioned {
                version: 5,
                value: serde_json::json!({"x": true})
            })
        );
        assert_eq!(db.get("kv", "missing").unwrap(), None);
        assert_eq!(db.set("kv", "k", 1).unwrap(), 9);
        assert_eq!(db.stats().unwrap()["server"], serde_json::json!(true));
        assert_eq!(db.connections_opened(), 1);
        db.close();
        assert!(db.is_closed());
        assert!(matches!(db.get("kv", "k"), Err(Error::Closed(_))));
        h.join().unwrap();
    }

    #[test]
    fn wrong_token_is_auth() {
        let (addr, h) = fake(|_, _| Some(err("AUTH invalid token")));
        let e = Client::connect(addr, Options::new().token("x")).unwrap_err();
        assert!(matches!(e, Error::Auth(_)), "{e:?}");
        h.join().unwrap();
    }

    #[test]
    fn lost_connection_is_io_and_reconnect_off_stays_failed() {
        let (addr, h) = fake(|cmd, _| if cmd == b'S' { None } else { Some(vec![0]) });
        let mut db = Client::connect(addr, Options::new().reconnect(false)).unwrap();
        let e = db.stats().unwrap_err();
        assert!(
            e.message()
                .starts_with("IO connection lost; a write without a reply has an unknown outcome"),
            "{e}"
        );
        let again = db.stats().unwrap_err();
        assert_eq!(
            again.message(),
            "IO the connection was lost (reconnect is off)"
        );
        h.join().unwrap();
    }

    #[test]
    fn nothing_listening() {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = format!("tcp://{}", l.local_addr().unwrap());
        drop(l);
        let e = Client::connect(
            addr.as_str(),
            Options::new().timeout(Duration::from_secs(5)),
        )
        .unwrap_err();
        assert!(
            e.message()
                .starts_with(&format!("IO cannot connect to {addr}")),
            "{e}"
        );
        assert!(matches!(
            Client::connect(" , ", Options::new()),
            Err(Error::Input(_))
        ));
        assert!(matches!(
            Client::connect("tcp://127.0.0.1", Options::new()),
            Err(Error::Input(_))
        ));
    }

    #[cfg(not(feature = "tls"))]
    #[test]
    fn tls_needs_the_feature() {
        let e = Client::connect("tls://127.0.0.1:1", Options::new()).unwrap_err();
        assert!(matches!(e, Error::Input(_)), "{e}");
    }

    #[test]
    fn address_lists() {
        assert_eq!("a:1, b:2".into_addresses(), vec!["a:1", " b:2"]);
        assert_eq!(vec!["a:1", "b:2"].into_addresses().len(), 2);
        assert_eq!(["a:1"].into_addresses(), vec!["a:1"]);
        assert_eq!(String::from("a:1").into_addresses(), vec!["a:1"]);
    }

    #[test]
    fn client_is_send() {
        fn send<T: Send>() {}
        send::<Client>();
        send::<Error>();
    }
}

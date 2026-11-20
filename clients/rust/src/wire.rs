//! The binary protocol without I/O: request payloads, reply parsing, addresses, and the
//! transaction state that SQL text implies. Integers are little-endian.
//!
//! A frame is `u32 length` + `command byte` + payload; a reply is `u32 length` + status byte
//! (255: an error whose message follows as UTF-8) + data.

use crate::error::{Error, Result};
use crate::types::{Op, Versioned};
use serde_json::Value;
use std::collections::HashSet;

/// Largest frame either way (64 MB), as on the server.
pub const MAX_FRAME: usize = 64 * 1024 * 1024;
/// Largest JSON value of a key (256 KiB).
pub const MAX_VALUE: usize = 256 * 1024;
/// Most keys in one `get_many`.
pub const MAX_GET_MANY: usize = 4096;
/// Most operations in one batch.
pub const MAX_BATCH: usize = 65536;
/// "Any version" in a batch operation.
const ANY_VERSION: u64 = u64::MAX;

fn short() -> Error {
    Error::Io("IO short reply from the server".into())
}

pub(crate) fn get_u32(b: &[u8], at: usize) -> Result<u32> {
    let s = b.get(at..at + 4).ok_or_else(short)?;
    Ok(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
}

pub(crate) fn get_u64(b: &[u8], at: usize) -> Result<u64> {
    let s = b.get(at..at + 8).ok_or_else(short)?;
    let mut a = [0u8; 8];
    a.copy_from_slice(s);
    Ok(u64::from_le_bytes(a))
}

fn put_u32(out: &mut Vec<u8>, v: usize) {
    out.extend_from_slice(&(v as u32).to_le_bytes());
}

pub(crate) fn parse_json(bytes: &[u8]) -> Result<Value> {
    serde_json::from_slice(bytes)
        .map_err(|e| Error::Io(format!("IO invalid JSON from the server: {e}")))
}

fn valid_collection(c: &str) -> bool {
    let b = c.as_bytes();
    !b.is_empty()
        && b.len() <= 64
        && (b[0].is_ascii_alphabetic() || b[0] == b'_')
        && b.iter().all(|&x| x.is_ascii_alphanumeric() || x == b'_')
}

/// `collection \0 key`, after checking both.
pub(crate) fn key_id(collection: &str, key: &str) -> Result<Vec<u8>> {
    if !valid_collection(collection) {
        return Err(Error::Input(
            "INPUT Collection must be a 1-64 character ASCII identifier.".into(),
        ));
    }
    let k = key.as_bytes();
    if k.is_empty() || k.len() > 512 || k.contains(&0) {
        return Err(Error::Input(
            "INPUT Key must contain 1-512 UTF-8 bytes and no NUL.".into(),
        ));
    }
    let mut out = Vec::with_capacity(collection.len() + 1 + k.len());
    out.extend_from_slice(collection.as_bytes());
    out.push(0);
    out.extend_from_slice(k);
    Ok(out)
}

/// `E`: u32 text length, the text, the parameters' JSON.
pub(crate) fn sql_payload(text: &str, params_json: &[u8]) -> Result<Vec<u8>> {
    if text.len() > MAX_FRAME {
        return Err(Error::Limit("LIMIT request too large".into()));
    }
    let mut out = Vec::with_capacity(4 + text.len() + params_json.len());
    put_u32(&mut out, text.len());
    out.extend_from_slice(text.as_bytes());
    out.extend_from_slice(params_json);
    Ok(out)
}

/// `M`: u32 count, then u32 length + id per key.
pub(crate) fn many_payload<K: AsRef<str>>(collection: &str, keys: &[K]) -> Result<Vec<u8>> {
    if keys.len() > MAX_GET_MANY {
        return Err(Error::Limit("LIMIT getMany count".into()));
    }
    let mut out = Vec::with_capacity(4 + keys.len() * (collection.len() + 16));
    put_u32(&mut out, keys.len());
    for k in keys {
        let id = key_id(collection, k.as_ref())?;
        put_u32(&mut out, id.len());
        out.extend_from_slice(&id);
    }
    Ok(out)
}

/// `G` reply: 1 = missing; else u64 version + JSON.
pub(crate) fn parse_get(reply: &[u8]) -> Result<Option<Versioned>> {
    if reply.first() == Some(&1) {
        return Ok(None);
    }
    let version = get_u64(reply, 1)?;
    Ok(Some(Versioned {
        version,
        value: parse_json(&reply[9..])?,
    }))
}

/// `M` reply: u32 count, then per key a found byte, and if found u64 version, u32 length, JSON.
pub(crate) fn parse_many(reply: &[u8], expected: usize) -> Result<Vec<Option<Versioned>>> {
    let count = get_u32(reply, 1)? as usize;
    if count != expected {
        return Err(Error::Io("IO getMany reply count".into()));
    }
    let mut out = Vec::with_capacity(count);
    let mut p = 5;
    for _ in 0..count {
        let found = *reply.get(p).ok_or_else(short)?;
        p += 1;
        if found == 0 {
            out.push(None);
            continue;
        }
        let version = get_u64(reply, p)?;
        let len = get_u32(reply, p + 8)? as usize;
        p += 12;
        let end = p
            .checked_add(len)
            .filter(|&e| e <= reply.len())
            .ok_or_else(short)?;
        out.push(Some(Versioned {
            version,
            value: parse_json(&reply[p..end])?,
        }));
        p = end;
    }
    Ok(out)
}

/// `B`: u32 count, then per operation: u8 set(1)/delete(0), u64 expected version
/// (all ones: any), u32 id length, u32 value length, id, value.
pub(crate) fn batch_payload(ops: &[Op]) -> Result<Vec<u8>> {
    if ops.is_empty() || ops.len() > MAX_BATCH {
        return Err(Error::Input("INPUT operation count".into()));
    }
    let mut out = Vec::with_capacity(4 + ops.len() * 64);
    put_u32(&mut out, ops.len());
    let mut seen = HashSet::new();
    for op in ops {
        let id = key_id(&op.collection, &op.key)?;
        let value: &[u8] = match (&op.value, op.set) {
            (_, false) => &[],
            (Ok(v), true) => v,
            (Err(e), true) => return Err(e.clone()),
        };
        if value.len() > MAX_VALUE {
            return Err(Error::Limit(
                "LIMIT A value may contain at most 256 KiB of JSON.".into(),
            ));
        }
        if ops.len() > 1 && !seen.insert(id.clone()) {
            return Err(Error::Input("INPUT duplicate transaction key".into()));
        }
        out.push(u8::from(op.set));
        out.extend_from_slice(&op.expected_version.unwrap_or(ANY_VERSION).to_le_bytes());
        put_u32(&mut out, id.len());
        put_u32(&mut out, value.len());
        out.extend_from_slice(&id);
        out.extend_from_slice(value);
        if out.len() > MAX_FRAME - 64 {
            return Err(Error::Limit("LIMIT transaction bytes".into()));
        }
    }
    Ok(out)
}

/// One member address: `tcp://host:port`, `tls://host:port`, `unix:///path` or `host:port`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Address {
    pub text: String,
    pub host: String,
    pub port: u16,
    pub tls: bool,
    pub unix_path: Option<String>,
}

impl Address {
    pub fn parse(text: &str) -> Result<Address> {
        let usage = || {
            Error::Input(format!(
                "INPUT address must be tcp://host:port, tls://host:port or unix:///path: {text}"
            ))
        };
        if let Some(path) = text.strip_prefix("unix://") {
            if path.is_empty() {
                return Err(usage());
            }
            return Ok(Address {
                text: text.into(),
                host: String::new(),
                port: 0,
                tls: false,
                unix_path: Some(path.into()),
            });
        }
        let tls = text.starts_with("tls://");
        let hp = if tls || text.starts_with("tcp://") {
            &text[6..]
        } else {
            text
        };
        let colon = hp.rfind(':').ok_or_else(usage)?;
        let mut host = &hp[..colon];
        if host.len() >= 2 && host.starts_with('[') && host.ends_with(']') {
            host = &host[1..host.len() - 1];
        }
        let port: u16 = hp[colon + 1..]
            .parse()
            .ok()
            .filter(|&p| p > 0)
            .ok_or_else(|| Error::Input(format!("INPUT invalid port in address {text}")))?;
        let host = if host.is_empty() { "127.0.0.1" } else { host };
        Ok(Address {
            text: text.into(),
            host: host.into(),
            port,
            tls,
            unix_path: None,
        })
    }

    /// The same member however it is written.
    pub fn key(&self) -> String {
        match &self.unix_path {
            Some(_) => self.text.clone(),
            None => format!(
                "{}://{}:{}",
                if self.tls { "tls" } else { "tcp" },
                self.host.to_ascii_lowercase(),
                self.port
            ),
        }
    }
}

/// The leader a `READONLY` message names ("... the leader is tcp://h:p ...").
pub(crate) fn leader_named(message: &str) -> Option<&str> {
    let at = message.find("the leader is ")? + "the leader is ".len();
    message[at..].split_whitespace().next()
}

/// The first word of SQL text, upper-cased (letters, digits and `_`).
fn first_word(text: &str) -> String {
    text.trim_start()
        .chars()
        .take_while(|c| c.is_ascii_alphanumeric() || *c == '_')
        .collect::<String>()
        .to_ascii_uppercase()
}

/// `COMMIT ...` or `END ...`: a failed one (not IO) has ended the transaction on the server.
pub(crate) fn is_commit(text: &str) -> bool {
    matches!(first_word(text).as_str(), "COMMIT" | "END")
}

fn unquote(word: &str) -> &str {
    let s = word.strip_suffix(';').unwrap_or(word);
    let b = s.as_bytes();
    if b.len() >= 2 && (b[0] == b'"' || b[0] == b'`') && b[b.len() - 1] == b[0] {
        &s[1..s.len() - 1]
    } else {
        s
    }
}

/// Whether a transaction is open, as the statements that succeeded say.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub(crate) struct TxState {
    pub open: bool,
    /// A SAVEPOINT that began the transaction: releasing it commits.
    pub implicit_savepoint: Option<String>,
}

impl TxState {
    pub fn end(&mut self) {
        self.open = false;
        self.implicit_savepoint = None;
    }

    /// BEGIN/START, COMMIT/END, ROLLBACK (not ROLLBACK TO), SAVEPOINT outside a transaction,
    /// and RELEASE of that savepoint.
    pub fn track(&mut self, text: &str) {
        let mut words = text.split_whitespace();
        let w0 = words.next().unwrap_or("");
        let w0 = w0.strip_suffix(';').unwrap_or(w0).to_ascii_uppercase();
        let w1 = words.next();
        let w2 = words.next();
        match w0.as_str() {
            "BEGIN" | "START" => {
                self.open = true;
                self.implicit_savepoint = None;
            }
            "COMMIT" | "END" => self.end(),
            "ROLLBACK" => {
                if !w1.is_some_and(|w| w.eq_ignore_ascii_case("TO")) {
                    self.end();
                }
            }
            "SAVEPOINT" => {
                if let (false, Some(name)) = (self.open, w1) {
                    self.open = true;
                    self.implicit_savepoint = Some(unquote(name).to_string());
                }
            }
            "RELEASE" => {
                let name = match (w1, w2) {
                    (Some(s), Some(n)) if s.eq_ignore_ascii_case("SAVEPOINT") => Some(n),
                    (Some(n), _) => Some(n),
                    _ => None,
                };
                if let (Some(n), Some(sp)) = (name, &self.implicit_savepoint) {
                    if unquote(n).eq_ignore_ascii_case(sp) {
                        self.end();
                    }
                }
            }
            _ => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn key_ids() {
        assert_eq!(
            key_id("users", "kestro").unwrap(),
            b"users\0kestro".to_vec()
        );
        assert_eq!(
            key_id("_a9", "ключ 😀").unwrap(),
            [b"_a9\0".as_slice(), "ключ 😀".as_bytes()].concat()
        );
        for bad in ["", "9a", "bad-name", "a b", "é", &"c".repeat(65)] {
            assert_eq!(key_id(bad, "k").unwrap_err().code(), "INPUT", "{bad}");
        }
        assert!(key_id(&"c".repeat(64), "k").is_ok());
        assert_eq!(key_id("kv", "").unwrap_err().code(), "INPUT");
        assert_eq!(key_id("kv", "a\0b").unwrap_err().code(), "INPUT");
        assert_eq!(key_id("kv", &"x".repeat(513)).unwrap_err().code(), "INPUT");
        assert!(key_id("kv", &"x".repeat(512)).is_ok());
        assert_eq!(key_id("kv", &"é".repeat(257)).unwrap_err().code(), "INPUT");
        // 514 bytes
    }

    #[test]
    fn sql_frame() {
        let p = sql_payload("SELECT ?", b"[1]").unwrap();
        assert_eq!(&p[..4], &8u32.to_le_bytes());
        assert_eq!(&p[4..12], b"SELECT ?");
        assert_eq!(&p[12..], b"[1]");
    }

    #[test]
    fn many_round_trip() {
        let p = many_payload("kv", &["a", "bc"]).unwrap();
        assert_eq!(
            p,
            [
                &2u32.to_le_bytes()[..],
                &4u32.to_le_bytes(),
                b"kv\0a",
                &5u32.to_le_bytes(),
                b"kv\0bc"
            ]
            .concat()
        );
        let too_many: Vec<String> = (0..4097).map(|i| format!("k{i}")).collect();
        assert_eq!(many_payload("kv", &too_many).unwrap_err().code(), "LIMIT");
        assert!(many_payload::<&str>("kv", &[]).is_ok());

        let mut reply = vec![0u8];
        reply.extend_from_slice(&2u32.to_le_bytes());
        reply.push(1);
        reply.extend_from_slice(&7u64.to_le_bytes());
        reply.extend_from_slice(&5u32.to_le_bytes());
        reply.extend_from_slice(b"[1,2]");
        reply.push(0);
        let got = parse_many(&reply, 2).unwrap();
        assert_eq!(
            got,
            vec![
                Some(Versioned {
                    version: 7,
                    value: json!([1, 2])
                }),
                None
            ]
        );
        assert_eq!(parse_many(&reply, 3).unwrap_err().code(), "IO");
        assert_eq!(parse_many(&reply[..12], 2).unwrap_err().code(), "IO");
    }

    #[test]
    fn get_reply() {
        assert_eq!(parse_get(&[1]).unwrap(), None);
        let mut r = vec![0u8];
        r.extend_from_slice(&9u64.to_le_bytes());
        r.extend_from_slice(br#"{"n":1}"#);
        assert_eq!(
            parse_get(&r).unwrap(),
            Some(Versioned {
                version: 9,
                value: json!({"n": 1})
            })
        );
        assert_eq!(parse_get(&[0, 1, 2]).unwrap_err().code(), "IO");
        r.truncate(12);
        assert_eq!(parse_get(&r).unwrap_err().code(), "IO");
    }

    #[test]
    fn batches() {
        let p =
            batch_payload(&[Op::set("kv", "a", 1).if_version(3), Op::delete("kv", "b")]).unwrap();
        let mut want = 2u32.to_le_bytes().to_vec();
        want.push(1);
        want.extend_from_slice(&3u64.to_le_bytes());
        want.extend_from_slice(&4u32.to_le_bytes());
        want.extend_from_slice(&1u32.to_le_bytes());
        want.extend_from_slice(b"kv\0a1");
        want.push(0);
        want.extend_from_slice(&u64::MAX.to_le_bytes());
        want.extend_from_slice(&4u32.to_le_bytes());
        want.extend_from_slice(&0u32.to_le_bytes());
        want.extend_from_slice(b"kv\0b");
        assert_eq!(p, want);
        assert_eq!(batch_payload(&[]).unwrap_err().code(), "INPUT");
        assert_eq!(
            batch_payload(&[Op::set("kv", "d", 1), Op::delete("kv", "d")])
                .unwrap_err()
                .code(),
            "INPUT"
        );
        assert_eq!(
            batch_payload(&[Op::set("kv", "h", "z".repeat(300 * 1024))])
                .unwrap_err()
                .code(),
            "LIMIT"
        );
        assert!(batch_payload(&[Op::set("kv", "h", "z".repeat(200 * 1024))]).is_ok());
        assert_eq!(
            batch_payload(&[Op::set("bad-name", "k", 1)])
                .unwrap_err()
                .code(),
            "INPUT"
        );
    }

    #[test]
    fn addresses() {
        let a = Address::parse("tcp://127.0.0.1:7820").unwrap();
        assert_eq!((a.host.as_str(), a.port, a.tls), ("127.0.0.1", 7820, false));
        let t = Address::parse("tls://DB.Example.org:443").unwrap();
        assert!(t.tls);
        assert_eq!(t.key(), "tls://db.example.org:443");
        let six = Address::parse("[::1]:9").unwrap();
        assert_eq!((six.host.as_str(), six.port), ("::1", 9));
        assert_eq!(Address::parse(":7820").unwrap().host, "127.0.0.1");
        assert_eq!(
            Address::parse("localhost:1").unwrap().key(),
            "tcp://localhost:1"
        );
        assert_eq!(
            Address::parse("tcp://LOCALHOST:1").unwrap().key(),
            Address::parse("localhost:1").unwrap().key()
        );
        let u = Address::parse("unix:///run/k.sock").unwrap();
        assert_eq!(u.unix_path.as_deref(), Some("/run/k.sock"));
        for bad in ["tcp://127.0.0.1", "x", "h:0", "h:65536", "h:abc", "unix://"] {
            assert_eq!(Address::parse(bad).unwrap_err().code(), "INPUT", "{bad}");
        }
    }

    #[test]
    fn leader_names() {
        assert_eq!(
            leader_named("READONLY this member follows; the leader is tcp://127.0.0.1:5 (term 3)"),
            Some("tcp://127.0.0.1:5")
        );
        assert_eq!(leader_named("READONLY no leader yet"), None);
    }

    #[test]
    fn transaction_tracking() {
        let mut t = TxState::default();
        t.track("SELECT 1");
        assert!(!t.open);
        t.track("  begin concurrent");
        assert!(t.open);
        t.track("  commit;");
        assert!(!t.open);
        t.track("start transaction");
        assert!(t.open);
        t.track("SAVEPOINT a");
        t.track("ROLLBACK TO SAVEPOINT a");
        assert!(t.open, "ROLLBACK TO keeps it open");
        t.track("rollback");
        assert!(!t.open);
        t.track("SAVEPOINT \"outer\";");
        assert!(t.open);
        assert_eq!(t.implicit_savepoint.as_deref(), Some("outer"));
        t.track("RELEASE other");
        assert!(t.open);
        t.track("release savepoint OUTER");
        assert!(!t.open, "releasing the savepoint that began it commits");
        t.track("BEGIN;");
        t.track("END");
        assert!(!t.open);
        assert!(is_commit("  commit;"));
        assert!(is_commit("END TRANSACTION"));
        assert!(!is_commit("ENDX"));
        assert!(!is_commit("SELECT 1"));
    }
}

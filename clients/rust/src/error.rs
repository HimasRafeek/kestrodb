//! Errors by code.

use std::fmt;

/// An error from kestro-server or from this client.
///
/// Each variant is one error code; the message always starts with that code, as the server
/// sends it (`"CONFLICT unique constraint users.email"`). [`Error::code`] returns the code,
/// [`Error::message`] (and `Display`) the whole message.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum Error {
    /// `CONFLICT`: a unique constraint, a duplicate id, a failed `expected_version`, or a
    /// `BEGIN CONCURRENT` transaction whose COMMIT lost to another committer.
    Conflict(String),
    /// `INPUT`: invalid SQL, parameters, names, values or addresses.
    Input(String),
    /// `LIMIT`: a size or count limit (a value over 256 KiB, more than 4096 keys, ...).
    Limit(String),
    /// `AUTH`: a wrong token, or a user that is not allowed to do this.
    Auth(String),
    /// `IO`: the connection failed or was lost. A write sent without a reply has an
    /// unknown outcome.
    Io(String),
    /// `BUSY`: the server is at its connection limit (`--max-connections`).
    Busy(String),
    /// `READONLY`: a follower or replica refused a write. A client given a cluster's members
    /// retries at the leader by itself.
    ReadOnly(String),
    /// `UNAVAILABLE`: a cluster's majority did not confirm a write in time; its outcome is
    /// unknown, as with `Io`.
    Unavailable(String),
    /// `CORRUPT`: the database file failed a checksum.
    Corrupt(String),
    /// `CLOSED`: the program closed this client.
    Closed(String),
    /// Any other code (`STALE`, `UNKNOWN_COMMIT`, `UNSUPPORTED`, ...); see [`Error::code`].
    Other(String),
}

impl Error {
    /// The error for a message whose first word is its code (as the server sends errors).
    pub fn from_message(message: impl Into<String>) -> Error {
        let m = message.into();
        match m.split(' ').next().unwrap_or("") {
            "CONFLICT" => Error::Conflict(m),
            "INPUT" => Error::Input(m),
            "LIMIT" => Error::Limit(m),
            "AUTH" => Error::Auth(m),
            "IO" => Error::Io(m),
            "BUSY" => Error::Busy(m),
            "READONLY" => Error::ReadOnly(m),
            "UNAVAILABLE" => Error::Unavailable(m),
            "CORRUPT" => Error::Corrupt(m),
            "CLOSED" => Error::Closed(m),
            _ => Error::Other(m),
        }
    }

    /// The error code: `CONFLICT`, `INPUT`, `LIMIT`, `AUTH`, `IO`, `BUSY`, `READONLY`,
    /// `UNAVAILABLE`, `CORRUPT`, `CLOSED`, another code the server sent, or `ERROR` when the
    /// message has none.
    pub fn code(&self) -> &str {
        match self {
            Error::Conflict(_) => "CONFLICT",
            Error::Input(_) => "INPUT",
            Error::Limit(_) => "LIMIT",
            Error::Auth(_) => "AUTH",
            Error::Io(_) => "IO",
            Error::Busy(_) => "BUSY",
            Error::ReadOnly(_) => "READONLY",
            Error::Unavailable(_) => "UNAVAILABLE",
            Error::Corrupt(_) => "CORRUPT",
            Error::Closed(_) => "CLOSED",
            Error::Other(m) => {
                let head = m.split(' ').next().unwrap_or("");
                if !head.is_empty() && head.bytes().all(|c| c.is_ascii_uppercase() || c == b'_') {
                    head
                } else {
                    "ERROR"
                }
            }
        }
    }

    /// The whole message, starting with the code.
    pub fn message(&self) -> &str {
        match self {
            Error::Conflict(m)
            | Error::Input(m)
            | Error::Limit(m)
            | Error::Auth(m)
            | Error::Io(m)
            | Error::Busy(m)
            | Error::ReadOnly(m)
            | Error::Unavailable(m)
            | Error::Corrupt(m)
            | Error::Closed(m)
            | Error::Other(m) => m,
        }
    }

    /// True for `IO` and `UNAVAILABLE`: a write that got this error may or may not have been
    /// applied.
    pub fn is_unknown_outcome(&self) -> bool {
        matches!(self, Error::Io(_) | Error::Unavailable(_))
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.message())
    }
}

impl std::error::Error for Error {}

/// `Result<T, kestrodb::Error>`.
pub type Result<T, E = Error> = std::result::Result<T, E>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_map_to_variants() {
        assert_eq!(
            Error::from_message("CONFLICT x"),
            Error::Conflict("CONFLICT x".into())
        );
        assert!(matches!(
            Error::from_message("INPUT bad SQL"),
            Error::Input(_)
        ));
        assert!(matches!(Error::from_message("LIMIT x"), Error::Limit(_)));
        assert!(matches!(
            Error::from_message("AUTH invalid token"),
            Error::Auth(_)
        ));
        assert!(matches!(Error::from_message("IO lost"), Error::Io(_)));
        assert!(matches!(
            Error::from_message("BUSY too many connections"),
            Error::Busy(_)
        ));
        assert!(matches!(
            Error::from_message("READONLY the leader is tcp://a:1"),
            Error::ReadOnly(_)
        ));
        assert!(matches!(
            Error::from_message("UNAVAILABLE x"),
            Error::Unavailable(_)
        ));
        assert!(matches!(
            Error::from_message("CORRUPT x"),
            Error::Corrupt(_)
        ));
        assert!(matches!(Error::from_message("CLOSED x"), Error::Closed(_)));
        assert!(matches!(Error::from_message("INPUT"), Error::Input(_)));
    }

    #[test]
    fn other_codes() {
        let e = Error::from_message("STALE something");
        assert!(matches!(e, Error::Other(_)));
        assert_eq!(e.code(), "STALE");
        assert_eq!(
            Error::from_message("UNKNOWN_COMMIT x").code(),
            "UNKNOWN_COMMIT"
        );
        assert_eq!(Error::from_message("weird message").code(), "ERROR");
        assert_eq!(Error::from_message("").code(), "ERROR");
        assert_eq!(Error::from_message("Conflict lowercase").code(), "ERROR");
    }

    #[test]
    fn display_and_std_error() {
        let e = Error::from_message("CONFLICT unique constraint");
        assert_eq!(e.to_string(), "CONFLICT unique constraint");
        assert_eq!(e.message(), "CONFLICT unique constraint");
        assert_eq!(e.code(), "CONFLICT");
        let boxed: Box<dyn std::error::Error + Send + Sync> = Box::new(e);
        assert_eq!(boxed.to_string(), "CONFLICT unique constraint");
        assert!(Error::Io("IO x".into()).is_unknown_outcome());
        assert!(!Error::Input("INPUT x".into()).is_unknown_outcome());
    }
}

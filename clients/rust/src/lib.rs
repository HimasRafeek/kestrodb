//! Client for kestro-server, the KestroDB database server: SQL and key/value over TCP, TLS
//! (feature `tls`) or a Unix socket.
//!
//! ```no_run
//! use kestrodb::serde_json::json;
//! use kestrodb::{named_params, params, Client, Options};
//!
//! fn main() -> kestrodb::Result<()> {
//!     let options = Options { token: std::env::var("KESTRODB_TOKEN").ok(), ..Options::default() };
//!     let mut db = Client::connect("tcp://127.0.0.1:7820", options)?;
//!     db.execute("INSERT INTO users (email, name) VALUES (:email, :name)",
//!                named_params! { "email" => "a@x.org", "name" => "Ann" })?;
//!     let id = db.last_insert_id().cloned();
//!     let user = db.query_one("SELECT * FROM users WHERE id = ?", params![id])?;
//!     db.transaction(|tx| {
//!         tx.execute("UPDATE accounts SET balance = balance - ? WHERE id = ?", params![10, 1])?;
//!         tx.execute("UPDATE accounts SET balance = balance + ? WHERE id = ?", params![10, 2])?;
//!         Ok::<_, kestrodb::Error>(())
//!     })?;
//!     let v = db.set("users", "kestro", json!({"name": "Kestro"}))?;
//!     db.set_if("users", "kestro", json!({"name": "K"}), v)?; // only if still at v
//!     Ok(())
//! }
//! ```
//!
//! Rows are ordered maps ([`Row`]) from column name to `serde_json::Value`, in `SELECT`
//! order; [`Client::query_as`] converts them to your own `Deserialize` types. Parameters are
//! any `Serialize` values. Errors are [`Error`] variants by code.
//!
//! See [`Client`] for reconnecting and following a cluster's leader, and [`Options`] for
//! timeouts and TLS.

#![forbid(unsafe_code)]
#![warn(missing_debug_implementations)]

mod client;
mod conn;
mod error;
mod params;
#[cfg(feature = "tls")]
mod tls;
mod types;
mod wire;

pub use client::{Client, IntoAddresses, Options};
pub use error::{Error, Result};
pub use params::Params;
pub use types::{Op, Row, SqlResult, Versioned};
pub use wire::{MAX_BATCH, MAX_FRAME, MAX_GET_MANY, MAX_VALUE};

/// The `serde_json` this crate uses (for `Value`, `Map` and `json!`).
pub use serde_json;

/// This client's version (the KestroDB release it was written for).
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

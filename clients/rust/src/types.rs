//! Results and key/value operations.

use crate::error::{Error, Result};
use serde::de::DeserializeOwned;
use serde::Serialize;
use serde_json::{Map, Value};

/// One row: column name to value, in `SELECT` order. Values are JSON: strings, integers
/// (`i64`/`u64`), floats, booleans, null, and arrays/objects for JSON columns.
pub type Row = Map<String, Value>;

/// The result of one SQL statement.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct SqlResult {
    /// The rows of a statement that returns rows (`SELECT`, ... and writes with `RETURNING`),
    /// even zero of them; `None` for a statement without rows.
    pub rows: Option<Vec<Row>>,
    /// Rows changed by a write (0 when the result has none).
    pub changes: u64,
    /// The id of the row an `INSERT` created (an integer, or a string for text ids).
    pub last_insert_id: Option<Value>,
}

impl SqlResult {
    /// True when the statement returned rows (even zero of them).
    pub fn has_rows(&self) -> bool {
        self.rows.is_some()
    }

    /// The column names, in `SELECT` order, of the first row (empty without rows).
    pub fn columns(&self) -> Vec<&str> {
        match self.rows.as_ref().and_then(|r| r.first()) {
            Some(row) => row.keys().map(String::as_str).collect(),
            None => Vec::new(),
        }
    }

    /// The rows, or an `INPUT` error for a statement without rows.
    pub fn into_rows(self) -> Result<Vec<Row>> {
        self.rows.ok_or_else(|| {
            Error::Input("INPUT query() needs a statement that returns rows; use execute()".into())
        })
    }

    /// The result as the server sent it, and whether it had a `lastInsertId` field.
    pub(crate) fn from_json(value: Value) -> Result<(SqlResult, bool)> {
        let Value::Object(mut obj) = value else {
            return Err(Error::Io(
                "IO the server's SQL result is not a JSON object".into(),
            ));
        };
        let rows = match obj.remove("rows") {
            None => None,
            Some(Value::Array(items)) => Some(
                items
                    .into_iter()
                    .map(|r| match r {
                        Value::Object(m) => Ok(m),
                        _ => Err(Error::Io(
                            "IO a row of the server's SQL result is not a JSON object".into(),
                        )),
                    })
                    .collect::<Result<Vec<Row>>>()?,
            ),
            Some(_) => {
                return Err(Error::Io(
                    "IO the server's SQL rows are not a JSON array".into(),
                ))
            }
        };
        let changes = obj.get("changes").and_then(Value::as_u64).unwrap_or(0);
        let has_id = obj.contains_key("lastInsertId");
        let last_insert_id = obj.remove("lastInsertId").filter(|v| !v.is_null());
        Ok((
            SqlResult {
                rows,
                changes,
                last_insert_id,
            },
            has_id,
        ))
    }
}

/// A key's value and the version that wrote it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Versioned<T = Value> {
    /// The version (transaction id) that wrote this value.
    pub version: u64,
    /// The value (JSON).
    pub value: T,
}

impl Versioned<Value> {
    /// The value converted to `T` (any [`DeserializeOwned`] type).
    pub fn into_typed<T: DeserializeOwned>(self) -> Result<Versioned<T>> {
        let value = serde_json::from_value(self.value)
            .map_err(|e| Error::Input(format!("INPUT cannot convert the value: {e}")))?;
        Ok(Versioned {
            version: self.version,
            value,
        })
    }
}

/// One operation of an all-or-nothing [`Client::batch`](crate::Client::batch).
#[derive(Debug, Clone, PartialEq)]
pub struct Op {
    pub(crate) set: bool,
    pub(crate) collection: String,
    pub(crate) key: String,
    /// The value as JSON bytes (empty for a delete), or why it could not be encoded.
    pub(crate) value: std::result::Result<Vec<u8>, Error>,
    pub(crate) expected_version: Option<u64>,
}

impl Op {
    /// Stores `value` (any [`Serialize`] type, as JSON) at `collection`/`key`.
    pub fn set(collection: impl Into<String>, key: impl Into<String>, value: impl Serialize) -> Op {
        let value =
            serde_json::to_vec(&value).map_err(|e| Error::Input(format!("INPUT value: {e}")));
        Op {
            set: true,
            collection: collection.into(),
            key: key.into(),
            value,
            expected_version: None,
        }
    }

    /// Deletes `collection`/`key` (deleting a missing key is not an error).
    pub fn delete(collection: impl Into<String>, key: impl Into<String>) -> Op {
        Op {
            set: false,
            collection: collection.into(),
            key: key.into(),
            value: Ok(Vec::new()),
            expected_version: None,
        }
    }

    /// Only if the key is at `version` (0: the key must not exist); otherwise the whole batch
    /// fails with `CONFLICT`.
    pub fn if_version(mut self, version: u64) -> Op {
        self.expected_version = Some(version);
        self
    }

    /// True for a set, false for a delete.
    pub fn is_set(&self) -> bool {
        self.set
    }

    pub fn collection(&self) -> &str {
        &self.collection
    }

    pub fn key(&self) -> &str {
        &self.key
    }

    pub fn expected_version(&self) -> Option<u64> {
        self.expected_version
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn select_result_keeps_column_order() {
        let v: Value =
            serde_json::from_str(r#"{"rows":[{"z":1,"a":"x","m":null},{"z":2,"a":"y","m":true}]}"#)
                .unwrap();
        let (r, has_id) = SqlResult::from_json(v).unwrap();
        assert!(!has_id);
        assert!(r.has_rows());
        assert_eq!(r.columns(), vec!["z", "a", "m"]);
        assert_eq!(r.changes, 0);
        let rows = r.into_rows().unwrap();
        assert_eq!(rows[1]["a"], json!("y"));
        assert_eq!(rows[0].keys().collect::<Vec<_>>(), vec!["z", "a", "m"]);
    }

    #[test]
    fn write_result() {
        let (r, has_id) = SqlResult::from_json(json!({"changes": 3, "lastInsertId": 7})).unwrap();
        assert!(has_id);
        assert!(!r.has_rows());
        assert!(r.columns().is_empty());
        assert_eq!(r.changes, 3);
        assert_eq!(r.last_insert_id, Some(json!(7)));
        let (r, has_id) =
            SqlResult::from_json(json!({"changes": 0, "lastInsertId": null})).unwrap();
        assert!(has_id);
        assert_eq!(r.last_insert_id, None);
        assert_eq!(r.clone().into_rows().unwrap_err().code(), "INPUT");
        let (empty, _) = SqlResult::from_json(json!({"rows": []})).unwrap();
        assert!(empty.has_rows());
        assert!(empty.columns().is_empty());
    }

    #[test]
    fn malformed_results() {
        assert_eq!(SqlResult::from_json(json!([1])).unwrap_err().code(), "IO");
        assert_eq!(
            SqlResult::from_json(json!({"rows": 1})).unwrap_err().code(),
            "IO"
        );
        assert_eq!(
            SqlResult::from_json(json!({"rows": [1]}))
                .unwrap_err()
                .code(),
            "IO"
        );
    }

    #[test]
    fn ops() {
        let s = Op::set("kv", "k", json!({"a": 1})).if_version(5);
        assert!(s.is_set());
        assert_eq!(s.value, Ok(br#"{"a":1}"#.to_vec()));
        assert_eq!(s.expected_version(), Some(5));
        assert_eq!((s.collection(), s.key()), ("kv", "k"));
        let d = Op::delete("kv", "k");
        assert!(!d.is_set());
        assert_eq!(d.expected_version(), None);
    }

    #[test]
    fn typed_versioned() {
        let v = Versioned {
            version: 3,
            value: json!([1, 2]),
        };
        let t: Versioned<Vec<i32>> = v.clone().into_typed().unwrap();
        assert_eq!(
            t,
            Versioned {
                version: 3,
                value: vec![1, 2]
            }
        );
        assert_eq!(v.into_typed::<String>().unwrap_err().code(), "INPUT");
    }
}

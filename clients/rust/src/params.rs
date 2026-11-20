//! SQL parameters.

use crate::error::{Error, Result};
use serde::Serialize;
use serde_json::{Map, Value};

const MIXED: &str = "INPUT named and positional parameters cannot be mixed";

/// The parameters of one SQL statement: positional for `?`, `?N` and `$N`, or named for
/// `:name` and `@name`.
///
/// Build them with [`params!`](crate::params!) / [`named_params!`](crate::named_params!)
/// (any [`Serialize`] values), [`Params::positional`] / [`Params::named`], or convert a
/// `serde_json` value: `()` and `Value::Null` are no parameters, a `Vec<Value>`, slice or JSON
/// array is positional, a `Map` or JSON object is named, and any other JSON value is one
/// positional parameter.
///
/// Every value is sent as JSON: strings, numbers, booleans, `None`/null; arrays, maps and
/// structs are stored as JSON. Non-finite floats (NaN, infinities) become null, as
/// `serde_json` serializes them.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Params(Kind);

#[derive(Debug, Clone, Default, PartialEq)]
enum Kind {
    #[default]
    Empty,
    Positional(Vec<Value>),
    Named(Map<String, Value>),
    Invalid(String),
}

fn to_value(value: impl Serialize) -> std::result::Result<Value, String> {
    serde_json::to_value(value).map_err(|e| format!("INPUT SQL parameter: {e}"))
}

fn strip(name: &str) -> &str {
    name.strip_prefix(':')
        .or_else(|| name.strip_prefix('@'))
        .unwrap_or(name)
}

impl Params {
    /// No parameters.
    pub fn new() -> Params {
        Params::default()
    }

    /// Positional parameters from any serializable values of one type.
    pub fn positional<I, T>(values: I) -> Params
    where
        I: IntoIterator<Item = T>,
        T: Serialize,
    {
        let mut p = Params::new();
        for v in values {
            p.push(v);
        }
        p
    }

    /// Named parameters; names may include the leading `:` or `@`.
    pub fn named<I, K, T>(pairs: I) -> Params
    where
        I: IntoIterator<Item = (K, T)>,
        K: AsRef<str>,
        T: Serialize,
    {
        let mut p = Params::new();
        for (k, v) in pairs {
            p.insert(k.as_ref(), v);
        }
        p
    }

    /// Appends a positional parameter.
    pub fn push(&mut self, value: impl Serialize) -> &mut Params {
        let v = match to_value(value) {
            Ok(v) => v,
            Err(e) => {
                self.invalid(e);
                return self;
            }
        };
        match &mut self.0 {
            Kind::Positional(list) => list.push(v),
            Kind::Empty => self.0 = Kind::Positional(vec![v]),
            Kind::Named(_) => self.invalid(MIXED.to_string()),
            Kind::Invalid(_) => {}
        }
        self
    }

    /// Sets a named parameter (`name`, `:name` or `@name`).
    pub fn insert(&mut self, name: &str, value: impl Serialize) -> &mut Params {
        let v = match to_value(value) {
            Ok(v) => v,
            Err(e) => {
                self.invalid(e);
                return self;
            }
        };
        let name = strip(name).to_string();
        match &mut self.0 {
            Kind::Named(map) => {
                map.insert(name, v);
            }
            Kind::Empty => {
                let mut map = Map::new();
                map.insert(name, v);
                self.0 = Kind::Named(map);
            }
            Kind::Positional(_) => self.invalid(MIXED.to_string()),
            Kind::Invalid(_) => {}
        }
        self
    }

    /// True when there are no parameters.
    pub fn is_empty(&self) -> bool {
        match &self.0 {
            Kind::Empty => true,
            Kind::Positional(v) => v.is_empty(),
            Kind::Named(m) => m.is_empty(),
            Kind::Invalid(_) => false,
        }
    }

    fn invalid(&mut self, message: String) {
        if !matches!(self.0, Kind::Invalid(_)) {
            self.0 = Kind::Invalid(message);
        }
    }

    /// The JSON the server receives: an array, an object, or `[]`.
    pub(crate) fn to_json(&self) -> Result<Vec<u8>> {
        let json = match &self.0 {
            Kind::Empty => return Ok(b"[]".to_vec()),
            Kind::Positional(v) => serde_json::to_vec(v),
            Kind::Named(m) => serde_json::to_vec(m),
            Kind::Invalid(m) => return Err(Error::Input(m.clone())),
        };
        json.map_err(|e| Error::Input(format!("INPUT SQL parameters: {e}")))
    }
}

impl From<()> for Params {
    fn from(_: ()) -> Params {
        Params::new()
    }
}

impl From<&Params> for Params {
    fn from(p: &Params) -> Params {
        p.clone()
    }
}

impl From<Vec<Value>> for Params {
    fn from(values: Vec<Value>) -> Params {
        if values.is_empty() {
            Params::new()
        } else {
            Params(Kind::Positional(values))
        }
    }
}

impl From<&[Value]> for Params {
    fn from(values: &[Value]) -> Params {
        Params::from(values.to_vec())
    }
}

impl<const N: usize> From<[Value; N]> for Params {
    fn from(values: [Value; N]) -> Params {
        Params::from(values.to_vec())
    }
}

impl From<Map<String, Value>> for Params {
    fn from(map: Map<String, Value>) -> Params {
        if map.is_empty() {
            return Params::new();
        }
        Params(Kind::Named(
            map.into_iter()
                .map(|(k, v)| (strip(&k).to_string(), v))
                .collect(),
        ))
    }
}

impl From<Value> for Params {
    fn from(value: Value) -> Params {
        match value {
            Value::Null => Params::new(),
            Value::Array(items) => Params::from(items),
            Value::Object(map) => Params::from(map),
            other => Params(Kind::Positional(vec![other])),
        }
    }
}

/// Positional SQL parameters (`?`, `?N`, `$N`) from values of any [`Serialize`] types:
/// `params![name, 42, 3.5, true, None::<i64>, json!({"a": 1})]`. Values are borrowed.
#[macro_export]
macro_rules! params {
    () => {
        $crate::Params::new()
    };
    ($($value:expr),+ $(,)?) => {{
        let mut p = $crate::Params::new();
        $( p.push(&$value); )+
        p
    }};
}

/// Named SQL parameters (`:name`, `@name`): `named_params! { "city" => "Kandy", ":age" => 30 }`.
/// Values are borrowed.
#[macro_export]
macro_rules! named_params {
    () => {
        $crate::Params::new()
    };
    ($($name:expr => $value:expr),+ $(,)?) => {{
        let mut p = $crate::Params::new();
        $( p.insert($name, &$value); )+
        p
    }};
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::collections::BTreeMap;

    fn text(p: impl Into<Params>) -> String {
        String::from_utf8(p.into().to_json().unwrap()).unwrap()
    }

    #[test]
    fn empty() {
        assert_eq!(text(()), "[]");
        assert_eq!(text(Value::Null), "[]");
        assert_eq!(text(crate::params![]), "[]");
        assert_eq!(text(Vec::<Value>::new()), "[]");
        assert!(Params::new().is_empty());
    }

    #[test]
    fn positional_of_every_type() {
        let name = String::from("Ann");
        let p = crate::params![
            name,
            "x",
            42,
            -7i8,
            1.5f32,
            0.1,
            true,
            None::<i64>,
            Some("s"),
            'c',
            (),
            vec![1, 2],
            json!({"a": [1, null]})
        ];
        assert_eq!(
            text(&p),
            r#"["Ann","x",42,-7,1.5,0.1,true,null,"s","c",null,[1,2],{"a":[1,null]}]"#
        );
        assert_eq!(name, "Ann"); // borrowed, not moved
        assert_eq!(
            text(crate::params![u64::MAX, i64::MIN]),
            "[18446744073709551615,-9223372036854775808]"
        );
        assert_eq!(
            text(crate::params!["quote\" \\ \n \u{1} é 😀"]),
            r#"["quote\" \\ \n \u0001 é 😀"]"#
        );
        assert_eq!(text(json!([1, "a"])), r#"[1,"a"]"#);
        assert_eq!(text(vec![json!(1), json!(2)]), "[1,2]");
        assert_eq!(text([json!("a")]), r#"["a"]"#);
        assert_eq!(text(&[json!(true)][..]), "[true]");
        assert_eq!(text(json!(5)), "[5]");
        assert_eq!(text(Params::positional(["a", "b"])), r#"["a","b"]"#);
        assert_eq!(text(crate::params![f64::NAN]), "[null]");
    }

    #[test]
    fn named_strips_prefixes_and_keeps_order() {
        let p = crate::named_params! { "email" => "a@x.org", ":name" => "Ann", "@age" => 31 };
        assert_eq!(text(p), r#"{"email":"a@x.org","name":"Ann","age":31}"#);
        assert_eq!(
            text(json!({":a": 1, "@b": 2, "c": 3})),
            r#"{"a":1,"b":2,"c":3}"#
        );
        assert_eq!(text(Params::named([("min", 26)])), r#"{"min":26}"#);
        let mut m = BTreeMap::new();
        m.insert("k", vec![1]);
        assert_eq!(text(crate::params![m]), r#"[{"k":[1]}]"#);
    }

    #[test]
    fn mixing_is_an_input_error() {
        let mut p = crate::params![1];
        p.insert("a", 2);
        assert!(matches!(p.to_json(), Err(Error::Input(m)) if m == MIXED));
        let mut q = crate::named_params! { "a" => 1 };
        q.push(2);
        assert!(matches!(q.to_json(), Err(Error::Input(_))));
    }

    #[test]
    fn unserializable_is_an_input_error() {
        let mut bad = BTreeMap::new();
        bad.insert(vec![1u8], 1);
        let p = crate::params![bad];
        let e = p.to_json().unwrap_err();
        assert_eq!(e.code(), "INPUT");
        assert!(e.message().starts_with("INPUT SQL parameter"));
    }
}

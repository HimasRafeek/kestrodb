package kestrodb

import (
	"bytes"
	"database/sql/driver"
	"encoding"
	"encoding/json"
	"math"
	"sort"
	"strconv"
	"time"
	"unicode/utf16"
	"unicode/utf8"
)

// JSON as KestroDB stores it. Decoding gives nil, bool, string, int64 (an integer that
// fits), float64 (a number with a fraction or exponent), json.Number (an integer beyond
// int64), []any and map[string]any. Encoding takes those and more (see encodeValue).

const maxDepth = 1000

// ---------------------------------------------------------------------- decoding

type decoder struct {
	b     []byte
	p     int
	depth int
}

// parseJSON parses one whole JSON text.
func parseJSON(b []byte) (any, error) {
	d := decoder{b: b}
	v, err := d.value()
	if err != nil {
		return nil, err
	}
	d.space()
	if d.p != len(d.b) {
		return nil, d.fail("trailing characters")
	}
	return v, nil
}

func (d *decoder) fail(what string) error {
	return inputError("invalid JSON: " + what + " at offset " + strconv.Itoa(d.p))
}

func (d *decoder) space() {
	for d.p < len(d.b) {
		switch d.b[d.p] {
		case ' ', '\t', '\n', '\r':
			d.p++
		default:
			return
		}
	}
}

func (d *decoder) value() (any, error) {
	d.space()
	if d.p >= len(d.b) {
		return nil, d.fail("unexpected end")
	}
	switch c := d.b[d.p]; {
	case c == '{':
		m := map[string]any{}
		err := d.object(func(k string) error {
			v, err := d.value()
			m[k] = v
			return err
		})
		return m, err
	case c == '[':
		return d.array()
	case c == '"':
		return d.str()
	case c == 't':
		return true, d.literal("true")
	case c == 'f':
		return false, d.literal("false")
	case c == 'n':
		return nil, d.literal("null")
	case c == '-' || c >= '0' && c <= '9':
		return d.number()
	}
	return nil, d.fail("unexpected character")
}

func (d *decoder) literal(word string) error {
	if !bytes.HasPrefix(d.b[d.p:], []byte(word)) {
		return d.fail("invalid literal")
	}
	d.p += len(word)
	return nil
}

func (d *decoder) enter() error {
	d.depth++
	if d.depth > maxDepth {
		return d.fail("nested too deeply")
	}
	return nil
}

// object calls member for each key; member must consume the value.
func (d *decoder) object(member func(key string) error) error {
	if err := d.enter(); err != nil {
		return err
	}
	defer func() { d.depth-- }()
	d.p++ // {
	d.space()
	if d.p < len(d.b) && d.b[d.p] == '}' {
		d.p++
		return nil
	}
	for {
		d.space()
		if d.p >= len(d.b) || d.b[d.p] != '"' {
			return d.fail("expected a key")
		}
		k, err := d.str()
		if err != nil {
			return err
		}
		d.space()
		if d.p >= len(d.b) || d.b[d.p] != ':' {
			return d.fail("expected ':'")
		}
		d.p++
		if err := member(k); err != nil {
			return err
		}
		d.space()
		if d.p >= len(d.b) {
			return d.fail("unexpected end")
		}
		if d.b[d.p] == ',' {
			d.p++
			continue
		}
		if d.b[d.p] == '}' {
			d.p++
			return nil
		}
		return d.fail("expected ',' or '}'")
	}
}

func (d *decoder) array() ([]any, error) {
	if err := d.enter(); err != nil {
		return nil, err
	}
	defer func() { d.depth-- }()
	d.p++ // [
	out := []any{}
	d.space()
	if d.p < len(d.b) && d.b[d.p] == ']' {
		d.p++
		return out, nil
	}
	for {
		v, err := d.value()
		if err != nil {
			return nil, err
		}
		out = append(out, v)
		d.space()
		if d.p >= len(d.b) {
			return nil, d.fail("unexpected end")
		}
		if d.b[d.p] == ',' {
			d.p++
			continue
		}
		if d.b[d.p] == ']' {
			d.p++
			return out, nil
		}
		return nil, d.fail("expected ',' or ']'")
	}
}

func (d *decoder) str() (string, error) {
	d.p++ // "
	start := d.p
	for d.p < len(d.b) { // fast path: no escapes
		c := d.b[d.p]
		if c == '"' {
			s := string(d.b[start:d.p])
			d.p++
			return s, nil
		}
		if c == '\\' {
			break
		}
		if c < 0x20 {
			return "", d.fail("control character in a string")
		}
		d.p++
	}
	buf := append([]byte(nil), d.b[start:d.p]...)
	for d.p < len(d.b) {
		c := d.b[d.p]
		switch {
		case c == '"':
			d.p++
			return string(buf), nil
		case c < 0x20:
			return "", d.fail("control character in a string")
		case c != '\\':
			buf = append(buf, c)
			d.p++
			continue
		}
		d.p++
		if d.p >= len(d.b) {
			break
		}
		e := d.b[d.p]
		d.p++
		switch e {
		case '"', '\\', '/':
			buf = append(buf, e)
		case 'b':
			buf = append(buf, '\b')
		case 'f':
			buf = append(buf, '\f')
		case 'n':
			buf = append(buf, '\n')
		case 'r':
			buf = append(buf, '\r')
		case 't':
			buf = append(buf, '\t')
		case 'u':
			r, ok := d.hex4()
			if !ok {
				return "", d.fail("invalid \\u escape")
			}
			if utf16.IsSurrogate(r) {
				r2 := rune(-1)
				if d.p+1 < len(d.b) && d.b[d.p] == '\\' && d.b[d.p+1] == 'u' {
					save := d.p
					d.p += 2
					if x, ok := d.hex4(); ok {
						r2 = x
					} else {
						return "", d.fail("invalid \\u escape")
					}
					if utf16.DecodeRune(r, r2) == utf8.RuneError {
						d.p = save // a lone surrogate, then another escape
						r2 = -1
					}
				}
				if r2 >= 0 {
					r = utf16.DecodeRune(r, r2)
				} else {
					r = utf8.RuneError
				}
			}
			buf = utf8.AppendRune(buf, r)
		default:
			return "", d.fail("invalid escape")
		}
	}
	return "", d.fail("unterminated string")
}

func (d *decoder) hex4() (rune, bool) {
	if d.p+4 > len(d.b) {
		return 0, false
	}
	var r rune
	for _, c := range d.b[d.p : d.p+4] {
		r <<= 4
		switch {
		case c >= '0' && c <= '9':
			r |= rune(c - '0')
		case c >= 'a' && c <= 'f':
			r |= rune(c - 'a' + 10)
		case c >= 'A' && c <= 'F':
			r |= rune(c - 'A' + 10)
		default:
			return 0, false
		}
	}
	d.p += 4
	return r, true
}

func (d *decoder) number() (any, error) {
	start := d.p
	digits := func() int {
		n := 0
		for d.p < len(d.b) && d.b[d.p] >= '0' && d.b[d.p] <= '9' {
			d.p++
			n++
		}
		return n
	}
	if d.b[d.p] == '-' {
		d.p++
	}
	if d.p < len(d.b) && d.b[d.p] == '0' {
		d.p++
		if d.p < len(d.b) && d.b[d.p] >= '0' && d.b[d.p] <= '9' {
			return nil, d.fail("leading zero")
		}
	} else if digits() == 0 {
		return nil, d.fail("invalid number")
	}
	integral := true
	if d.p < len(d.b) && d.b[d.p] == '.' {
		d.p++
		integral = false
		if digits() == 0 {
			return nil, d.fail("invalid number")
		}
	}
	if d.p < len(d.b) && (d.b[d.p] == 'e' || d.b[d.p] == 'E') {
		d.p++
		integral = false
		if d.p < len(d.b) && (d.b[d.p] == '+' || d.b[d.p] == '-') {
			d.p++
		}
		if digits() == 0 {
			return nil, d.fail("invalid number")
		}
	}
	text := string(d.b[start:d.p])
	if integral {
		if n, err := strconv.ParseInt(text, 10, 64); err == nil {
			return n, nil
		}
		return json.Number(text), nil
	}
	f, err := strconv.ParseFloat(text, 64)
	if err != nil {
		return json.Number(text), nil // beyond float64
	}
	return f, nil
}

// sqlResult is the server's reply to a statement: {"rows":[...]} or {"changes":n,"lastInsertId":id}.
func parseResult(b []byte) (*Result, error) {
	d := decoder{b: b}
	d.space()
	if d.p >= len(d.b) || d.b[d.p] != '{' {
		return nil, d.fail("expected an object")
	}
	r := &Result{}
	var seen map[string]struct{}
	err := d.object(func(k string) error {
		switch k {
		case "rows":
			d.space()
			if d.p >= len(d.b) || d.b[d.p] != '[' {
				v, err := d.value()
				r.extra(k, v)
				return err
			}
			if err := d.enter(); err != nil {
				return err
			}
			d.p++
			r.Rows = []map[string]any{}
			d.space()
			if d.p < len(d.b) && d.b[d.p] == ']' {
				d.p++
				d.depth--
				return nil
			}
			for {
				d.space()
				if d.p >= len(d.b) || d.b[d.p] != '{' {
					return d.fail("expected a row object")
				}
				row := make(map[string]any, len(r.Columns))
				i := 0
				err := d.object(func(col string) error {
					// Columns in order of first appearance (a row may lack a NULL column).
					if i < len(r.Columns) && r.Columns[i] == col {
						i++
					} else {
						if seen == nil {
							seen = make(map[string]struct{}, len(r.Columns)+1)
							for _, c := range r.Columns {
								seen[c] = struct{}{}
							}
						}
						if _, ok := seen[col]; !ok {
							seen[col] = struct{}{}
							r.Columns = append(r.Columns, col)
						}
					}
					v, err := d.value()
					row[col] = v
					return err
				})
				if err != nil {
					return err
				}
				r.Rows = append(r.Rows, row)
				d.space()
				if d.p >= len(d.b) {
					return d.fail("unexpected end")
				}
				if d.b[d.p] == ',' {
					d.p++
					continue
				}
				if d.b[d.p] == ']' {
					d.p++
					d.depth--
					return nil
				}
				return d.fail("expected ',' or ']'")
			}
		default:
			v, err := d.value()
			if err != nil {
				return err
			}
			r.extra(k, v)
			return nil
		}
	})
	if err != nil {
		return nil, err
	}
	d.space()
	if d.p != len(d.b) {
		return nil, d.fail("trailing characters")
	}
	return r, nil
}

// ---------------------------------------------------------------------- encoding

// encodeValue is v as JSON. It takes nil, bool, every integer and float type (NaN and
// infinities are refused), string (valid UTF-8), []byte (as text; must be UTF-8),
// json.Number, json.RawMessage, time.Time (RFC 3339 text), []any, map[string]any, Named,
// driver.Valuer, json.Marshaler and encoding.TextMarshaler; anything else (structs,
// typed slices and maps, pointers) through encoding/json.
func encodeValue(v any) ([]byte, error) {
	return appendJSON(make([]byte, 0, 64), v, 0)
}

func appendJSON(b []byte, v any, depth int) ([]byte, error) {
	if depth > maxDepth {
		return nil, inputError("a value is nested too deeply (or contains itself)")
	}
	switch x := v.(type) {
	case nil:
		return append(b, "null"...), nil
	case bool:
		return strconv.AppendBool(b, x), nil
	case string:
		return appendString(b, x)
	case int:
		return strconv.AppendInt(b, int64(x), 10), nil
	case int8:
		return strconv.AppendInt(b, int64(x), 10), nil
	case int16:
		return strconv.AppendInt(b, int64(x), 10), nil
	case int32:
		return strconv.AppendInt(b, int64(x), 10), nil
	case int64:
		return strconv.AppendInt(b, x, 10), nil
	case uint:
		return strconv.AppendUint(b, uint64(x), 10), nil
	case uint8:
		return strconv.AppendUint(b, uint64(x), 10), nil
	case uint16:
		return strconv.AppendUint(b, uint64(x), 10), nil
	case uint32:
		return strconv.AppendUint(b, uint64(x), 10), nil
	case uint64:
		return strconv.AppendUint(b, x, 10), nil
	case float64:
		return appendFloat(b, x, 64)
	case float32:
		return appendFloat(b, float64(x), 32)
	case json.Number:
		if !isNumberText(string(x)) {
			return nil, inputError("invalid json.Number " + strconv.Quote(string(x)))
		}
		return append(b, x...), nil
	case json.RawMessage:
		if !json.Valid(x) {
			return nil, inputError("invalid json.RawMessage")
		}
		return append(b, bytes.TrimSpace(x)...), nil
	case []byte:
		if !utf8.Valid(x) {
			return nil, inputError("a []byte value must be UTF-8 text (JSON has no binary type)")
		}
		return appendString(b, string(x))
	case time.Time:
		return appendString(b, x.Format(time.RFC3339Nano))
	case []any:
		b = append(b, '[')
		for i, e := range x {
			if i > 0 {
				b = append(b, ',')
			}
			var err error
			if b, err = appendJSON(b, e, depth+1); err != nil {
				return nil, err
			}
		}
		return append(b, ']'), nil
	case map[string]any:
		return appendMap(b, x, depth)
	case Named:
		return appendMap(b, x, depth)
	case driver.Valuer:
		dv, err := x.Value()
		if err != nil {
			return nil, inputError("a driver.Valuer failed: " + err.Error())
		}
		if _, again := dv.(driver.Valuer); again {
			return nil, inputError("a driver.Valuer returned another Valuer")
		}
		return appendJSON(b, dv, depth+1)
	case json.Marshaler:
		return appendMarshaled(b, v)
	case encoding.TextMarshaler:
		t, err := x.MarshalText()
		if err != nil {
			return nil, inputError("MarshalText failed: " + err.Error())
		}
		return appendString(b, string(t))
	}
	return appendMarshaled(b, v)
}

func isNumberText(s string) bool {
	d := decoder{b: []byte(s)}
	if s == "" || !(s[0] == '-' || s[0] >= '0' && s[0] <= '9') {
		return false
	}
	_, err := d.number()
	return err == nil && d.p == len(s)
}

// appendMap writes keys in sorted order (as encoding/json does).
func appendMap(b []byte, m map[string]any, depth int) ([]byte, error) {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	b = append(b, '{')
	for i, k := range keys {
		if i > 0 {
			b = append(b, ',')
		}
		var err error
		if b, err = appendString(b, k); err != nil {
			return nil, err
		}
		b = append(b, ':')
		if b, err = appendJSON(b, m[k], depth+1); err != nil {
			return nil, err
		}
	}
	return append(b, '}'), nil
}

func appendMarshaled(b []byte, v any) ([]byte, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		return nil, inputError("cannot encode a value of type " + typeName(v) + " as JSON: " + err.Error())
	}
	return append(b, bytes.TrimRight(buf.Bytes(), "\n")...), nil
}

// appendFloat keeps a float a float: 3.0 is written 3.0 (not 3), so it reads back as float64.
func appendFloat(b []byte, f float64, bits int) ([]byte, error) {
	if math.IsNaN(f) || math.IsInf(f, 0) {
		return nil, inputError("NaN and infinite numbers are not JSON")
	}
	start := len(b)
	abs := math.Abs(f)
	format := byte('f')
	if abs != 0 && (bits == 64 && (abs < 1e-6 || abs >= 1e21) || bits == 32 && (float32(abs) < 1e-6 || float32(abs) >= 1e21)) {
		format = 'e'
	}
	b = strconv.AppendFloat(b, f, format, -1, bits)
	if format == 'e' {
		// 1e-07 -> 1e-7, as JavaScript and encoding/json write it
		n := len(b)
		if n-start >= 4 && b[n-4] == 'e' && b[n-3] == '-' && b[n-2] == '0' {
			b[n-2] = b[n-1]
			b = b[:n-1]
		}
		return b, nil
	}
	if bytes.IndexByte(b[start:], '.') < 0 {
		b = append(b, '.', '0')
	}
	return b, nil
}

const hexDigits = "0123456789abcdef"

func appendString(b []byte, s string) ([]byte, error) {
	if !utf8.ValidString(s) {
		return nil, inputError("a string is not valid UTF-8")
	}
	b = append(b, '"')
	start := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 0x20 && c != '"' && c != '\\' {
			continue
		}
		b = append(b, s[start:i]...)
		switch c {
		case '"', '\\':
			b = append(b, '\\', c)
		case '\n':
			b = append(b, '\\', 'n')
		case '\r':
			b = append(b, '\\', 'r')
		case '\t':
			b = append(b, '\\', 't')
		case '\b':
			b = append(b, '\\', 'b')
		case '\f':
			b = append(b, '\\', 'f')
		default:
			b = append(b, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&15])
		}
		start = i + 1
	}
	b = append(b, s[start:]...)
	return append(b, '"'), nil
}

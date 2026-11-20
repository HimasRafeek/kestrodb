package kestrodb

import (
	"encoding/json"
	"errors"
	"math"
	"reflect"
	"strings"
	"testing"
	"time"
)

func TestParseJSON(t *testing.T) {
	cases := []struct {
		in   string
		want any
	}{
		{`{"a":1,"b":[true,false,null],"c":-2.5e3,"d":"x"}`, map[string]any{"a": int64(1), "b": []any{true, false, nil}, "c": -2500.0, "d": "x"}},
		{`9223372036854775807`, int64(math.MaxInt64)},
		{`-9223372036854775808`, int64(math.MinInt64)},
		{`9223372036854775808`, json.Number("9223372036854775808")},
		{`0`, int64(0)},
		{`-0.0`, math.Copysign(0, -1)},
		{`1E2`, 100.0},
		{`1.5`, 1.5},
		{" \t\n[ ]\r\n", []any{}},
		{`{}`, map[string]any{}},
		{`"😀 é\n\t\"\\\/\b\f\r"`, "\U0001F600 é\n\t\"\\/\b\f\r"},
		{`"日本 😀"`, "日本 😀"},
		{`"\ud800x"`, "�x"}, // a lone surrogate
		{`"\ud800A"`, "�A"}, // a high surrogate, then not a low one
		{`{"a":{"b":{"c":[1,{"d":2}]}}}`, map[string]any{"a": map[string]any{"b": map[string]any{"c": []any{int64(1), map[string]any{"d": int64(2)}}}}}},
	}
	for _, c := range cases {
		got, err := parseJSON([]byte(c.in))
		if err != nil {
			t.Errorf("parse %q: %v", c.in, err)
			continue
		}
		if !reflect.DeepEqual(got, c.want) {
			t.Errorf("parse %q: got %#v, want %#v", c.in, got, c.want)
		}
	}
	if f, _ := parseJSON([]byte("-0.0")); !math.Signbit(f.(float64)) {
		t.Errorf("-0.0 lost its sign")
	}
	for _, bad := range []string{"", "{", "[1,]", `{"a" 1}`, "01", "1.", "-", "tru", `"\x"`, `"a`, "\"\x01\"", "[1] 2", "{a:1}", "NaN", `"\u12"`, "[", `{"a":1,}`, "+1", ".5", "1e", "nul"} {
		if _, err := parseJSON([]byte(bad)); !errors.Is(err, ErrInput) {
			t.Errorf("invalid JSON %q: got %v, want an INPUT error", bad, err)
		}
	}
	deep := strings.Repeat("[", 2000)
	if _, err := parseJSON([]byte(deep)); !errors.Is(err, ErrInput) {
		t.Errorf("too deep: %v", err)
	}
	ok := strings.Repeat("[", 999) + strings.Repeat("]", 999)
	if _, err := parseJSON([]byte(ok)); err != nil {
		t.Errorf("999 deep: %v", err)
	}
}

func TestParseResult(t *testing.T) {
	r, err := parseResult([]byte(`{"rows":[{"id":1,"name":"a","n":null},{"id":2,"extra":true,"name":"b"}]}`))
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(r.Columns, []string{"id", "name", "n", "extra"}) {
		t.Errorf("columns %v", r.Columns)
	}
	if len(r.Rows) != 2 || r.Rows[1]["extra"] != true || r.Rows[0]["n"] != nil {
		t.Errorf("rows %v", r.Rows)
	}
	if !reflect.DeepEqual(r.Values(), [][]any{{int64(1), "a", nil, nil}, {int64(2), "b", nil, true}}) {
		t.Errorf("values %v", r.Values())
	}
	if !r.HasRows() || r.hasID {
		t.Errorf("hasRows/hasID")
	}
	r, err = parseResult([]byte(`{"rows":[]}`))
	if err != nil || !r.HasRows() || len(r.Rows) != 0 || len(r.Columns) != 0 {
		t.Errorf("empty rows: %v %v", r, err)
	}
	r, err = parseResult([]byte(`{"changes":3,"lastInsertId":42}`))
	if err != nil || r.HasRows() || r.Changes != 3 || r.LastInsertID != int64(42) || !r.hasID {
		t.Errorf("write result: %+v %v", r, err)
	}
	r, err = parseResult([]byte(`{"changes":1,"lastInsertId":"u-1","other":[1]}`))
	if err != nil || r.LastInsertID != "u-1" || !reflect.DeepEqual(r.Extra, map[string]any{"other": []any{int64(1)}}) {
		t.Errorf("text id and extra: %+v %v", r, err)
	}
	for _, bad := range []string{`[]`, `{"rows":[1]}`, `{"rows":[{"a":1}`, `{"rows":[]} x`} {
		if _, err := parseResult([]byte(bad)); err == nil {
			t.Errorf("parseResult(%s) succeeded", bad)
		}
	}
}

type color int

func (c color) MarshalText() ([]byte, error) { return []byte([]string{"RED", "GREEN"}[c]), nil }

type point struct {
	X int    `json:"x"`
	Y string `json:"y,omitempty"`
}

func TestEncodeValue(t *testing.T) {
	enc := func(v any) string {
		t.Helper()
		b, err := encodeValue(v)
		if err != nil {
			t.Fatalf("encode %#v: %v", v, err)
		}
		return string(b)
	}
	eq := func(got, want string) {
		t.Helper()
		if got != want {
			t.Errorf("got %s want %s", got, want)
		}
	}
	eq(enc(map[string]any{"s": "q\"b\\n\n\x01\x1f 😀/<&>", "n": nil, "t": true}), `{"n":null,"s":"q\"b\\n\n\u0001\u001f`+" 😀/<&>"+`","t":true}`)
	eq(enc([]any{1, int8(-2), int16(3), int32(4), int64(5), uint(6), uint8(7), uint16(8), uint32(9), uint64(math.MaxUint64)}), "[1,-2,3,4,5,6,7,8,9,18446744073709551615]")
	eq(enc([]any{1.5, float32(0.1), 3.0, math.Copysign(0, -1), 1e21, 1e-7, 123456789.0, 0.000001}), "[1.5,0.1,3.0,-0.0,1e+21,1e-7,123456789.0,0.000001]")
	eq(enc(json.Number("12.50")), "12.50")
	eq(enc(json.RawMessage(` {"a": [1, 2]} `)), `{"a": [1, 2]}`)
	eq(enc([]byte("text")), `"text"`)
	eq(enc(time.Date(2026, 11, 20, 10, 15, 30, 500000000, time.UTC)), `"2026-11-20T10:15:30.5Z"`)
	eq(enc(color(1)), `"GREEN"`)
	eq(enc([]int{1, 2}), "[1,2]")
	eq(enc(map[string]int{"b": 2, "a": 1}), `{"a":1,"b":2}`)
	eq(enc(point{X: 1}), `{"x":1}`)
	eq(enc(&point{X: 2, Y: "<y>"}), `{"x":2,"y":"<y>"}`)
	eq(enc(Named{"a": 1}), `{"a":1}`)
	eq(enc([]any{}), "[]")
	eq(enc(map[string]any{}), "{}")
	var nilPtr *point
	eq(enc(nilPtr), "null")

	for _, bad := range []any{math.NaN(), math.Inf(1), float32(math.Inf(-1)), "\xff", []byte{0xff}, json.Number("1x"), json.Number(""), json.RawMessage("{"), make(chan int), func() {}, map[string]any{"x": math.NaN()}} {
		if _, err := encodeValue(bad); !errors.Is(err, ErrInput) {
			t.Errorf("encode %#v: got %v, want INPUT", bad, err)
		}
	}
	cycle := []any{nil}
	cycle[0] = cycle
	if _, err := encodeValue(cycle); !errors.Is(err, ErrInput) {
		t.Errorf("cycle: %v", err)
	}
	m := map[string]any{}
	m["self"] = m
	if _, err := encodeValue(m); !errors.Is(err, ErrInput) {
		t.Errorf("map cycle: %v", err)
	}

	// Round trip, UTF-8 included.
	v := map[string]any{"emoji": "😀", "nested": map[string]any{"a": []any{int64(1), 2.5, "x", map[string]any{}}}, "big": int64(math.MinInt64), "f": 3.0}
	b, _ := encodeValue(v)
	back, err := parseJSON(b)
	if err != nil || !reflect.DeepEqual(back, v) {
		t.Errorf("round trip: %#v %v", back, err)
	}
}

func TestSQLParams(t *testing.T) {
	check := func(args []any, want string) {
		t.Helper()
		b, err := sqlParams(args)
		if err != nil {
			t.Fatalf("%v: %v", args, err)
		}
		if string(b) != want {
			t.Errorf("got %s want %s", b, want)
		}
	}
	check(nil, "[]")
	check([]any{"a", 1, nil, true}, `["a",1,null,true]`)
	check([]any{[]any{1, 2}}, `[[1,2]]`)
	check([]any{Named{":a": 1, "@b": 2, "c": 3}}, `{"a":1,"b":2,"c":3}`)
	check([]any{map[string]any{"a": 1}}, `[{"a":1}]`) // a plain map is one JSON parameter
	check([]any{sqlNamed("x", 1), sqlNamed(":y", "z")}, `{"x":1,"y":"z"}`)
	if _, err := sqlParams([]any{sqlNamed("x", 1), 2}); !errors.Is(err, ErrInput) {
		t.Errorf("mixed: %v", err)
	}
	if _, err := sqlParams([]any{Named{"a": 1}, 2}); !errors.Is(err, ErrInput) {
		t.Errorf("Named not alone: %v", err)
	}
	_, err := sqlParams([]any{math.NaN()})
	if !errors.Is(err, ErrInput) || !strings.HasPrefix(err.Error(), "INPUT SQL parameters: ") {
		t.Errorf("NaN parameter: %v", err)
	}
	if p := sqlPayload("SELECT ?", []byte("[1]")); string(p) != "\x08\x00\x00\x00SELECT ?[1]" {
		t.Errorf("payload %q", p)
	}
}

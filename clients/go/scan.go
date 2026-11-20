package kestrodb

import (
	"database/sql"
	"database/sql/driver"
	"encoding/json"
	"math"
	"reflect"
	"strconv"
	"time"
)

func itoa(n int) string { return strconv.Itoa(n) }

// driverValue is a decoded JSON value as a database/sql driver.Value: JSON arrays and
// objects become their JSON text, json.Number its text.
func driverValue(v any) driver.Value {
	switch x := v.(type) {
	case nil, bool, int64, float64, string:
		return x
	case json.Number:
		return string(x)
	case []any, map[string]any:
		b, err := encodeValue(x)
		if err != nil {
			return nil
		}
		return string(b)
	}
	return v
}

// timeLayouts are the ISO forms a time is parsed from.
var timeLayouts = []string{
	time.RFC3339Nano,
	"2006-01-02T15:04:05.999999999",
	"2006-01-02 15:04:05.999999999Z07:00",
	"2006-01-02 15:04:05.999999999",
	"2006-01-02T15:04Z07:00",
	"2006-01-02T15:04",
	"2006-01-02",
}

// ParseTime parses ISO 8601 text as stored in DATE and TIMESTAMP columns (UTC unless it
// names an offset).
func ParseTime(s string) (time.Time, error) {
	for _, layout := range timeLayouts {
		if t, err := time.Parse(layout, s); err == nil {
			return t, nil
		}
	}
	return time.Time{}, inputError("cannot parse " + strconv.Quote(s) + " as a time")
}

var (
	scannerType = reflect.TypeOf((*sql.Scanner)(nil)).Elem()
	timeType    = reflect.TypeOf(time.Time{})
)

// assign stores a decoded JSON value in *dest.
func assign(dest any, src any) error {
	switch d := dest.(type) {
	case nil:
		return inputError("Scan: a nil destination")
	case sql.Scanner:
		return d.Scan(driverValue(src))
	case *any:
		*d = src
		return nil
	case *string:
		s, ok := asText(src)
		if !ok {
			return inputError("cannot store NULL in a *string (use *sql.NullString or **string)")
		}
		*d = s
		return nil
	case *[]byte:
		if src == nil {
			*d = nil
			return nil
		}
		s, _ := asText(src)
		*d = []byte(s)
		return nil
	case *json.RawMessage:
		b, err := encodeValue(src)
		if err != nil {
			return err
		}
		*d = b
		return nil
	case *time.Time:
		t, err := asTime(src)
		if err != nil {
			return err
		}
		*d = t
		return nil
	}
	rv := reflect.ValueOf(dest)
	if rv.Kind() != reflect.Pointer || rv.IsNil() {
		return inputError("Scan: the destination must be a non-nil pointer, not " + typeName(dest))
	}
	return assignValue(rv.Elem(), src)
}

func assignValue(dv reflect.Value, src any) error {
	if dv.CanAddr() && dv.Addr().Type().Implements(scannerType) {
		return dv.Addr().Interface().(sql.Scanner).Scan(driverValue(src))
	}
	if dv.Kind() == reflect.Pointer {
		if src == nil {
			dv.Set(reflect.Zero(dv.Type()))
			return nil
		}
		p := reflect.New(dv.Type().Elem())
		if err := assignValue(p.Elem(), src); err != nil {
			return err
		}
		dv.Set(p)
		return nil
	}
	if src == nil {
		switch dv.Kind() {
		case reflect.Interface, reflect.Map, reflect.Slice:
			dv.Set(reflect.Zero(dv.Type()))
			return nil
		}
		return inputError("cannot store NULL in a " + dv.Type().String() + " (use a pointer or a sql.Null* type)")
	}
	switch dv.Kind() {
	case reflect.Interface:
		if reflect.TypeOf(src).AssignableTo(dv.Type()) {
			dv.Set(reflect.ValueOf(src))
			return nil
		}
	case reflect.String:
		s, _ := asText(src)
		dv.SetString(s)
		return nil
	case reflect.Bool:
		b, err := asBool(src)
		if err != nil {
			return err
		}
		dv.SetBool(b)
		return nil
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		n, err := asInt(src)
		if err != nil {
			return err
		}
		if dv.OverflowInt(n) {
			return inputError(strconv.FormatInt(n, 10) + " overflows " + dv.Type().String())
		}
		dv.SetInt(n)
		return nil
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
		n, err := asUint(src)
		if err != nil {
			return err
		}
		if dv.OverflowUint(n) {
			return inputError(strconv.FormatUint(n, 10) + " overflows " + dv.Type().String())
		}
		dv.SetUint(n)
		return nil
	case reflect.Float32, reflect.Float64:
		f, err := asFloat(src)
		if err != nil {
			return err
		}
		dv.SetFloat(f)
		return nil
	case reflect.Struct:
		if dv.Type() == timeType {
			t, err := asTime(src)
			if err != nil {
				return err
			}
			dv.Set(reflect.ValueOf(t))
			return nil
		}
	}
	// A JSON value (or JSON text) into a struct, map, slice, ...: encoding/json.
	var raw []byte
	if s, ok := src.(string); ok && json.Valid([]byte(s)) && dv.Kind() != reflect.String {
		raw = []byte(s)
	} else {
		b, err := encodeValue(src)
		if err != nil {
			return err
		}
		raw = b
	}
	if err := json.Unmarshal(raw, dv.Addr().Interface()); err != nil {
		return inputError("cannot store " + typeName(src) + " in a " + dv.Type().String() + ": " + err.Error())
	}
	return nil
}

func asText(src any) (string, bool) {
	switch x := src.(type) {
	case nil:
		return "", false
	case string:
		return x, true
	case bool:
		return strconv.FormatBool(x), true
	case int64:
		return strconv.FormatInt(x, 10), true
	case float64:
		return strconv.FormatFloat(x, 'g', -1, 64), true
	case json.Number:
		return string(x), true
	}
	b, err := encodeValue(src)
	if err != nil {
		return "", false
	}
	return string(b), true
}

func asBool(src any) (bool, error) {
	switch x := src.(type) {
	case bool:
		return x, nil
	case int64:
		return x != 0, nil
	case float64:
		return x != 0, nil
	case string:
		if b, err := strconv.ParseBool(x); err == nil {
			return b, nil
		}
	}
	return false, inputError("cannot convert " + typeName(src) + " to bool")
}

func asInt(src any) (int64, error) {
	switch x := src.(type) {
	case int64:
		return x, nil
	case float64:
		if x == math.Trunc(x) && x >= -9.223372036854775808e18 && x < 9.223372036854775808e18 {
			return int64(x), nil
		}
		return 0, inputError("cannot convert " + strconv.FormatFloat(x, 'g', -1, 64) + " to an integer")
	case bool:
		if x {
			return 1, nil
		}
		return 0, nil
	case string, json.Number:
		s, _ := asText(x)
		n, err := strconv.ParseInt(s, 10, 64)
		if err != nil {
			return 0, inputError("cannot convert " + strconv.Quote(s) + " to an integer")
		}
		return n, nil
	}
	return 0, inputError("cannot convert " + typeName(src) + " to an integer")
}

func asUint(src any) (uint64, error) {
	switch x := src.(type) {
	case string, json.Number:
		s, _ := asText(x)
		n, err := strconv.ParseUint(s, 10, 64)
		if err != nil {
			return 0, inputError("cannot convert " + strconv.Quote(s) + " to an unsigned integer")
		}
		return n, nil
	}
	n, err := asInt(src)
	if err != nil {
		return 0, err
	}
	if n < 0 {
		return 0, inputError(strconv.FormatInt(n, 10) + " is negative")
	}
	return uint64(n), nil
}

func asFloat(src any) (float64, error) {
	switch x := src.(type) {
	case float64:
		return x, nil
	case int64:
		return float64(x), nil
	case string, json.Number:
		s, _ := asText(x)
		f, err := strconv.ParseFloat(s, 64)
		if err != nil {
			return 0, inputError("cannot convert " + strconv.Quote(s) + " to a float")
		}
		return f, nil
	}
	return 0, inputError("cannot convert " + typeName(src) + " to a float")
}

func asTime(src any) (time.Time, error) {
	switch x := src.(type) {
	case string:
		return ParseTime(x)
	case int64:
		return time.UnixMilli(x).UTC(), nil // unix milliseconds
	case nil:
		return time.Time{}, inputError("cannot store NULL in a time.Time (use *time.Time or sql.NullTime)")
	}
	return time.Time{}, inputError("cannot convert " + typeName(src) + " to a time")
}

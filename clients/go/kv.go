package kestrodb

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"strings"
	"unicode/utf8"
)

// Item is a key's value and version.
type Item struct {
	Version uint64 // the transaction that last wrote the key
	Value   any    // the value: nil, bool, int64, float64, json.Number, string, []any or map[string]any
	Raw     json.RawMessage
}

// Decode unmarshals the value into v (a struct, map, ...) with encoding/json.
func (it *Item) Decode(v any) error {
	if err := json.Unmarshal(it.Raw, v); err != nil {
		return inputError("Decode: " + err.Error())
	}
	return nil
}

// OpKind is what an Op does.
type OpKind uint8

const (
	OpDelete OpKind = 0
	OpSet    OpKind = 1
)

// AnyVersion as an Op's expected version: no condition.
const AnyVersion = ^uint64(0)

// Op is one operation of a Batch; make it with SetOp, SetOpIf, DeleteOp or DeleteOpIf.
type Op struct {
	Kind       OpKind
	Collection string
	Key        string
	Value      any // for OpSet
	// Expected makes the operation conditional: the key must be at this version (0: must
	// not exist), else the whole batch fails with ErrConflict. AnyVersion: no condition.
	Expected uint64
}

// SetOp stores value at collection/key.
func SetOp(collection, key string, value any) Op {
	return Op{Kind: OpSet, Collection: collection, Key: key, Value: value, Expected: AnyVersion}
}

// SetOpIf stores value only if the key is at expectedVersion (0: must not exist).
func SetOpIf(collection, key string, value any, expectedVersion uint64) Op {
	return Op{Kind: OpSet, Collection: collection, Key: key, Value: value, Expected: expectedVersion}
}

// DeleteOp deletes collection/key (deleting a missing key is not an error).
func DeleteOp(collection, key string) Op {
	return Op{Kind: OpDelete, Collection: collection, Key: key, Expected: AnyVersion}
}

// DeleteOpIf deletes the key only if it is at expectedVersion.
func DeleteOpIf(collection, key string, expectedVersion uint64) Op {
	return Op{Kind: OpDelete, Collection: collection, Key: key, Expected: expectedVersion}
}

func validCollection(s string) bool {
	if len(s) < 1 || len(s) > 64 {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !(c == '_' || c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || i > 0 && c >= '0' && c <= '9') {
			return false
		}
	}
	return true
}

// keyID is "collection\0key", checked as the server checks it.
func keyID(collection, key string) ([]byte, error) {
	if !validCollection(collection) {
		return nil, inputError("Collection must be a 1-64 character ASCII identifier.")
	}
	if len(key) == 0 || len(key) > 512 || !validUTF8NoNUL(key) {
		return nil, inputError("Key must contain 1-512 UTF-8 bytes and no NUL.")
	}
	id := make([]byte, 0, len(collection)+1+len(key))
	id = append(id, collection...)
	id = append(id, 0)
	return append(id, key...), nil
}

func validUTF8NoNUL(s string) bool {
	return utf8.ValidString(s) && strings.IndexByte(s, 0) < 0
}

func encodeItemValue(v any) ([]byte, error) {
	b, err := encodeValue(v)
	if err != nil {
		return nil, err
	}
	if len(b) > MaxValue {
		return nil, limitError("A value may contain at most 256 KiB of JSON.")
	}
	return b, nil
}

func manyPayload(collection string, keys []string) ([]byte, error) {
	if len(keys) > 4096 {
		return nil, limitError("getMany count")
	}
	b := binary.LittleEndian.AppendUint32(nil, uint32(len(keys)))
	for _, k := range keys {
		id, err := keyID(collection, k)
		if err != nil {
			return nil, err
		}
		b = binary.LittleEndian.AppendUint32(b, uint32(len(id)))
		b = append(b, id...)
	}
	return b, nil
}

func parseMany(reply []byte, want int) ([]*Item, error) {
	bad := func() ([]*Item, error) { return nil, ioError("malformed getMany reply", nil) }
	if len(reply) < 5 {
		return bad()
	}
	count := int(binary.LittleEndian.Uint32(reply[1:]))
	if count != want {
		return nil, ioError("getMany reply count", nil)
	}
	out := make([]*Item, count)
	p := 5
	for i := 0; i < count; i++ {
		if p >= len(reply) {
			return bad()
		}
		found := reply[p]
		p++
		if found == 0 {
			continue
		}
		if p+12 > len(reply) {
			return bad()
		}
		version := binary.LittleEndian.Uint64(reply[p:])
		n := int(binary.LittleEndian.Uint32(reply[p+8:]))
		p += 12
		if n < 0 || p+n > len(reply) {
			return bad()
		}
		it, err := newItem(version, reply[p:p+n])
		if err != nil {
			return nil, err
		}
		out[i] = it
		p += n
	}
	return out, nil
}

func newItem(version uint64, raw []byte) (*Item, error) {
	v, err := parseJSON(raw)
	if err != nil {
		return nil, ioError("a stored value is not JSON: "+err.Error(), err)
	}
	return &Item{Version: version, Value: v, Raw: json.RawMessage(append([]byte(nil), raw...))}, nil
}

func batchPayload(ops []Op) ([]byte, error) {
	if len(ops) == 0 || len(ops) > 65536 {
		return nil, inputError("operation count")
	}
	b := binary.LittleEndian.AppendUint32(make([]byte, 0, 64*len(ops)), uint32(len(ops)))
	seen := make(map[string]struct{}, len(ops))
	for _, op := range ops {
		if op.Kind != OpSet && op.Kind != OpDelete {
			return nil, inputError("operation type")
		}
		id, err := keyID(op.Collection, op.Key)
		if err != nil {
			return nil, err
		}
		if _, dup := seen[string(id)]; dup {
			return nil, inputError("duplicate transaction key")
		}
		seen[string(id)] = struct{}{}
		var v []byte
		if op.Kind == OpSet {
			if v, err = encodeItemValue(op.Value); err != nil {
				return nil, err
			}
		}
		b = append(b, byte(op.Kind))
		b = binary.LittleEndian.AppendUint64(b, op.Expected)
		b = binary.LittleEndian.AppendUint32(b, uint32(len(id)))
		b = binary.LittleEndian.AppendUint32(b, uint32(len(v)))
		b = append(b, id...)
		b = append(b, v...)
		if len(b) > MaxFrame-64 {
			return nil, limitError("transaction bytes")
		}
	}
	return b, nil
}

func (c *Client) get(ctx context.Context, tx *Tx, collection, key string) (*Item, error) {
	id, err := keyID(collection, key)
	if err != nil {
		return nil, err
	}
	reply, err := c.txDo(ctx, tx, 'G', id)
	if err != nil {
		return nil, err
	}
	if reply[0] == 1 {
		return nil, nil
	}
	if len(reply) < 9 {
		return nil, ioError("short reply", nil)
	}
	return newItem(binary.LittleEndian.Uint64(reply[1:]), reply[9:])
}

func (c *Client) getMany(ctx context.Context, tx *Tx, collection string, keys []string) ([]*Item, error) {
	payload, err := manyPayload(collection, keys)
	if err != nil {
		return nil, err
	}
	reply, err := c.txDo(ctx, tx, 'M', payload)
	if err != nil {
		return nil, err
	}
	return parseMany(reply, len(keys))
}

// txDo is do for a call that may be inside a Tx.
func (c *Client) txDo(ctx context.Context, tx *Tx, cmd byte, payload []byte) ([]byte, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if err := c.acquire(ctx, tx != nil); err != nil {
		return nil, err
	}
	defer c.release(tx != nil)
	if err := tx.check(); err != nil {
		return nil, err
	}
	reply, err := c.call(ctx, cmd, payload)
	tx.after(err)
	return reply, err
}

// Set stores a JSON value (see SQL for the types) and returns its new version.
func (c *Client) Set(ctx context.Context, collection, key string, value any) (uint64, error) {
	return c.Batch(ctx, SetOp(collection, key, value))
}

// SetIf stores a value only if the key is at expectedVersion (0: must not exist); else
// the error is ErrConflict.
func (c *Client) SetIf(ctx context.Context, collection, key string, value any, expectedVersion uint64) (uint64, error) {
	return c.Batch(ctx, SetOpIf(collection, key, value, expectedVersion))
}

// Delete deletes a key (deleting a missing key is not an error) and returns the version
// of the transaction.
func (c *Client) Delete(ctx context.Context, collection, key string) (uint64, error) {
	return c.Batch(ctx, DeleteOp(collection, key))
}

// DeleteIf deletes a key only if it is at expectedVersion; else ErrConflict.
func (c *Client) DeleteIf(ctx context.Context, collection, key string, expectedVersion uint64) (uint64, error) {
	return c.Batch(ctx, DeleteOpIf(collection, key, expectedVersion))
}

// Batch applies up to 65,536 operations all-or-nothing, durable when it returns, and
// returns the version they share.
func (c *Client) Batch(ctx context.Context, ops ...Op) (uint64, error) {
	payload, err := batchPayload(ops)
	if err != nil {
		return 0, err
	}
	reply, err := c.do(ctx, false, 'B', payload)
	if err != nil {
		return 0, err
	}
	if len(reply) < 9 {
		return 0, ioError("short reply", nil)
	}
	return binary.LittleEndian.Uint64(reply[1:]), nil
}

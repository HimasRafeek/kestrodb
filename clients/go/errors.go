package kestrodb

import (
	"database/sql"
	"errors"
	"strings"
)

// Error is every error this package returns. Kind is the leading code of the message
// (CONFLICT, INPUT, LIMIT, AUTH, IO, CLOSED, BUSY, READONLY, UNAVAILABLE, CORRUPT, ...;
// ERROR when a message has none). Compare kinds with errors.Is and the Err* values:
//
//	if errors.Is(err, kestrodb.ErrConflict) { ... }
//
// or read Kind after errors.As. An IO error caused by a network, TLS or context error
// wraps it (errors.Is(err, context.DeadlineExceeded) works).
type Error struct {
	Kind    string // the code: "CONFLICT", "INPUT", ...
	Message string // the whole message, starting with the code
	Err     error  // the cause (network, TLS, context), or nil

	sentinel bool
}

func (e *Error) Error() string { return e.Message }

// Unwrap returns the cause, if any.
func (e *Error) Unwrap() error { return e.Err }

// Is reports whether target is the sentinel of e's kind (ErrConflict for a CONFLICT, ...).
func (e *Error) Is(target error) bool {
	t, ok := target.(*Error)
	return ok && t.sentinel && t.Kind == e.Kind
}

func sentinel(kind string) *Error { return &Error{Kind: kind, Message: kind, sentinel: true} }

// Sentinels for errors.Is, one per error code.
var (
	ErrConflict    = sentinel("CONFLICT")    // unique constraint, duplicate id, failed expected version, lost BEGIN CONCURRENT
	ErrInput       = sentinel("INPUT")       // invalid SQL, parameters, names or values
	ErrLimit       = sentinel("LIMIT")       // a size or count limit
	ErrAuth        = sentinel("AUTH")        // wrong token, or not allowed
	ErrIO          = sentinel("IO")          // connection failed or lost; a write without a reply has an unknown outcome
	ErrClosed      = sentinel("CLOSED")      // the client was closed by the program
	ErrBusy        = sentinel("BUSY")        // the server is at its connection limit
	ErrReadOnly    = sentinel("READONLY")    // a follower or replica refused a write
	ErrUnavailable = sentinel("UNAVAILABLE") // a cluster's majority did not confirm a write in time: unknown outcome
	ErrCorrupt     = sentinel("CORRUPT")     // the file failed a checksum
	ErrUnsupported = sentinel("UNSUPPORTED") // not available in this server build
)

// ErrNoRows is returned by Row.Scan when the query returned no rows (it is sql.ErrNoRows).
var ErrNoRows = sql.ErrNoRows

// ErrTxDone is returned by a Tx used after Commit or Rollback (it is sql.ErrTxDone).
var ErrTxDone = sql.ErrTxDone

// kindOf is the leading upper-case word of a message, or ERROR.
func kindOf(message string) string {
	word := message
	if i := strings.IndexByte(message, ' '); i >= 0 {
		word = message[:i]
	}
	if word == "" {
		return "ERROR"
	}
	for i := 0; i < len(word); i++ {
		c := word[i]
		if !(c >= 'A' && c <= 'Z' || c == '_') {
			return "ERROR"
		}
	}
	return word
}

// newError is the error for a message from the server or the client ("CONFLICT ...").
func newError(message string) *Error { return &Error{Kind: kindOf(message), Message: message} }

func inputError(message string) *Error { return &Error{Kind: "INPUT", Message: "INPUT " + message} }

func limitError(message string) *Error { return &Error{Kind: "LIMIT", Message: "LIMIT " + message} }

func ioError(message string, cause error) *Error {
	return &Error{Kind: "IO", Message: "IO " + message, Err: cause}
}

func closedError() *Error {
	return &Error{Kind: "CLOSED", Message: "CLOSED the connection was closed"}
}

// kindIs reports whether err is an *Error of that kind.
func kindIs(err error, kind string) bool {
	var e *Error
	return errors.As(err, &e) && e.Kind == kind
}

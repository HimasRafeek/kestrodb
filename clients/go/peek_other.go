//go:build !unix

package kestrodb

import "net"

// peekRaw: no free check on this platform; an idle connection is checked with a short
// read instead (see conn.probe).
func peekRaw(net.Conn) peekState { return peekUnknown }

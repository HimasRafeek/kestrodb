//go:build unix

package kestrodb

import (
	"net"
	"syscall"
)

// peekRaw looks at a socket without blocking or consuming: whether the server sent
// something (or closed it) while the client was not waiting for a reply.
func peekRaw(c net.Conn) peekState {
	sc, ok := c.(syscall.Conn)
	if !ok {
		return peekUnknown
	}
	rc, err := sc.SyscallConn()
	if err != nil {
		return peekUnknown
	}
	state := peekUnknown
	var buf [1]byte
	err = rc.Read(func(fd uintptr) bool {
		for {
			n, _, e := syscall.Recvfrom(int(fd), buf[:], syscall.MSG_PEEK|syscall.MSG_DONTWAIT)
			switch {
			case e == syscall.EINTR:
				continue
			case e == syscall.EAGAIN || e == syscall.EWOULDBLOCK:
				state = peekEmpty
			case e != nil:
				state = peekClosed // reset
			case n > 0:
				state = peekData
			default:
				state = peekClosed // end of stream
			}
			return true // never wait
		}
	})
	if err != nil {
		return peekClosed
	}
	return state
}

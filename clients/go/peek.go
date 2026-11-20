package kestrodb

type peekState int

const (
	peekUnknown peekState = iota // cannot tell without reading
	peekEmpty                    // open, nothing to read
	peekData                     // bytes are waiting
	peekClosed                   // the peer closed or reset it
)

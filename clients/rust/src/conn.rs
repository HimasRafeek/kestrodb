//! One open connection: TCP, TLS (feature `tls`) or a Unix socket.

use crate::error::Error;
use crate::wire::MAX_FRAME;
use std::io::{self, BufRead, BufReader, Read, Write};
use std::net::{Shutdown, TcpStream, ToSocketAddrs};
use std::time::Duration;

pub(crate) enum Stream {
    Tcp(TcpStream),
    #[cfg(unix)]
    Unix(std::os::unix::net::UnixStream),
    #[cfg(feature = "tls")]
    Tls(Box<rustls::StreamOwned<rustls::ClientConnection, TcpStream>>),
}

impl Stream {
    fn set_nonblocking(&self, on: bool) -> io::Result<()> {
        match self {
            Stream::Tcp(s) => s.set_nonblocking(on),
            #[cfg(unix)]
            Stream::Unix(s) => s.set_nonblocking(on),
            #[cfg(feature = "tls")]
            Stream::Tls(t) => t.sock.set_nonblocking(on),
        }
    }

    fn shutdown(&self) {
        let _ = match self {
            Stream::Tcp(s) => s.shutdown(Shutdown::Both),
            #[cfg(unix)]
            Stream::Unix(s) => s.shutdown(Shutdown::Both),
            #[cfg(feature = "tls")]
            Stream::Tls(t) => t.sock.shutdown(Shutdown::Both),
        };
    }
}

impl Read for Stream {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match self {
            Stream::Tcp(s) => s.read(buf),
            #[cfg(unix)]
            Stream::Unix(s) => s.read(buf),
            #[cfg(feature = "tls")]
            Stream::Tls(t) => t.read(buf),
        }
    }
}

impl Write for Stream {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match self {
            Stream::Tcp(s) => s.write(buf),
            #[cfg(unix)]
            Stream::Unix(s) => s.write(buf),
            #[cfg(feature = "tls")]
            Stream::Tls(t) => t.write(buf),
        }
    }

    fn flush(&mut self) -> io::Result<()> {
        match self {
            Stream::Tcp(s) => s.flush(),
            #[cfg(unix)]
            Stream::Unix(s) => s.flush(),
            #[cfg(feature = "tls")]
            Stream::Tls(t) => t.flush(),
        }
    }
}

/// What an idle connection shows before the next request.
pub(crate) enum Idle {
    /// Nothing to read: the connection is open.
    Open,
    /// The server closed it (or sent bytes that are not an error frame).
    Closed,
    /// The server sent an error on its own (BUSY) and closed the connection.
    Unsolicited(Error),
}

pub(crate) struct Conn {
    io: BufReader<Stream>,
}

impl Conn {
    pub fn new(stream: Stream) -> Conn {
        Conn {
            io: BufReader::with_capacity(64 * 1024, stream),
        }
    }

    /// Sends one frame and reads its reply (status byte first).
    pub fn exchange(&mut self, frame: &[u8]) -> io::Result<Vec<u8>> {
        let s = self.io.get_mut();
        s.write_all(frame)?;
        s.flush()?;
        self.read_frame(MAX_FRAME + 4096)
    }

    fn read_frame(&mut self, max: usize) -> io::Result<Vec<u8>> {
        let mut head = [0u8; 4];
        self.io.read_exact(&mut head)?;
        let n = u32::from_le_bytes(head) as usize;
        if n == 0 || n > max {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("invalid response length {n}"),
            ));
        }
        let mut reply = vec![0u8; n];
        self.io.read_exact(&mut reply)?;
        Ok(reply)
    }

    /// Whether the server closed this connection, or sent an error without a request, while
    /// nothing was asked of it. One non-blocking read.
    pub fn check_idle(&mut self) -> Idle {
        if self.io.buffer().is_empty() {
            if self.io.get_ref().set_nonblocking(true).is_err() {
                return Idle::Closed;
            }
            let got = self.io.fill_buf().map(|b| b.len());
            if self.io.get_ref().set_nonblocking(false).is_err() {
                return Idle::Closed;
            }
            match got {
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => return Idle::Open,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => return Idle::Open,
                Ok(0) | Err(_) => return Idle::Closed,
                Ok(_) => {}
            }
        }
        match self.read_frame(64 * 1024) {
            Ok(f) if f.first() == Some(&255) => Idle::Unsolicited(Error::from_message(
                String::from_utf8_lossy(&f[1..]).into_owned(),
            )),
            _ => Idle::Closed,
        }
    }

    pub fn close(self) {
        self.io.get_ref().shutdown();
    }
}

/// A TCP connection to the first of host's addresses that accepts.
pub(crate) fn connect_tcp(
    host: &str,
    port: u16,
    connect_timeout: Option<Duration>,
) -> io::Result<TcpStream> {
    let mut last = None;
    for addr in (host, port).to_socket_addrs()? {
        let r = match connect_timeout {
            Some(t) => TcpStream::connect_timeout(&addr, t),
            None => TcpStream::connect(addr),
        };
        match r {
            Ok(s) => return Ok(s),
            Err(e) => last = Some(e),
        }
    }
    Err(last
        .unwrap_or_else(|| io::Error::new(io::ErrorKind::NotFound, "the host name has no address")))
}

/// A short text for an I/O error.
pub(crate) fn describe(e: &io::Error) -> String {
    match e.kind() {
        io::ErrorKind::UnexpectedEof => "connection closed".into(),
        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut => format!("timed out ({e})"),
        _ => e.to_string(),
    }
}

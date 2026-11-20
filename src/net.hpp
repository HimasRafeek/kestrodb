#pragma once
// Minimal blocking sockets for the KestroDB server: TCP everywhere, Unix domain sockets on
// Linux and macOS. Winsock on Windows, BSD sockets elsewhere.
#include "platform.hpp"
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // macOS: SIGPIPE is ignored process-wide in init()
#endif
#endif
namespace kestro::net {
// Pending-connection queue. Windows clamps a plain SOMAXCONN to about 200 on client editions,
// which refused connections in a 2,000-client connect burst; Linux clamps to net.core.somaxconn.
#ifdef _WIN32
inline const int BACKLOG = SOMAXCONN_HINT(65535);
#else
inline const int BACKLOG = 65535;
#endif
#ifdef _WIN32
using Socket = SOCKET;
inline const Socket invalid = INVALID_SOCKET;
inline void init() { WSADATA d; require(!WSAStartup(MAKEWORD(2, 2), &d), "IO WSAStartup failed"); }
inline void close(Socket s) { if (s != invalid) closesocket(s); }
inline void shutdown_both(Socket s) { ::shutdown(s, SD_BOTH); }
inline void shutdown_send(Socket s) { ::shutdown(s, SD_SEND); }
inline void set_nonblocking(Socket s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
// Reads and drops what is waiting: 1 something was read, 2 nothing yet, 0 closed or failed.
inline int drain_some(Socket s) { char b[4096]; int r = ::recv(s, b, int(sizeof b), 0); if (r > 0) return 1; if (r == 0) return 0; return WSAGetLastError() == WSAEWOULDBLOCK ? 2 : 0; }
inline std::string error_text() { return "socket error " + std::to_string(WSAGetLastError()); }
#else
using Socket = int;
inline const Socket invalid = -1;
inline void init() { signal(SIGPIPE, SIG_IGN); }
inline void close(Socket s) { if (s >= 0) ::close(s); }
inline void shutdown_both(Socket s) { ::shutdown(s, SHUT_RDWR); }
inline void shutdown_send(Socket s) { ::shutdown(s, SHUT_WR); }
inline void set_nonblocking(Socket s) { int f = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, f | O_NONBLOCK); }
inline int drain_some(Socket s) { char b[4096]; auto r = ::recv(s, b, sizeof b, 0); if (r > 0) return 1; if (r == 0) return 0; return errno == EAGAIN || errno == EWOULDBLOCK ? 2 : 0; }
inline std::string error_text() { return std::string("socket error: ") + std::strerror(errno); }
#endif
[[noreturn]] inline void fail(const std::string& what) { throw std::runtime_error("IO " + what + ": " + error_text()); }

// Listens on host:port ("127.0.0.1:7820", "[::1]:7820", "0.0.0.0:7820"; port 0 picks one).
// Returns the socket and writes the bound port to port_out.
inline Socket listen_tcp(const std::string& address, int& port_out) {
  auto colon = address.rfind(':');
  require(colon != std::string::npos && colon + 1 < address.size(), "INPUT --tcp expects HOST:PORT");
  std::string host = address.substr(0, colon), port = address.substr(colon + 1);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
  addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.empty() ? nullptr : host.c_str(), port.c_str(), &hints, &found) || !found) throw std::runtime_error("INPUT cannot resolve " + address);
  Socket s = ::socket(found->ai_family, found->ai_socktype, found->ai_protocol);
  if (s == invalid) { freeaddrinfo(found); fail("socket"); }
  int one = 1;
#ifdef _WIN32
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof one);
#else
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
  if (::bind(s, found->ai_addr, int(found->ai_addrlen)) || ::listen(s, BACKLOG)) { freeaddrinfo(found); close(s); fail("bind/listen on " + address); }
  freeaddrinfo(found);
  sockaddr_storage bound{}; socklen_t len = sizeof bound;
  getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len);
  port_out = bound.ss_family == AF_INET6 ? ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port) : ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
  return s;
}

// connect() giving up after timeout_ms (0: the system's own timeout, which can be long for a
// host that is down; on Windows even a refused local port takes about two seconds).
inline bool connect_within(Socket s, const sockaddr* a, int len, int timeout_ms) {
  if (timeout_ms <= 0) return ::connect(s, a, len) == 0;
#ifdef _WIN32
  u_long on = 1, off = 0; ioctlsocket(s, FIONBIO, &on);
  bool ok = ::connect(s, a, len) == 0;
  if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
    fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    ok = select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(s, &w) && !FD_ISSET(s, &e);
  }
  ioctlsocket(s, FIONBIO, &off);
#else
  int flags = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, flags | O_NONBLOCK);
  bool ok = ::connect(s, a, socklen_t(len)) == 0;
  if (!ok && errno == EINPROGRESS) {
    pollfd p{s, POLLOUT, 0}; int r;
    do { r = ::poll(&p, 1, timeout_ms); } while (r < 0 && errno == EINTR);
    int err = 0; socklen_t l = sizeof err;
    ok = r > 0 && !getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l) && err == 0;
  }
  fcntl(s, F_SETFL, flags);
#endif
  return ok;
}
// Connects to HOST:PORT (a follower to its leader). Throws IO on failure.
inline Socket connect_tcp(const std::string& address, int timeout_ms = 0) {
  auto colon = address.rfind(':');
  require(colon != std::string::npos && colon + 1 < address.size(), "INPUT address must be HOST:PORT");
  std::string host = address.substr(0, colon), port = address.substr(colon + 1);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
  addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_NUMERICSERV;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) || !found) throw std::runtime_error("IO cannot resolve " + address);
  Socket s = invalid;
  for (auto* a = found; a; a = a->ai_next) { s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol); if (s == invalid) continue; if (connect_within(s, a->ai_addr, int(a->ai_addrlen), timeout_ms)) break; close(s); s = invalid; }
  freeaddrinfo(found);
  if (s == invalid) fail("connect to " + address);
  int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
  return s;
}

#ifndef _WIN32
// Listens on a Unix domain socket. An existing socket file at path is replaced; any other
// kind of file is refused. mode is the file permission (e.g. 0600, 0660).
inline Socket listen_unix(const std::string& path, unsigned mode) {
  sockaddr_un a{}; a.sun_family = AF_UNIX;
  require(!path.empty() && path.size() < sizeof a.sun_path, "INPUT --unix path too long");
  struct stat st{};
  if (lstat(path.c_str(), &st) == 0) { require(S_ISSOCK(st.st_mode), "INPUT --unix path exists and is not a socket"); ::unlink(path.c_str()); }
  memcpy(a.sun_path, path.c_str(), path.size());
  Socket s = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (s == invalid) fail("socket");
  if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) || ::chmod(path.c_str(), mode) || ::listen(s, BACKLOG)) { close(s); fail("bind/listen on " + path); }
  return s;
}
// Connects to a Unix domain socket (kestro-server admin unix://PATH).
inline Socket connect_unix(const std::string& path) {
  sockaddr_un a{}; a.sun_family = AF_UNIX;
  require(!path.empty() && path.size() < sizeof a.sun_path, "INPUT unix socket path too long");
  memcpy(a.sun_path, path.c_str(), path.size());
  Socket s = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (s == invalid) fail("socket");
  if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a)) { close(s); fail("connect to " + path); }
  return s;
}
#endif

inline Socket accept_one(Socket listener) {
  Socket c = ::accept(listener, nullptr, nullptr);
  if (c == invalid) return invalid;
  int one = 1;  // fails harmlessly on Unix sockets
  setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
  return c;
}
// The peer's IP address ("" for a Unix socket or when unknown).
inline std::string peer_address(Socket s) {
  sockaddr_storage a{}; socklen_t n = sizeof a;
  if (getpeername(s, reinterpret_cast<sockaddr*>(&a), &n) != 0) return "";
  char buf[64] = {};
  if (a.ss_family == AF_INET) inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&a)->sin_addr, buf, sizeof buf);
  else if (a.ss_family == AF_INET6) inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&a)->sin6_addr, buf, sizeof buf);
  else return "";
  return buf;
}
inline bool loopback_address(const std::string& ip) { return ip.rfind("127.", 0) == 0 || ip == "::1" || ip.rfind("::ffff:127.", 0) == 0; }
// Receive/send timeout in seconds (0: none).
inline void set_timeout(Socket s, int seconds) {
#ifdef _WIN32
  DWORD ms = DWORD(seconds) * 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
#else
  timeval tv{seconds, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}
// Bytes read (0 when the peer closed), or -1 on error.
inline long long recv_some(Socket s, void* p, size_t n) {
  for (;;) {
    auto got = ::recv(s, static_cast<char*>(p), int(std::min<size_t>(n, 1u << 30)), 0);
#ifndef _WIN32
    if (got < 0 && errno == EINTR) continue;
#endif
    return got;
  }
}
inline bool send_all(Socket s, const unsigned char* p, size_t n) {
  while (n) {
#ifdef _WIN32
    auto put = ::send(s, reinterpret_cast<const char*>(p), int(std::min<size_t>(n, 1u << 30)), 0);
#else
    auto put = ::send(s, p, n, MSG_NOSIGNAL);
    if (put < 0 && errno == EINTR) continue;
#endif
    if (put <= 0) return false;
    p += put; n -= size_t(put);
  }
  return true;
}
}

// kestro-server: one KestroDB file shared by many clients (any language) over TCP or a
// Unix socket. Reads run in parallel on each connection's thread; writes from every
// connection are group-committed by one committer thread, so concurrent writers share
// disk flushes. Requests on one connection are answered in order, and a connection
// always sees its own earlier writes.
//
// Wire protocol (the same frames as the worker): every request and reply is
// u32 little-endian length + body. A request body is a command byte + payload; a reply
// body is a status byte (0 ok, 1 not found, 255 error + message) + payload.
//   A token      authenticate (required first when the server has users); the token
//                selects the user, whose role and tables limit every later request:
//                  readonly  G M S, SELECT, BEGIN/COMMIT/ROLLBACK
//                  readwrite + B, INSERT/UPDATE/DELETE
//                  admin     + CREATE/DROP/ALTER, V P C, reserved keys
//                A user limited to tables may only use those tables/collections.
//                With --tls-client-ca a TLS client must show a certificate from those CAs
//                (--tls-client-cert required, the default) or may (optional); a user bound to
//                a certificate (cert= in the users file) logs in by it alone (token -) or
//                needs both it and the token.
//   G key        get              -> u64 version, value
//   M ...        getMany          -> u32 count, per key: found, [u64 version, u32 length, value]
//   B ...        atomic batch     -> u64 version
//   E ...        SQL: u32 statement length, statement, parameters JSON array -> result JSON
//   S            stats JSON       V verify       P path backup      C path compact
//   Q            close this connection
// Administration (admin role; the server's operators, kestro-server admin):
//   L            the live connections (JSON)    D u64 id  close that connection
//   O [u32 n]    the newest slow statements      T         the metrics (Prometheus text)
// After LISTEN (SQL, over tcp:// or unix://), the server also sends notification frames at
// any time, between replies: status byte 2, u64 txid, u32 channel length, channel, payload.
// A client that never LISTENs never gets one. They wait in a queue of --notify-queue MB per
// connection (16); a client that falls further behind is disconnected (nothing is dropped
// silently), and commits never wait for it.
// Keys are "collection\0key"; values are the client's bytes (JSON in the bundled clients).
#include "protocol.hpp"
#include "net.hpp"
#ifdef KESTRO_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#elif defined(KESTRO_WINTLS)
#include "tls_windows.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <functional>
#include <future>
#include <iostream>
#include <set>
#include <filesystem>
#include <fstream>
#include <random>
#include <ctime>
#include <cstdio>
#include <thread>
#include <cctype>
using namespace kestro;
using namespace kestro::protocol;

namespace {
std::atomic<bool> stopping{false};
std::mutex listeners_m;
std::vector<net::Socket> listeners;
std::vector<bool> listener_tls;  // parallel to listeners
void stop_server() {
  if (stopping.exchange(true)) return;
  std::lock_guard<std::mutex> g(listeners_m);
  for (auto s : listeners) { net::shutdown_both(s); net::close(s); }
}
#ifdef _WIN32
BOOL WINAPI on_console(DWORD) { stop_server(); return TRUE; }
#else
void on_signal(int) { stop_server(); }
#endif

// Connections refused at a limit get BUSY, then the socket is half-closed and what the client
// already sent (its login, usually) is read and dropped for a moment before closing: closing
// with unread bytes makes the OS reset the connection, which can destroy the BUSY reply before
// the client reads it. One thread does this for every refused connection, so accepting never
// waits; it holds at most 1,024 of them (more are closed at once).
struct Rejecter {
  std::mutex m; std::condition_variable cv; std::deque<std::pair<net::Socket, std::chrono::steady_clock::time_point>> q; bool started = false;
  void refuse(net::Socket s, const std::string& message) {
    std::string frame(4, '\0'); frame += char(255); frame += message;
    unsigned char h[4]; put32(h, uint32_t(frame.size() - 4)); memcpy(&frame[0], h, 4);
    net::send_all(s, reinterpret_cast<const unsigned char*>(frame.data()), frame.size());
    net::shutdown_send(s); net::set_nonblocking(s);
    std::lock_guard<std::mutex> g(m);
    if (q.size() >= 1024) { net::close(s); return; }
    if (!started) { started = true; std::thread([this] { run(); }).detach(); }
    q.emplace_back(s, std::chrono::steady_clock::now() + std::chrono::milliseconds(250)); cv.notify_one();
  }
  void run() {
    std::unique_lock<std::mutex> g(m);
    for (;;) {
      cv.wait(g, [&] { return !q.empty(); });
      auto now = std::chrono::steady_clock::now();
      for (size_t i = q.size(); i-- > 0;) {
        auto [s, until] = q.front(); q.pop_front();
        int r = 1; while (r == 1) r = net::drain_some(s);
        if (r == 0 || now >= until) net::close(s); else q.emplace_back(s, until);
      }
      if (!q.empty()) { g.unlock(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); g.lock(); }
    }
  }
};
// Never destroyed: its thread waits on cv until the process ends, and destroying a condition
// variable with a waiter blocks forever on Linux (a stop after any BUSY hung).
Rejecter& g_rejecter = *new Rejecter;

#include "metrics.inc"

// Group commit across connections: each connection hands over its pipelined batches and
// waits; the committer takes everything queued and commits it with one flush.
class Committer {
public:
  struct Job { std::vector<std::vector<Op>> txs; size_t bytes = 0; std::vector<Store::Outcome> results; std::exception_ptr error; std::promise<void> done; };
  explicit Committer(Store& db) : db_(db), thread_([this] { run(); }) {}
  ~Committer() { { std::lock_guard<std::mutex> g(m_); stop_ = true; } cv_.notify_one(); thread_.join(); }
  void commit(Job& job) {
    auto f = job.done.get_future();
    { std::lock_guard<std::mutex> g(m_); queue_.push_back(&job); }
    cv_.notify_one();
    f.wait();
  }
private:
  void run() {
    std::unique_lock<std::mutex> l(m_);
    for (;;) {
      cv_.wait(l, [&] { return stop_ || !queue_.empty(); });
      if (queue_.empty()) return;
      std::vector<Job*> work; work.swap(queue_); l.unlock();
      for (size_t first = 0; first < work.size();) {  // one flush per chunk of at most 64 MiB
        size_t last = first, bytes = 0;
        while (last < work.size() && (last == first || bytes + work[last]->bytes <= MAX_GROUP)) bytes += work[last++]->bytes;
        std::vector<const std::vector<Op>*> group;
        for (size_t i = first; i < last; i++) for (auto& tx : work[i]->txs) group.push_back(&tx);
        try {
          auto results = db_.commit_group(group); size_t g = 0;
          for (size_t i = first; i < last; i++) { work[i]->results.assign(results.begin() + g, results.begin() + g + work[i]->txs.size()); g += work[i]->txs.size(); }
        } catch (...) { for (size_t i = first; i < last; i++) work[i]->error = std::current_exception(); }
        for (size_t i = first; i < last; i++) work[i]->done.set_value();
        first = last;
      }
      l.lock();
    }
  }
  Store& db_;
  std::mutex m_;
  std::condition_variable cv_;
  std::vector<Job*> queue_;
  bool stop_ = false;
  std::thread thread_;
};

// A user of the server: a token (kept as its SHA-256), a role, and optionally the only
// tables/collections it may use and the client certificate it must show: cert is
// "cn:NAME" (the subject's common name) or "sha256:HEX" (the certificate's fingerprint).
// A user without a token (has_token false) logs in by its certificate alone.
struct User { std::string name, role; Hash hash{}; bool has_token = true; std::vector<std::string> tables; std::string cert; };
// A TLS client's certificate (--tls-client-ca), checked during the connection's setup.
struct PeerCert { bool present = false; std::string error, cn; Hash sha256{}; };
bool cert_matches(const User& u, const PeerCert& c);
struct Config {
  std::vector<User> users;  // empty: no authentication (every client is an administrator)
  const Options* options = nullptr;
  void* tls = nullptr;      // SSL_CTX* (wintls::Credentials* on Windows) when TLS is on
  int client_cert = 0;      // --tls-client-cert with --tls-client-ca: 1 optional, 2 required
  std::string backup_dir;   // where backups clients ask for are written (by plain file name)
  int auth_timeout = 10, transaction_timeout = 60, idle_timeout = 0;  // seconds (0: none)
  int statement_timeout_ms = 30000;
  size_t notify_queue = size_t(16) << 20;  // bytes of notifications a connection may fall behind
  int sync_replicas = 0, sync_timeout_ms = 5000; bool sync_strict = false;  // synchronous replication
  bool cluster = false;  // --cluster: writes wait for a majority, never degrade
};
std::atomic<bool> g_cluster_leader{false};  // this node leads its cluster
std::atomic<uint64_t> g_listeners_dropped{0};  // LISTENing connections closed for falling behind (stats)
// Synchronous replication: a follower's replication request asks for the log after its own
// end, which it has applied durably (it applies each reply before asking again), so that
// end is how far it has the leader's commits. With --sync-replicas N, a write is answered
// only once N followers have it. Past --sync-timeout the leader goes on alone (degraded,
// reported in stats) until a follower catches up, unless --sync-strict: then writes wait.
struct SyncReplication {
  std::mutex m; std::condition_variable cv;
  std::map<const void*, uint64_t> acked;  // follower connection -> its durable end
  bool degraded = false; uint64_t waits = 0, timeouts = 0;
  size_t replicas = 0;  // --sync-replicas
  // leader_end: the leader's log end now. A follower that has caught up ends a degraded spell
  // at once, not on the next write.
  void ack(const void* who, uint64_t end, uint64_t leader_end) {
    {
      std::lock_guard<std::mutex> l(m); auto& a = acked[who]; if (end > a) a = end;
      if (degraded && replicas && nth(replicas) >= leader_end) { degraded = false; std::cerr << "KESTRODB SYNC restored: followers caught up" << std::endl; }
    }
    cv.notify_all();
  }
  void gone(const void* who) { { std::lock_guard<std::mutex> l(m); acked.erase(who); } cv.notify_all(); }
  uint64_t nth(size_t n) const {  // the n-th furthest follower (lock held); 0 when fewer
    if (n == 0) return UINT64_MAX;
    std::vector<uint64_t> v; for (const auto& [w, e] : acked) v.push_back(e);
    if (v.size() < n) return 0;
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(n - 1), v.end(), std::greater<uint64_t>()); return v[n - 1];
  }
  void wait(uint64_t end, const struct Config& cfg);
  std::string json(int want) {
    std::lock_guard<std::mutex> l(m);
    uint64_t at = nth(size_t(want));
    return "{\"replicas\":" + std::to_string(want) + ",\"followers\":" + std::to_string(acked.size()) + ",\"acknowledgedEnd\":" + std::to_string(at == UINT64_MAX ? 0 : at) +
      ",\"degraded\":" + (degraded ? "true" : "false") + ",\"waits\":" + std::to_string(waits) + ",\"timeouts\":" + std::to_string(timeouts) + "}";
  }
} g_sync;
std::atomic<bool> g_stopping_sync{false};
void SyncReplication::wait(uint64_t end, const Config& cfg) {
  if (cfg.sync_replicas <= 0) return;
  std::unique_lock<std::mutex> l(m);
  auto reached = [&] { return nth(size_t(cfg.sync_replicas)) >= end; };
  if (cfg.cluster) {
    // A cluster never goes on without a majority: a write that is not confirmed fails, and
    // its outcome is unknown (a later leader may or may not have it).
    if (reached()) return;
    waits++;
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.sync_timeout_ms);
    while (!reached()) {
      if (!g_cluster_leader) throw std::runtime_error("UNAVAILABLE this server stopped leading the cluster before a majority confirmed the write; it may or may not have been kept (check before retrying)");
      if (g_stopping_sync) throw std::runtime_error("UNAVAILABLE the server is stopping; the write may or may not have been kept");
      if (std::chrono::steady_clock::now() >= until) { timeouts++; throw std::runtime_error("UNAVAILABLE no majority of the cluster confirmed the write within --sync-timeout; it may or may not have been kept (check before retrying)"); }
      cv.wait_for(l, std::chrono::milliseconds(50));
    }
    return;
  }
  if (reached()) { if (degraded) { degraded = false; std::cerr << "KESTRODB SYNC restored: followers caught up" << std::endl; } return; }
  if (degraded && !cfg.sync_strict) return;  // alone until a follower catches up (checked on every write)
  waits++;
  if (cfg.sync_strict) { while (!reached() && !g_stopping_sync) cv.wait_for(l, std::chrono::milliseconds(500)); return; }
  if (!cv.wait_for(l, std::chrono::milliseconds(cfg.sync_timeout_ms), reached)) {
    timeouts++; degraded = true;
    std::cerr << "KESTRODB SYNC degraded: no " << cfg.sync_replicas << " follower(s) confirmed within " << cfg.sync_timeout_ms << " ms; continuing without them" << std::endl;
  }
}
// Failed logins per client address, so that guessing tokens over many connections is slow.
struct AuthFailures {
  std::mutex m; std::map<std::string, std::pair<int, std::chrono::steady_clock::time_point>> by_ip;
  bool blocked(const std::string& ip) {
    std::lock_guard<std::mutex> l(m); auto it = by_ip.find(ip); if (it == by_ip.end()) return false;
    if (std::chrono::steady_clock::now() - it->second.second > std::chrono::minutes(1)) { by_ip.erase(it); return false; }
    return it->second.first >= 10;
  }
  void failed(const std::string& ip) {
    std::lock_guard<std::mutex> l(m); auto& e = by_ip[ip]; auto now = std::chrono::steady_clock::now();
    if (now - e.second > std::chrono::minutes(1)) e = {0, now};
    e.first++;
    if (by_ip.size() > 100000) by_ip.clear();  // bounded memory
  }
} g_auth_failures;
enum Need { NeedRead, NeedWrite, NeedAdmin, NeedReplicate };
[[noreturn]] void denied(const std::string& why) { throw std::runtime_error("AUTH " + why); }
// A follower serves reads only, until it is promoted.
std::atomic<bool> g_readonly{false};
std::string g_leader;  // --replica-of (set before any thread starts)
bool g_cluster_mode = false;
std::mutex g_hint_m; std::string g_leader_hint;  // a cluster's current leader, for READONLY errors
void set_leader_hint(const std::string& l) { std::lock_guard<std::mutex> g(g_hint_m); g_leader_hint = l; }
std::function<std::string()> g_replica_json, g_cluster_json;  // stats sections (set while replicating / in a cluster)
void refuse_writes() {
  if (!g_readonly) return;
  if (g_cluster_mode) {
    std::string l; { std::lock_guard<std::mutex> g(g_hint_m); l = g_leader_hint; }
    // Nothing was applied: a client may send the write to the leader named here.
    throw std::runtime_error(l.empty() ? "READONLY no cluster leader is known yet (an election is under way); retry shortly" : "READONLY this server follows the cluster's leader; the leader is " + l);
  }
  throw std::runtime_error("READONLY this server is a replica of " + g_leader + "; write to the leader (or promote this server)");
}
// user null: an open server.
void allow(const User* u, Need need, std::string_view table = {}) {
  if (need == NeedWrite) refuse_writes();
  if (!u) return;
  if (u->role == "replica" && need != NeedReplicate) denied("user " + u->name + " (replica) may only replicate");
  if (need == NeedReplicate && u->role != "admin" && u->role != "replica") denied("user " + u->name + " (" + u->role + ") may not replicate; it needs the replica or admin role");
  if (need == NeedAdmin && u->role != "admin") denied("user " + u->name + " (" + u->role + ") may not do this; it needs the admin role");
  if (need == NeedWrite && u->role == "readonly") denied("user " + u->name + " is read-only");
  if (!table.empty() && !u->tables.empty() && std::find(u->tables.begin(), u->tables.end(), table) == u->tables.end())
    denied("user " + u->name + " has no access to " + std::string(table));
}
void allow_key(const User* u, Need need, std::string_view key) {
  if (need == NeedWrite) refuse_writes();  // a follower, with or without users
  if (!u) return;
  if (!key.empty() && uint8_t(key[0]) == 1) { allow(u, NeedAdmin); return; }  // the engine's own records
  auto z = key.find('\0');
  auto collection = key.substr(0, z == std::string_view::npos ? key.size() : z);
  allow(u, need);
  // A table-limited user names only its own collections (the empty one included).
  if (!u->tables.empty() && std::find(u->tables.begin(), u->tables.end(), collection) == u->tables.end())
    denied("user " + u->name + " has no access to " + (collection.empty() ? std::string("the unnamed collection") : std::string(collection)));
}
std::string hex_of(const Hash& h) { static const char* d = "0123456789abcdef"; std::string s; for (auto c : h) { s += d[c >> 4]; s += d[c & 15]; } return s; }
bool cert_matches(const User& u, const PeerCert& c) {
  return c.present && c.error.empty() && !u.cert.empty() && (u.cert.compare(0, 3, "cn:") == 0 ? u.cert.compare(3, std::string::npos, c.cn) == 0 : u.cert == "sha256:" + hex_of(c.sha256));
}
// Token and users files are only read, and are often read-only to the server: Docker and
// Compose secrets, root-owned files in /etc (read-only under systemd's ProtectSystem=strict).
// They are opened for reading alone; as before, only a regular file (not a symlink) is taken.
Bytes read_config_file(const std::string& file) {
#ifdef _WIN32
  FD f(platform::open_file(file)); return read_at(f.n, size_t(platform::size(f.n)), 0);
#else
  require(!file.empty() && file.find('\0') == std::string::npos, "INPUT invalid path");
  FD f(::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (!platform::valid(f.n)) platform::error("open");
  struct stat st{};
  if (fstat(f.n, &st)) platform::error("fstat");
  require(S_ISREG(st.st_mode), "INPUT requires regular file");
  return read_at(f.n, size_t(st.st_size), 0);
#endif
}
// Users file: one user per line, "name role token-or-sha256:HEX-or-- [tables=a,b] [cert=cn:NAME|cert=sha256:HEX]";
// # comments. A token of - (no token) needs cert=: that user logs in by its certificate alone.
std::vector<User> load_users(const std::string& file) {
  auto bytes = read_config_file(file);
  std::string text(bytes.begin(), bytes.end()), line; std::vector<User> users; size_t lineno = 0;
  for (size_t p = 0; p <= text.size(); p++) {
    if (p < text.size() && text[p] != '\n') { line += text[p]; continue; }
    lineno++;
    std::vector<std::string> w; std::string cur;
    for (char c : line) { if (c == ' ' || c == '\t' || c == '\r') { if (!cur.empty()) w.push_back(cur), cur.clear(); } else cur += c; }
    if (!cur.empty()) w.push_back(cur);
    line.clear();
    if (w.empty() || w[0][0] == '#') continue;
    auto bad = [&](const std::string& why) { throw std::runtime_error("INPUT users file line " + std::to_string(lineno) + ": " + why); };
    if (w.size() < 3 || w.size() > 5) bad("expected: name role token [tables=a,b] [cert=cn:NAME]");
    User u; u.name = w[0]; u.role = w[1];
    if (u.role != "admin" && u.role != "readwrite" && u.role != "readonly" && u.role != "replica") bad("role must be admin, readwrite, readonly or replica");
    if (w[2] == "-") u.has_token = false;
    else if (w[2].rfind("sha256:", 0) == 0) {
      auto h = w[2].substr(7); if (h.size() != 64) bad("sha256: needs 64 hex digits (kestro-server --hash-token)");
      for (size_t i = 0; i < 32; i++) { auto v = std::stoul(h.substr(i * 2, 2), nullptr, 16); u.hash[i] = (unsigned char)v; }
    } else {
      if (w[2].size() < 16) bad("tokens must be at least 16 characters");
      if (w[2].size() < 32) std::cerr << "warning: users file line " << lineno << ": a token of " << w[2].size() << " characters; use at least 32 random characters (openssl rand -hex 24)" << std::endl;
      u.hash = digest(w[2].data(), w[2].size());
    }
    for (size_t x = 3; x < w.size(); x++) {
      if (w[x].rfind("cert=", 0) == 0) {
        if (!u.cert.empty()) bad("cert= appears twice");
        std::string c = w[x].substr(5), kind = c.substr(0, c.find(':')); for (auto& ch : kind) ch = char(std::tolower((unsigned char)ch));
        if (kind == "cn" && c.size() > 3) u.cert = "cn:" + c.substr(3);
        else if (kind == "sha256") {
          std::string h; for (char ch : c.substr(7)) if (ch != ':') h += char(std::tolower((unsigned char)ch));
          if (h.size() != 64 || h.find_first_not_of("0123456789abcdef") != std::string::npos) bad("cert=sha256: needs the certificate's SHA-256 fingerprint, 64 hex digits (openssl x509 -noout -fingerprint -sha256)");
          u.cert = "sha256:" + h;
        } else bad("cert= must be cert=cn:NAME (the subject's common name) or cert=sha256:FINGERPRINT");
        continue;
      }
      if (w[x].rfind("tables=", 0) != 0 || !u.tables.empty()) bad("after the token: tables=a,b and cert=cn:NAME, each at most once");
      std::string list = w[x].substr(7), t;
      for (size_t i = 0; i <= list.size(); i++) { if (i == list.size() || list[i] == ',') { if (!t.empty()) u.tables.push_back(t); t.clear(); } else t += list[i]; }
      if (u.tables.empty()) bad("tables= lists no table");
      if (u.role == "admin" || u.role == "replica") bad("tables= cannot limit an " + u.role + " (backups, compaction and replication cover every table)");
    }
    if (!u.has_token && u.cert.empty()) bad("a user without a token (-) needs cert=cn:NAME or cert=sha256:FINGERPRINT: it logs in by its client certificate");
    for (const auto& o : users) {
      if (o.name == u.name) bad("user " + u.name + " appears twice");
      if (o.has_token && u.has_token && o.hash == u.hash) bad("two users share a token");
      if (!o.cert.empty() && o.cert == u.cert) bad("two users share a certificate (cert=)");
    }
    users.push_back(std::move(u));
  }
  require(!users.empty(), "INPUT users file lists no user");
  return users;
}

// A client connection: a plain socket, or TLS over it.
struct Conn {
  net::Socket s;
  std::mutex send_m; bool shared_send = false;  // once LISTENing: replies and notification frames take turns
  void* ssl = nullptr;
  std::string peer;  // the client's IP address ("" for a Unix socket)
  PeerCert cert;     // its TLS client certificate (with --tls-client-ca)
  int timeout = -1;  // the receive/send timeout now set on the socket
  ConnInfo info;     // its counts and what the admin requests show
  void set_timeout(int seconds) { if (seconds != timeout) { net::set_timeout(s, seconds); timeout = seconds; } }
  long long recv(void* p, size_t n) {
#ifdef KESTRO_TLS
    if (ssl) { int r = SSL_read(static_cast<SSL*>(ssl), p, int(std::min<size_t>(n, 1 << 30))); return r > 0 ? r : -1; }
#elif defined(KESTRO_WINTLS)
    if (ssl) return static_cast<wintls::Stream*>(ssl)->read(p, n);
#endif
    return net::recv_some(s, p, n);
  }
  bool send(const unsigned char* p, size_t n) {
#ifdef KESTRO_TLS
    if (ssl) { while (n) { int w = SSL_write(static_cast<SSL*>(ssl), p, int(std::min<size_t>(n, 1 << 30))); if (w <= 0) return false; p += w; n -= size_t(w); } return true; }
#elif defined(KESTRO_WINTLS)
    if (ssl) return static_cast<wintls::Stream*>(ssl)->write(p, n);
#endif
    return net::send_all(s, p, n);
  }
};

// LISTEN on a connection: commits queue notifications here (deliver never blocks: a client more
// than --notify-queue behind is disconnected instead), and the connection's sender thread
// writes them as status-2 frames. Not over TLS (one TLS stream cannot read and write at once).
struct ConnListener : Store::Listener {
  Conn& conn; size_t limit;
  std::mutex m; std::condition_variable cv; std::vector<Store::Note> q; size_t bytes = 0; bool overflow = false, stop = false; std::thread thread;
  ConnListener(Conn& c, size_t l) : conn(c), limit(l) {}
  void deliver(const Store::Note* const* notes, size_t n) override {
    std::lock_guard<std::mutex> g(m); if (overflow || stop) return;
    for (size_t i = 0; i < n; i++) {
      bytes += 17 + notes[i]->channel.size() + notes[i]->payload.size();
      // Too far behind: the connection ends now (its sender may be stuck in a send meanwhile).
      if (bytes > limit) { overflow = true; q.clear(); g_listeners_dropped++; net::shutdown_both(conn.s); break; }
      q.push_back(*notes[i]);
    }
    cv.notify_one();
  }
  // Connection thread, after a LISTEN: replies take the send lock from now on.
  void start() { if (thread.joinable()) return; conn.shared_send = true; thread = std::thread([this] { run(); }); }
  void run() {
    try { for (Bytes out;;) {
      std::vector<Store::Note> batch;
      { std::unique_lock<std::mutex> l(m); cv.wait(l, [&] { return stop || overflow || !q.empty(); }); if (stop) return; if (overflow) break; batch.swap(q); bytes = 0; }
      out.clear();
      for (const auto& x : batch) {
        size_t at = out.size(), len = 13 + x.channel.size() + x.payload.size(); out.resize(at + 4 + len);
        unsigned char* p = out.data() + at; put32(p, uint32_t(len)); p[4] = 2; put64(p + 5, x.txid); put32(p + 13, uint32_t(x.channel.size()));
        memcpy(p + 17, x.channel.data(), x.channel.size()); memcpy(p + 17 + x.channel.size(), x.payload.data(), x.payload.size());
      }
      std::lock_guard<std::mutex> g(conn.send_m); if (!conn.send(out.data(), out.size())) break;
      bump(conn.info.st.notes, batch.size()); bump(conn.info.st.note_bytes, out.size());
    } } catch (...) {}
    net::shutdown_both(conn.s);  // gone, or behind by more than the queue: the connection ends
  }
  void finish() { { std::lock_guard<std::mutex> g(m); stop = true; } cv.notify_one(); if (thread.joinable()) { net::shutdown_both(conn.s); thread.join(); } }
};
Bytes error_reply(const std::string& msg) { Bytes out(1, 255); out.insert(out.end(), msg.begin(), msg.end()); return out; }
Bytes error_reply(std::exception_ptr p) { try { std::rethrow_exception(p); } catch (const std::exception& e) { return error_reply(e.what()); } catch (...) { return error_reply("UNKNOWN error"); } }
bool same_token(const std::string& a, const std::string& b) {  // constant time in the token's length
  unsigned diff = unsigned(a.size() ^ b.size());
  for (size_t i = 0; i < b.size(); i++) diff |= unsigned((i < a.size() ? a[i] : 0) ^ b[i]);
  return diff == 0;
}

std::function<Bytes(Store&, Store::SqlSession&, const Config&, const unsigned char*, size_t)> g_cluster_handle;  // 'Z' requests (in a cluster)
std::string render_metrics();
const char* role_now() { return g_readonly ? "follower" : "leader"; }
Bytes handle(Store& db, Store::SqlSession& session, const Config& cfg, std::atomic<int>& clients, const unsigned char* b, size_t n, const User* user, bool tls, ConnInfo& ci) {
  Bytes out(1, 0); char cmd = char(b[0]); std::string arg;
  if (cmd == 'G' || cmd == 'P' || cmd == 'C') arg.assign(reinterpret_cast<const char*>(b + 1), n - 1);
  if (cmd == 'G') allow_key(user, NeedRead, arg);
  if (cmd == 'V' || cmd == 'P' || cmd == 'C' || cmd == 'K' || cmd == 'X' || cmd == 'L' || cmd == 'D' || cmd == 'O' || cmd == 'T') allow(user, NeedAdmin);
  if (cmd == 'K') refuse_writes();  // a follower's file must stay identical to its leader's
  if (cmd == 'R' || cmd == 'Z') allow(user, NeedReplicate);
  if (cmd == 'Z') { require(g_cluster_handle != nullptr, "INPUT this server is not in a cluster (--cluster)"); return g_cluster_handle(db, session, cfg, b, n); }
  if (cmd == 'G') {
    thread_local std::string value; uint64_t version = 0;
    if (!db.read(arg, value, version)) out[0] = 1;
    else { out.resize(9 + value.size()); put64(out.data() + 1, version); memcpy(out.data() + 9, value.data(), value.size()); }
  } else if (cmd == 'M') { auto keys = parse_keys(b, n); for (const auto& k : keys) allow_key(user, NeedRead, k); return encode_values(db.getMany(keys)); }
  else if (cmd == 'E') {
    require(n >= 5, "INPUT SQL frame"); auto len = get32(b + 1); require(uint64_t(len) + 5 <= n, "INPUT SQL frame");
    auto access = db.sql_access(std::string_view(reinterpret_cast<const char*>(b + 5), len));
    // BEGIN (or SAVEPOINT outside a transaction) takes the write gate: every other writer,
    // and a follower's replication, waits for it. Read-only users and followers may not.
    bool begins = access.kind == Store::SqlKind::Transaction && !session.active;
    if (g_readonly && (access.kind == Store::SqlKind::Write || access.kind == Store::SqlKind::Schema || begins)) refuse_writes();
    // A leader that stepped down mid-transaction: only ROLLBACK (its log must stay a prefix of the new leader's).
    if (g_readonly && session.active && access.kind == Store::SqlKind::Transaction) {
      std::string_view t(reinterpret_cast<const char*>(b + 5), len); size_t i = 0; while (i < t.size() && std::isspace(uint8_t(t[i]))) i++;
      std::string w; while (i < t.size() && std::isalpha(uint8_t(t[i]))) w += char(std::toupper(uint8_t(t[i++])));
      if (w != "ROLLBACK") refuse_writes();
    }
    if (user) {
      Need need = access.kind == Store::SqlKind::Schema ? NeedAdmin : access.kind == Store::SqlKind::Write ? NeedWrite : NeedRead;
      allow(user, need);
      if (begins && user->role == "readonly") denied("user " + user->name + " is read-only: transactions hold the write gate");
      if (access.unscoped && !user->tables.empty()) denied("user " + user->name + " is limited to tables: name the table (DROP INDEX name ON table)");
      for (const auto& t : access.tables) allow(user, need, t);
    }
    std::string result;
    {
      Store::StatementDeadline limit(std::chrono::milliseconds(cfg.statement_timeout_ms));
      result = db.sql(&session, std::string_view(reinterpret_cast<const char*>(b + 5), len), std::string_view(reinterpret_cast<const char*>(b + 5 + len), n - 5 - len));
    }
    // A write that committed (not one inside an open transaction): wait for the followers.
    if (cfg.sync_replicas > 0 && !session.active && access.kind != Store::SqlKind::Read) g_sync.wait(db.bytes(), cfg);
    if (access.lists_tables && user && !user->tables.empty()) {  // only the user's own tables
      auto j = json::parse(result); std::string rows;
      if (const auto* list = j.get("rows")) for (const auto& r : list->items) {
        const auto* t = r.get("table");
        if (t && t->is_string() && std::find(user->tables.begin(), user->tables.end(), t->str) != user->tables.end()) { if (!rows.empty()) rows += ','; rows += r.raw; }
      }
      result = "{\"rows\":[" + rows + "]}";
    }
    out.insert(out.end(), result.begin(), result.end());
  }
  else if (cmd == 'V') db.verify();
  else if (cmd == 'R') {
    // Replication: u64 from_end, hash[32], u32 max_bytes, u32 wait_ms -> u64 leader end,
    // u64 leader txid, uuid[16], then whole groups.
    require(n == 1 + 8 + 32 + 4 + 4, "INPUT replication request");
    Hash chain; memcpy(chain.data(), b + 9, 32);
    // The wait field's high bit asks for raw bytes (seeding: a group may arrive in pieces).
    uint32_t wait = get32(b + 45); bool raw = (wait & 0x80000000u) != 0; wait &= 0x7fffffffu;
    // A streaming follower has everything before from_end: its confirmation, counted once
    // read_log has checked that its log continues this one, and before waiting for news.
    auto c = db.read_log(get64(b + 1), chain, std::min<size_t>(get32(b + 41), 32u << 20), 0, raw);
    if (!raw && cfg.sync_replicas > 0) g_sync.ack(&session, get64(b + 1), db.bytes());
    if (c.bytes.empty() && wait) c = db.read_log(get64(b + 1), chain, std::min<size_t>(get32(b + 41), 32u << 20), std::min<uint32_t>(wait, 30000), raw);
    out.resize(1 + 32 + c.bytes.size()); put64(out.data() + 1, c.leader_end); put64(out.data() + 9, c.leader_txid); memcpy(out.data() + 17, c.uuid.data(), 16);
    if (!c.bytes.empty()) memcpy(out.data() + 33, c.bytes.data(), c.bytes.size());
  }
  else if (cmd == 'X') { require(!g_cluster_mode, "INPUT this server is in a cluster: the cluster elects its leader"); if (g_readonly) { g_readonly = false; std::cout << "KESTRODB PROMOTED" << std::endl; } }
  else if (cmd == 'K') {
    Store::CompactResult r;
    try { r = db.compact(); } catch (...) { g_counters.compaction_failures.fetch_add(1, std::memory_order_relaxed); throw; }
    g_counters.compactions.fetch_add(1, std::memory_order_relaxed); g_counters.last_compaction_ms = unix_ms();
    auto s = std::string("{\"before\":")+std::to_string(r.before)+",\"after\":"+std::to_string(r.after)+",\"keys\":"+std::to_string(r.keys)+"}"; out.insert(out.end(), s.begin(), s.end()); }
  else if (cmd == 'P' || cmd == 'C') {
    // Only into the server's backup directory, by a plain file name: a client never names a
    // path on the server's machine (which could create files anywhere it may write).
    require(!cfg.backup_dir.empty(), "INPUT backups through the server need the server's --backup-dir (they are written there)");
    bool plain = !arg.empty() && arg.size() <= 200 && arg != "." && arg != ".." && arg[0] != '.';
    for (char c : arg) if (c == '/' || c == '\\' || c == ':' || c == '\0' || uint8_t(c) < 32) plain = false;
    require(plain, "INPUT a backup through the server is a plain file name (written in the server's --backup-dir)");
    try { db.snapshot((std::filesystem::u8path(cfg.backup_dir) / std::filesystem::u8path(arg)).u8string(), cmd == 'C'); } catch (...) { backup_done(false); throw; }
    backup_done(true);
  }
  else if (cmd == 'L') { auto j = connections_json(ci.id); out.insert(out.end(), j.begin(), j.end()); }
  else if (cmd == 'D') {
    require(n == 9, "INPUT kill request: u64 connection id"); uint64_t id = get64(b + 1);
    require(id != ci.id, "INPUT that is this connection; close it instead");
    require(g_registry.kill(id), "INPUT no connection " + std::to_string(id));
  }
  else if (cmd == 'O') { auto j = slow_json(n >= 5 ? std::min<size_t>(get32(b + 1), 128) : 20); out.insert(out.end(), j.begin(), j.end()); }
  else if (cmd == 'T') { auto j = render_metrics(); out.insert(out.end(), j.begin(), j.end()); }
  else if (cmd == 'S') {
    auto s = std::string("{\"keys\":") + std::to_string(db.count()) + ",\"bytes\":" + std::to_string(db.bytes()) + ",\"txid\":" + std::to_string(db.txid()) +
      ",\"server\":true,\"connections\":" + std::to_string(clients.load()) + ",\"readMode\":\"" + (cfg.options->reads == ReadMode::Mapped ? "mapped" : "pread") +
      "\",\"openedFrom\":\"" + db.opened_from() + "\",\"verification\":\"" + db.verification() + "\",\"indexMemory\":" + std::to_string(db.index_memory()) + ",\"indexFileBytes\":" + std::to_string(db.index_file_bytes()) + ",\"orderedMemory\":" + std::to_string(db.ordered_memory()) + ",\"orderedFileBytes\":" + std::to_string(db.ordered_file_bytes()) + ",\"snapshotYields\":" + std::to_string(db.snapshot_yields()) + ",\"snapshotMemory\":" + std::to_string(db.snapshot_memory()) + ",\"indexMode\":\"" + db.index_mode() +
      "\",\"platform\":\"" + platform::name + "\",\"fileSync\":\"" + platform::sync_name + "\",\"tls\":" + (tls ? "true" : "false") +
      ",\"user\":" + (user ? "\"" + user->name + "\",\"role\":\"" + user->role + "\"" : std::string("null")) +
      ",\"readOnly\":" + (g_readonly ? "true" : "false") + ",\"replica\":" + (g_replica_json ? g_replica_json() : std::string("null")) +
      ",\"sync\":" + (cfg.sync_replicas > 0 ? g_sync.json(cfg.sync_replicas) : std::string("null")) +
      ",\"cluster\":" + (g_cluster_json ? g_cluster_json() : std::string("null")) + ",\"listenersDropped\":" + std::to_string(g_listeners_dropped.load()) +
      ",\"version\":\"" KESTRO_VERSION "\",\"serverRole\":\"" + role_now() + "\",\"uptimeMs\":" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_started).count()) +
      ",\"connectionId\":" + std::to_string(ci.id) + ",\"slowQueryMs\":" + std::to_string(g_slow.threshold_ns / 1000000) + "}";
    out.insert(out.end(), s.begin(), s.end());
  } else throw std::runtime_error("INPUT unknown command");
  return out;
}

void serve_connection(Conn& s, Store& db, Store::SqlSession& session, Committer& committer, const Config& cfg, std::atomic<int>& clients);
// One connection; its SQL session ends with it (an open transaction is rolled back).
void serve(net::Socket sock, bool tls, Store& db, Committer& committer, const Config& cfg, std::atomic<int>& clients, const std::string& peer) {
  Conn s{sock};s.peer = peer;
  s.info.peer = peer; s.info.tls = tls; s.info.sock = sock; g_registry.add(s.info);
  struct Unregister { ConnInfo& c; ~Unregister() { g_registry.remove(c); } } unregister{s.info};  // its counts join the totals
#ifdef KESTRO_TLS
  if (tls) {
    SSL* ssl = SSL_new(static_cast<SSL_CTX*>(cfg.tls)); if (!ssl) return;
    SSL_set_fd(ssl, int(sock));
    s.set_timeout(cfg.auth_timeout ? cfg.auth_timeout : 10);  // a client must finish the handshake promptly
    if (SSL_accept(ssl) <= 0) { SSL_free(ssl); return; }
    s.ssl = ssl;
    // The handshake accepts any client certificate (see --tls-client-ca); it is judged here,
    // so that the client hears why (serve_connection) before any request is served.
    if (X509* x = cfg.client_cert ? SSL_get1_peer_certificate(ssl) : nullptr) {
      s.cert.present = true;
      long v = SSL_get_verify_result(ssl);
      if (v != X509_V_OK)
        s.cert.error = v == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY || v == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT || v == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN || v == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT ||
                       v == X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE ? "not issued by a trusted CA" : v == X509_V_ERR_CERT_HAS_EXPIRED || v == X509_V_ERR_CERT_NOT_YET_VALID ? "expired or not yet valid"
                     : v == X509_V_ERR_INVALID_PURPOSE ? "not for this use (extended key usage)" : X509_verify_cert_error_string(v);
      unsigned char* der = nullptr; int n = i2d_X509(x, &der);
      if (n > 0) { s.cert.sha256 = digest(der, size_t(n)); OPENSSL_free(der); }
      char cn[256]; int k = X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_commonName, cn, sizeof cn);
      if (k > 0) s.cert.cn.assign(cn, size_t(k));
      X509_free(x);
    }
  }
#elif defined(KESTRO_WINTLS)
  if (tls) {
    int deadline = cfg.auth_timeout ? cfg.auth_timeout : 10;  // a client must finish the handshake promptly
    s.set_timeout(deadline);
    auto& creds = *static_cast<wintls::Credentials*>(cfg.tls);
    auto* t = wintls::Stream::accept(creds, sock, deadline);
    if (!t) return;
    s.ssl = t;
    // The client's certificate (--tls-client-ca), judged in serve_connection.
    if (PCCERT_CONTEXT c = cfg.client_cert ? t->client_certificate() : nullptr) {
      s.cert.present = true;
      try { if (long e = wintls::chain_error(creds.client_engine, c, false, nullptr)) s.cert.error = wintls::chain_reason(e); } catch (const std::exception& e) { s.cert.error = e.what(); }
      s.cert.sha256 = digest(c->pbCertEncoded, c->cbCertEncoded);
      wchar_t cn[256]; DWORD k = CertGetNameStringW(c, CERT_NAME_ATTR_TYPE, 0, const_cast<char*>(szOID_COMMON_NAME), cn, 256);
      if (k > 1) s.cert.cn = platform::utf8(std::wstring(cn, k - 1));
      CertFreeCertificateContext(c);
    }
  }
#else
  (void)tls;
#endif
  Store::SqlSession session; ConnListener listener(s, cfg.notify_queue);
  if (!tls) session.listener = &listener;
  auto finish = [&] {
    g_sync.gone(&session);
    db.remove_listener(listener); listener.finish();
    db.end_session(session);
#ifdef KESTRO_TLS
    if (s.ssl) { SSL_shutdown(static_cast<SSL*>(s.ssl)); SSL_free(static_cast<SSL*>(s.ssl)); }
#elif defined(KESTRO_WINTLS)
    if (s.ssl) { auto* t = static_cast<wintls::Stream*>(s.ssl); t->shutdown(); delete t; }
#endif
  };
  try { serve_connection(s, db, session, committer, cfg, clients); } catch (...) { finish(); throw; }
  finish();
}
void serve_connection(Conn& s, Store& db, Store::SqlSession& session, Committer& committer, const Config& cfg, std::atomic<int>& clients) {
  struct Frame { const unsigned char* data; size_t size; };
  Bytes in(4096), replies;  // grows to the largest frame this connection sends
  size_t start = 0, end = 0; bool authed = cfg.users.empty(); const User* user = nullptr;
  ConnInfo& ci = s.info; const bool timed = g_timing.load(std::memory_order_relaxed);
  Clock::time_point mark; if (timed) mark = Clock::now();  // when the request now served began (one clock read per request)
  auto reply = [&](const Bytes& b0) {
    // A reply is at most one frame (64 MB): a larger one would not fit its length field.
    static const Bytes too_large = error_reply("LIMIT the reply is larger than 64 MB; ask for fewer rows (LIMIT/OFFSET)");
    const Bytes& b = b0.size() > MAX_GROUP + 4096 ? too_large : b0;  // room for a whole group with its reply header
    unsigned char h[4]; put32(h, uint32_t(b.size())); replies.insert(replies.end(), h, h + 4); replies.insert(replies.end(), b.begin(), b.end()); };
  auto flush = [&] {
    bool ok = true;
    if (!replies.empty()) { std::unique_lock<std::mutex> g(s.send_m, std::defer_lock); if (s.shared_send) g.lock(); ok = s.send(replies.data(), replies.size()); bump(ci.st.bytes_out, replies.size()); }
    replies.clear(); return ok;
  };
  std::string refused;  // a TLS client's certificate that is bad (or none, when required)
  if (s.ssl && cfg.client_cert) {
    std::string why = !s.cert.error.empty() ? "refused: " + s.cert.error + " (--tls-client-ca)" : !s.cert.present && cfg.client_cert == 2 ? "required: this server accepts only clients with a certificate from its CA" : "";
    if (!why.empty()) {
      std::string cn; for (char ch : s.cert.cn) cn += (unsigned char)ch < 32 || ch == '"' ? '?' : ch;  // the client's text, unverified: one log line
      std::cerr << "KESTRODB TLS client certificate " << (cn.empty() ? "" : "\"" + cn + "\" ") << "from " << s.peer << ": " << why << std::endl;
      refused = "AUTH TLS client certificate " + why;
    }
    // A user bound to this certificate without a token is logged in by it.
    else for (const auto& u : cfg.users) if (!u.has_token && cert_matches(u, s.cert)) { authed = true; user = &u; g_registry.set_user(ci, u.name, u.role); break; }
  }
  try {
    for (;;) {
      std::vector<Frame> frames; size_t pos = start;
      // Before login only one small frame at a time: a client that has not shown a token
      // cannot make the server hold large buffers.
      while (end - pos >= 4) {
        auto n = get32(in.data() + pos);
        require(n > 0 && n <= (authed ? MAX_GROUP : 64 * 1024), authed ? "INPUT request length" : "AUTH send the access token first (requests before it are at most 64 KB)");
        if (end - pos - 4 < n) break;
        frames.push_back({in.data() + pos + 4, n}); pos += 4 + n;
        if (!authed) break;
      }
      for (size_t i = 0; i < frames.size();) {
        char cmd = char(frames[i].data[0]);
        // A refused certificate: the reply to the first request, which is not served. (Read
        // first: closing with a request unread would reset the connection, losing the reply.)
        if (!refused.empty()) { g_counters.auth_cert.fetch_add(1, std::memory_order_relaxed); reply(error_reply(refused)); flush(); return; }
        if (cmd != 'B') bump(ci.st.req[req_of(cmd)]);
        if (cmd == 'A') {
          if (cfg.users.empty()) { authed = true; reply(Bytes(1, 0)); i++; continue; }
          if (!s.peer.empty() && g_auth_failures.blocked(s.peer)) { g_counters.auth_rate.fetch_add(1, std::memory_order_relaxed); std::this_thread::sleep_for(std::chrono::milliseconds(250)); reply(error_reply("AUTH too many failed logins from this address; wait a minute")); flush(); return; }
          auto h = digest(frames[i].data + 1, frames[i].size - 1);
          // Every user is compared, in constant time, so timing reveals nothing.
          const User* match = nullptr;
          for (const auto& u : cfg.users) { unsigned diff = 0; for (size_t k = 0; k < h.size(); k++) diff |= unsigned(h[k] ^ u.hash[k]); if (!diff && u.has_token) match = &u; }
          // A user bound to a certificate needs both: the token and its certificate.
          bool cert_ok = !match || match->cert.empty() || cert_matches(*match, s.cert);
          if (match && cert_ok) { authed = true; user = match; g_registry.set_user(ci, match->name, match->role); reply(Bytes(1, 0)); }
          else { (match ? g_counters.auth_cert : g_counters.auth_token).fetch_add(1, std::memory_order_relaxed); if (!s.peer.empty()) g_auth_failures.failed(s.peer); std::this_thread::sleep_for(std::chrono::milliseconds(250)); reply(error_reply(match ? "AUTH user " + match->name + " must also show its TLS client certificate" : "AUTH invalid token")); flush(); return; }  // slows guessing
          i++; continue;
        }
        if (!authed) { g_counters.auth_missing.fetch_add(1, std::memory_order_relaxed); reply(error_reply("AUTH required: send the access token first")); flush(); return; }
        if (cmd == 'Q') { reply(Bytes(1, 0)); flush(); return; }
        if (cmd == 'B' && session.active) {
          // The committer would wait for this connection's own transaction.
          bump(ci.st.req[RqBatch]); g_request_errors.add("INPUT");
          reply(error_reply("INPUT a SQL transaction is open on this connection; COMMIT or ROLLBACK it before key/value writes")); i++; continue;
        }
        if (cmd == 'B') {
          // Consecutive pipelined batches go to the committer together.
          size_t j = i; while (j < frames.size() && frames[j].data[0] == 'B' && j - i < 4096) j++;
          bump(ci.st.req[RqBatch], j - i);
          Committer::Job job; std::vector<Bytes> early(j - i); std::vector<size_t> slot;
          for (size_t k = i; k < j; k++) {
            try {
              auto ops = parse_batch(frames[k].data, frames[k].size);
              for (const auto& o : ops) {
                allow_key(user, NeedWrite, o.key);
                // Rows of SQL tables change through SQL, where their constraints are checked;
                // administrators may still write them directly.
                auto z = o.key.find('\0');
                if (user && user->role != "admin" && z != std::string::npos && db.is_sql_table(std::string_view(o.key).substr(0, z)))
                  denied("user " + user->name + ": " + o.key.substr(0, z) + " is a SQL table; change its rows with SQL (its constraints apply)");
              }
              job.txs.push_back(std::move(ops)); job.bytes += frames[k].size; slot.push_back(k - i);
            }
            catch (const std::exception& e) { early[k - i] = error_reply(e.what()); }
          }
          if (!job.txs.empty()) {
            committer.commit(job);
            if (!job.error && cfg.sync_replicas > 0) { try { g_sync.wait(db.bytes(), cfg); } catch (...) { job.error = std::current_exception(); } }
            for (size_t t = 0; t < slot.size(); t++) {
              if (job.error) early[slot[t]] = error_reply(job.error);
              else if (job.results[t].error) early[slot[t]] = error_reply(job.results[t].error);
              else { Bytes ok(9, 0); put64(ok.data() + 1, job.results[t].version); early[slot[t]] = std::move(ok); }
            }
          }
          for (auto& r : early) { if (r[0] == 255) g_request_errors.add(error_code(std::string_view(reinterpret_cast<const char*>(r.data() + 1), r.size() - 1))); reply(r); }
          if (timed) mark = Clock::now();
          i = j; continue;
        }
        if ((cmd == 'R' || (cmd == 'Z' && frames[i].size > 1 && frames[i].data[1] == 'f')) && !ci.replicating.load(std::memory_order_relaxed)) ci.replicating = true;
        Bytes r; std::string error;
        try { r = handle(db, session, cfg, clients, frames[i].data, frames[i].size, user, s.ssl != nullptr, ci); } catch (const std::exception& e) { error = e.what(); r = error_reply(error); }
        if (!error.empty()) g_request_errors.add(error_code(error));
        if (cmd == 'E') {
          // By kind; timed (the histogram, the slow-query log); the connection's state for admin connections.
          std::string_view text, params; const auto* f = frames[i].data; size_t fn = frames[i].size;
          if (fn >= 5 && uint64_t(get32(f + 1)) + 5 <= fn) { text = std::string_view(reinterpret_cast<const char*>(f + 5), get32(f + 1)); params = std::string_view(reinterpret_cast<const char*>(f + 5) + text.size(), fn - 5 - text.size()); }
          int kind = sql_kind_of(text); bump(ci.st.sql[kind]);
          if (!error.empty()) g_sql_errors.add(error_code(error));
          if (timed) {
            auto now = Clock::now(); uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count()); mark = now;
            ci.st.statement.add(ns);
            if (g_slow.threshold_ns && ns >= g_slow.threshold_ns) { bump(ci.st.slow); log_slow(ci, ns, kind, text, params, std::string_view(reinterpret_cast<const char*>(r.data() + 1), r.size() - 1), error); }
          }
          if (ci.in_tx.load(std::memory_order_relaxed) != session.active) ci.in_tx.store(session.active, std::memory_order_relaxed);
          bool listening = session.listener && !session.listener->channels.empty();
          if (ci.listening.load(std::memory_order_relaxed) != listening) ci.listening.store(listening, std::memory_order_relaxed);
        } else if (timed) mark = Clock::now();
        reply(r);
        if (cmd == 'E' && !s.shared_send && session.listener && !session.listener->channels.empty()) static_cast<ConnListener*>(session.listener)->start();
        i++;
        if (replies.size() >= 4 * 1024 * 1024 && !flush()) return;
      }
      start = pos;
      if (!flush()) return;
      if (start == end) start = end = 0;
      else if (start > 0) { memmove(in.data(), in.data() + start, end - start); end -= start; start = 0; }
      if (end >= 4 && end - 4 >= get32(in.data())) continue;  // a whole request is here already (sent with the login)
      // The buffer grows with the bytes that arrive (doubling, up to the frame's length),
      // never to a length a client merely declared; a large one is released when idle.
      if (end == in.size()) { size_t need = end >= 4 ? size_t(4) + get32(in.data()) : 0; size_t grow = in.size() * 2; if (need > in.size()) grow = std::min(grow, need); in.resize(std::max(grow, in.size() + 4096)); }
      else if (end == 0 && in.size() > (4u << 20)) Bytes(4096).swap(in);
      // Before login: a deadline. In a transaction: an idle limit (it holds the write gate;
      // closing the connection rolls it back). Otherwise --idle-timeout (0: none).
      s.set_timeout(!authed ? cfg.auth_timeout : session.active ? cfg.transaction_timeout : s.shared_send ? 0 : cfg.idle_timeout);  // a listener may be idle
      auto got = s.recv(in.data() + end, in.size() - end);
      if (got <= 0) return;
      end += size_t(got); bump(ci.st.bytes_in, size_t(got));
      if (timed) mark = Clock::now();
    }
  } catch (const std::exception& e) { reply(error_reply(e.what())); flush(); }
}

// --replica-cert/--replica-key: the client certificate a follower or cluster member shows over
// tls:// (to a leader or peer with --tls-client-ca). Set before any link opens.
struct LinkCert {
  std::string cert, key;
#ifdef KESTRO_WINTLS
  std::unique_ptr<wintls::Credentials> creds;  // the key, imported once
#endif
} g_link_cert;
// A follower's connection to its leader: tcp://HOST:PORT, or tls://HOST:PORT (OpenSSL on
// Linux/macOS, Schannel on Windows; the certificate is checked against the system CAs or ca_file).
// Also the admin command's connection (who: how errors name the server; unix://PATH too).
struct LeaderLink {
  net::Socket sock = net::invalid; void* ssl = nullptr; void* ctx = nullptr; const char* who = "the leader";
  ~LeaderLink() { close(); }
  void close() {
#ifdef KESTRO_TLS
    if (ssl) { SSL_free(static_cast<SSL*>(ssl)); ssl = nullptr; }
    if (ctx) { SSL_CTX_free(static_cast<SSL_CTX*>(ctx)); ctx = nullptr; }
#elif defined(KESTRO_WINTLS)
    if (ssl) { delete static_cast<wintls::Stream*>(ssl); ssl = nullptr; }
#endif
    net::close(sock); sock = net::invalid;
  }
  void open(const std::string& address, const std::string& token, const std::string& ca_file, int connect_ms = 0, int io_seconds = 60) {
    close();
    bool secure = address.rfind("tls://", 0) == 0, local = address.rfind("unix://", 0) == 0;
    std::string hp = local ? address.substr(7) : address.rfind("tcp://", 0) == 0 || secure ? address.substr(6) : address;
#ifdef _WIN32
    require(!local, "UNSUPPORTED unix:// on Windows; use tcp://");
    sock = net::connect_tcp(hp, connect_ms);
#else
    sock = local ? net::connect_unix(hp) : net::connect_tcp(hp, connect_ms);
#endif
    net::set_timeout(sock, io_seconds);
    if (secure) {
#ifdef KESTRO_TLS
      auto* c = SSL_CTX_new(TLS_client_method()); ctx = c; require(c != nullptr, "IO TLS context");
      SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
      if (ca_file.empty()) SSL_CTX_set_default_verify_paths(c); else require(SSL_CTX_load_verify_locations(c, ca_file.c_str(), nullptr) == 1, "INPUT --replica-ca");
      SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
      if (!g_link_cert.cert.empty()) require(SSL_CTX_use_certificate_chain_file(c, g_link_cert.cert.c_str()) == 1 && SSL_CTX_use_PrivateKey_file(c, g_link_cert.key.c_str(), SSL_FILETYPE_PEM) == 1 && SSL_CTX_check_private_key(c) == 1, "INPUT --replica-cert or --replica-key");
      auto* t = SSL_new(c); ssl = t; SSL_set_fd(t, int(sock));
      std::string host = hp.substr(0, hp.rfind(':'));
      SSL_set1_host(t, host.c_str()); SSL_set_tlsext_host_name(t, host.c_str());
      if (SSL_connect(t) != 1) throw std::runtime_error("IO TLS handshake with the leader failed (certificate?)");
#elif defined(KESTRO_WINTLS)
      ssl = wintls::Stream::connect(sock, hp.substr(0, hp.rfind(':')), ca_file, g_link_cert.creds.get());
#else
      (void)ca_file;
      throw std::runtime_error("UNSUPPORTED tls:// in this build (no OpenSSL)");
#endif
    }
    if (!token.empty()) call('A', Bytes(token.begin(), token.end()));
  }
  bool send_all(const unsigned char* p, size_t n) {
#ifdef KESTRO_TLS
    if (ssl) { while (n) { int w = SSL_write(static_cast<SSL*>(ssl), p, int(std::min<size_t>(n, 1 << 30))); if (w <= 0) return false; p += w; n -= size_t(w); } return true; }
#elif defined(KESTRO_WINTLS)
    if (ssl) return static_cast<wintls::Stream*>(ssl)->write(p, n);
#endif
    return net::send_all(sock, p, n);
  }
  void recv_all(unsigned char* p, size_t n) {
    while (n) {
      long long got;
#ifdef KESTRO_TLS
      if (ssl) { int r = SSL_read(static_cast<SSL*>(ssl), p, int(std::min<size_t>(n, 1 << 30))); got = r > 0 ? r : -1; } else
#elif defined(KESTRO_WINTLS)
      if (ssl) got = static_cast<wintls::Stream*>(ssl)->read(p, n); else
#endif
      got = net::recv_some(sock, p, n);
      if (got <= 0) throw std::runtime_error(std::string("IO ") + who + " closed the connection");
      p += got; n -= size_t(got);
    }
  }
  Bytes call(char cmd, const Bytes& payload) {
    Bytes f(5 + payload.size()); put32(f.data(), uint32_t(1 + payload.size())); f[4] = (unsigned char)cmd;
    if (!payload.empty()) memcpy(f.data() + 5, payload.data(), payload.size());
    if (!send_all(f.data(), f.size())) throw std::runtime_error(std::string("IO cannot send to ") + who);
    unsigned char h[4]; recv_all(h, 4); auto len = get32(h); if (len < 1 || len > MAX_GROUP + 4096) throw std::runtime_error(std::string("IO bad reply length from ") + who);  // a whole group plus the reply header
    Bytes reply(len); recv_all(reply.data(), len);
    if (reply[0] == 255) throw std::runtime_error(std::string(reply.begin() + 1, reply.end()));
    return reply;
  }
  Store::LogChunk fetch(uint64_t from, const Hash& chain, uint32_t max_bytes, uint32_t wait_ms) {
    Bytes req(48); put64(req.data(), from); memcpy(req.data() + 8, chain.data(), 32); put32(req.data() + 40, max_bytes); put32(req.data() + 44, wait_ms);
    auto reply = call('R', req);
    require(reply.size() >= 33, "IO short replication reply");
    Store::LogChunk c; c.leader_end = get64(reply.data() + 1); c.leader_txid = get64(reply.data() + 9); memcpy(c.uuid.data(), reply.data() + 17, 16);
    c.bytes.assign(reply.begin() + 33, reply.end());
    return c;
  }
};
// Follower: keeps applying the leader's new groups; reconnects with a back-off.
struct Replicator {
  Store* db = nullptr; std::string leader, token, ca;
  std::thread thread; std::atomic<bool> stop{false};
  std::mutex m; std::string state = "starting", last_error; uint64_t leader_txid = 0, leader_end = 0; int64_t last_contact_ms = 0;
  Clock::time_point caught_up = Clock::now();  // when this follower last had everything the leader had
  void run() {
    int backoff = 100;
    while (!stop && g_readonly) {
      LeaderLink link;
      try {
        link.open(leader, token, ca);
        { std::lock_guard<std::mutex> g(m); state = "streaming"; }
        backoff = 100;
        while (!stop && g_readonly) {
          auto c = link.fetch(db->bytes(), db->chain(), 8u << 20, 1000);
          if (!c.bytes.empty()) db->apply_replicated(c.bytes.data(), c.bytes.size());
          bool level = db->bytes() >= c.leader_end;
          std::lock_guard<std::mutex> g(m); leader_txid = c.leader_txid; leader_end = c.leader_end; last_contact_ms = int64_t(Store::now_ms()); if (level) caught_up = Clock::now();
        }
      } catch (const std::exception& e) {
        std::string what = e.what();
        { std::lock_guard<std::mutex> g(m); last_error = what; state = what.rfind("STALE", 0) == 0 || what.rfind("DIVERGED", 0) == 0 ? "stopped" : "reconnecting"; }
        std::cerr << "KESTRODB REPLICA " << what << std::endl;
        if (what.rfind("STALE", 0) == 0 || what.rfind("DIVERGED", 0) == 0 || what.rfind("AUTH", 0) == 0) return;  // needs an operator
        for (int t = 0; t < backoff / 50 && !stop; t++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        backoff = std::min(backoff * 2, 5000);
      }
    }
    std::lock_guard<std::mutex> g(m); if (state != "stopped") state = g_readonly ? "stopped" : "promoted";
  }
  void start() { if (!leader.empty()) thread = std::thread([this] { run(); }); }
  void finish() { stop = true; if (thread.joinable()) thread.join(); }
  // Behind the leader: transactions, bytes, and seconds since it last had everything (metrics, readiness).
  struct Lag { bool connected = false; uint64_t transactions = 0, bytes = 0; double seconds = 0; };
  Lag lag() {
    uint64_t tx = db->txid(), end = db->bytes(); std::lock_guard<std::mutex> g(m); Lag l;
    l.connected = state == "streaming"; l.transactions = leader_txid > tx ? leader_txid - tx : 0; l.bytes = leader_end > end ? leader_end - end : 0;
    l.seconds = std::chrono::duration<double>(Clock::now() - caught_up).count(); return l;
  }
  std::string json() {
    if (leader.empty()) return "null";
    std::lock_guard<std::mutex> g(m);
    auto lag = leader_txid > db->txid() ? leader_txid - db->txid() : 0;
    std::string e; for (char ch : last_error) { if (ch == '"' || ch == '\\') e += '\\'; if (uint8_t(ch) >= 32) e += ch; }
    return "{\"leader\":\"" + leader + "\",\"state\":\"" + (g_readonly ? state : std::string("promoted")) + "\",\"leaderTxid\":" + std::to_string(leader_txid) +
      ",\"lagTransactions\":" + std::to_string(lag) + ",\"lastContactMs\":" + std::to_string(last_contact_ms) + ",\"lastError\":\"" + e + "\"}";
  }
};

#include "cluster.inc"
#include "monitor.inc"

// "90", "500ms", "30s", "15m", "1h", "7d" -> milliseconds (plain numbers are seconds).
uint64_t parse_duration(const std::string& v) {
  size_t i = 0; while (i < v.size() && v[i] >= '0' && v[i] <= '9') i++;
  require(i > 0, ("INPUT duration " + v).c_str());
  uint64_t n = std::stoull(v.substr(0, i)); std::string u = v.substr(i);
  uint64_t ms = u == "ms" ? n : u == "" || u == "s" ? n * 1000 : u == "m" ? n * 60000 : u == "h" ? n * 3600000 : u == "d" ? n * 86400000 : 0;
  require(ms > 0, ("INPUT duration " + v + " (use ms, s, m, h or d)").c_str());
  return ms;
}
// Unix milliseconds, or ISO 8601 UTC "2026-11-17T14:32:10Z" (fractions and +HH:MM offsets allowed).
int64_t parse_time(const std::string& v) {
  if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) return int64_t(std::stoll(v));
  int Y, M, D, h = 0, m = 0; double sec = 0; char tail[16] = {0};
  int n = std::sscanf(v.c_str(), "%d-%d-%dT%d:%d:%lf%15s", &Y, &M, &D, &h, &m, &sec, tail);
  require(n >= 3 && M >= 1 && M <= 12 && D >= 1 && D <= 31, ("INPUT time " + v + " (use unix milliseconds or 2026-11-17T14:32:10Z)").c_str());
  // Days since 1970-01-01 (civil calendar).
  int y = Y - (M <= 2); int era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = unsigned(y - era * 400);
  unsigned doy = unsigned((153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1); unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = int64_t(era) * 146097 + int64_t(doe) - 719468;
  int64_t ms = ((days * 24 + h) * 60 + m) * 60000 + int64_t(sec * 1000);
  std::string t = tail;
  if (!t.empty() && (t[0] == '+' || t[0] == '-')) { int oh = 0, om = 0; std::sscanf(t.c_str() + 1, "%d:%d", &oh, &om); int64_t off = (int64_t(oh) * 60 + om) * 60000; ms += t[0] == '+' ? -off : off; }
  else require(t.empty() || t == "Z", ("INPUT time zone in " + v + " (use Z or +HH:MM)").c_str());
  return ms;
}
std::string utc_stamp() {
  auto t = std::time(nullptr); std::tm g{};
#ifdef _WIN32
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  char b[32]; std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &g); return b;
}
// Scheduled backups (kept: the newest N) and continuous archiving, on one thread.
struct Maintenance {
  Store& db; std::string backup_dir, archive_dir, stem; uint64_t backup_every = 0, archive_every = 1000; size_t keep = 7;
  std::thread thread; std::mutex m; std::condition_variable cv; bool stop = false;
  void backup_now() {
    try {
      std::filesystem::create_directories(std::filesystem::u8path(backup_dir));
      auto path = backup_dir + "/" + stem + "-" + utc_stamp() + ".kdb";
      if (std::filesystem::exists(std::filesystem::u8path(path))) return;  // twice in one second
      auto p = db.backup(path);
      std::cout << "KESTRODB BACKUP " << path << " txid " << p.txid << std::endl; backup_done(true);
      std::vector<std::filesystem::path> files;
      for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(backup_dir))) {
        auto n = e.path().filename().u8string();
        if (e.is_regular_file() && n.rfind(stem + "-", 0) == 0 && n.size() == stem.size() + 20 && n.substr(n.size() - 4) == ".kdb") files.push_back(e.path());
      }
      std::sort(files.begin(), files.end());
      while (files.size() > keep) { std::filesystem::remove(files.front()); files.erase(files.begin()); }
    } catch (const std::exception& e) { backup_done(false); std::cerr << "KESTRODB BACKUP FAILED " << e.what() << std::endl; }
  }
  void archive_now() {
    try { db.archive(archive_dir); }
    catch (const std::exception& e) { std::cerr << "KESTRODB ARCHIVE FAILED " << e.what() << std::endl; }
  }
  void start() {
    if (backup_dir.empty() && archive_dir.empty()) return;
    thread = std::thread([this] {
      auto now = [] { return std::chrono::steady_clock::now(); };
      auto next_backup = now() + std::chrono::milliseconds(backup_every), next_archive = now();
      std::unique_lock<std::mutex> l(m);
      while (!stop) {
        auto wake = archive_dir.empty() ? next_backup : backup_dir.empty() ? next_archive : std::min(next_backup, next_archive);
        cv.wait_until(l, wake, [&] { return stop; });
        if (stop) break;
        l.unlock();
        if (!archive_dir.empty() && now() >= next_archive) { archive_now(); next_archive = now() + std::chrono::milliseconds(archive_every); }
        if (!backup_dir.empty() && now() >= next_backup) { backup_now(); next_backup = now() + std::chrono::milliseconds(backup_every); }
        l.lock();
      }
    });
  }
  // Stops the thread; the archive takes one last step so it holds every commit.
  void finish() {
    if (!thread.joinable()) return;
    { std::lock_guard<std::mutex> g(m); stop = true; } cv.notify_all(); thread.join();
    if (!archive_dir.empty()) archive_now();
  }
};

#include "admin.inc"

int server_main(const std::vector<std::string>& args) {
  try {
    // Fresh state: after a cluster rejoin the server runs again in the same process.
    stopping = false; { std::lock_guard<std::mutex> g(listeners_m); listeners.clear(); listener_tls.clear(); }
    g_readonly = false; g_cluster_leader = false; g_cluster_mode = false; g_stopping_sync = false; g_rejoin = Rejoin{}; set_leader_hint("");
    { std::lock_guard<std::mutex> g(g_sync.m); g_sync.acked.clear(); g_sync.degraded = false; g_sync.waits = g_sync.timeouts = 0; g_sync.replicas = 0; }
    g_cluster_json = nullptr; g_cluster_handle = nullptr;
    std::string db_path, token, token_file, users_file, tls_cert, tls_key, tls_pfx, tls_pfx_password, tls_client_ca, tls_client_cert; std::vector<std::string> tcp, unix_paths; bool create = false, stdin_stop = false;
    g_link_cert = LinkCert{};
    if (args.size() == 2 && args[1] == "--hash-token") {
      // Reads a token from standard input (not the command line, which is visible to other
      // users and kept in shell history) and prints the users-file form.
      std::string t; std::getline(std::cin, t); while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
      require(t.size() >= 16, "INPUT tokens must be at least 16 characters");
      std::cout << "sha256:" << hex_of(digest(t.data(), t.size())) << std::endl; return 0;
    }
    unsigned socket_mode = 0600; int max_clients = 1024, max_per_ip = 256; bool no_auth = false; Options options; Config limits;
    std::string cluster_list, cluster_self, cluster_token_file; int election_ms = 1500;
    std::string metrics_address, metrics_token_file, slow_log; bool metrics_public = false, slow_params = false; uint64_t ready_lag_ms = 10000, slow_ms = 0, slow_max_mb = 64;
    std::string backup_dir, archive_dir, restore_from, restore_to, replica_of, replica_token_file, replica_ca; uint64_t backup_every = 3600000, archive_every = 1000, restore_txid = 0; size_t backup_keep = 7; int64_t restore_at = -1;
    for (size_t i = 1; i < args.size(); i++) {
      const auto& a = args[i];
      auto value = [&]() -> std::string { require(i + 1 < args.size(), ("INPUT " + a + " needs a value").c_str()); return args[++i]; };
      if (a == "--db") db_path = value();
      else if (a == "--create") create = true;
      else if (a == "--tcp") tcp.push_back(value());
      else if (a == "--unix") unix_paths.push_back(value());
      else if (a == "--socket-mode") socket_mode = unsigned(std::stoul(value(), nullptr, 8));
      else if (a == "--token-file") token_file = value();
      else if (a == "--users") users_file = value();
      else if (a == "--tls-cert") tls_cert = value();
      else if (a == "--tls-key") tls_key = value();
      else if (a == "--tls-pfx") tls_pfx = value();
      else if (a == "--tls-pfx-password-file") tls_pfx_password = value();
      else if (a == "--tls-client-ca") tls_client_ca = value();
      else if (a == "--tls-client-cert") tls_client_cert = value();
      else if (a == "--replica-cert") g_link_cert.cert = value();
      else if (a == "--replica-key") g_link_cert.key = value();
      else if (a == "--max-connections") max_clients = std::stoi(value());
      else if (a == "--max-connections-per-ip") max_per_ip = std::stoi(value());
      else if (a == "--statement-timeout") limits.statement_timeout_ms = int(parse_duration(value()));
      else if (a == "--transaction-timeout") limits.transaction_timeout = int(parse_duration(value()) / 1000);
      else if (a == "--idle-timeout") limits.idle_timeout = int(parse_duration(value()) / 1000);
      else if (a == "--notify-queue") { auto mb = std::stoull(value()); require(mb >= 1 && mb <= 4096, "INPUT --notify-queue is 1 to 4096 MB"); limits.notify_queue = size_t(mb) << 20; }
      else if (a == "--no-auth") no_auth = true;
      else if (a == "--sync-replicas") { limits.sync_replicas = std::stoi(value()); g_sync.replicas = size_t(std::max(0, limits.sync_replicas)); }
      else if (a == "--sync-timeout") limits.sync_timeout_ms = int(parse_duration(value()));
      else if (a == "--sync-strict") limits.sync_strict = true;
      else if (a == "--auth-timeout") limits.auth_timeout = std::max(1, int(parse_duration(value()) / 1000));
      else if (a == "--index-memory") { auto mb = std::stoull(value()); require(mb < (uint64_t(1) << 40), "INPUT --index-memory"); options.indexMemory = size_t(mb) << 20; }
      else if (a == "--ordered-memory") { auto mb = std::stoull(value()); require(mb < (uint64_t(1) << 40), "INPUT --ordered-memory"); options.orderedMemory = size_t(mb) << 20; }
      else if (a == "--pread") options.reads = ReadMode::Pread;
      else if (a == "--mapped") options.reads = ReadMode::Mapped;
      else if (a == "--ordered-sync") options.orderedSync = true;
      else if (a == "--verify-on-open") options.verifyOnOpen = true;
      else if (a == "--stop-on-stdin-eof") stdin_stop = true;
      else if (a == "--backup-dir") backup_dir = value();
      else if (a == "--backup-every") backup_every = parse_duration(value());
      else if (a == "--backup-keep") { backup_keep = size_t(std::stoul(value())); require(backup_keep >= 1, "INPUT --backup-keep must be at least 1"); }
      else if (a == "--archive-dir") archive_dir = value();
      else if (a == "--archive-every") archive_every = parse_duration(value());
      else if (a == "--restore") restore_from = value();
      else if (a == "--replica-of") replica_of = value();
      else if (a == "--replica-token-file") replica_token_file = value();
      else if (a == "--replica-ca") replica_ca = value();
      else if (a == "--cluster") cluster_list = value();
      else if (a == "--cluster-self") cluster_self = value();
      else if (a == "--cluster-token-file") cluster_token_file = value();
      else if (a == "--election-timeout") { election_ms = int(parse_duration(value())); require(election_ms >= 100 && election_ms <= 600000, "INPUT --election-timeout between 100ms and 10m"); }
      else if (a == "--metrics") metrics_address = value();
      else if (a == "--metrics-token-file") metrics_token_file = value();
      else if (a == "--metrics-public") metrics_public = true;
      else if (a == "--ready-max-lag") ready_lag_ms = parse_duration(value());
      else if (a == "--slow-query-ms") { auto v = value(); require(!v.empty() && v.find_first_not_of("0123456789") == std::string::npos && v.size() < 10, "INPUT --slow-query-ms is a number of milliseconds (0: off)"); slow_ms = std::stoull(v); }
      else if (a == "--slow-query-log") slow_log = value();
      else if (a == "--slow-query-log-max") { slow_max_mb = std::stoull(value()); require(slow_max_mb <= 1048576, "INPUT --slow-query-log-max is in MB (0: never rotate)"); }
      else if (a == "--slow-query-params") slow_params = true;
      else if (a == "--to") restore_to = value();
      else if (a == "--txid") restore_txid = std::stoull(value());
      else if (a == "--time") restore_at = parse_time(value());
      else throw std::runtime_error("INPUT unknown option " + a);
    }
    // Restore: a new database file from a backup or an archive, then exit.
    if (!restore_from.empty()) {
      require(!restore_to.empty(), "INPUT --restore SOURCE needs --to NEW_FILE");
      require(!(restore_txid && restore_at >= 0), "INPUT use --txid or --time, not both");
      auto p = Store::restore(restore_from, restore_to, restore_txid, restore_at);
      std::cout << "KESTRODB RESTORED " << restore_to << " txid " << p.txid << " bytes " << p.end << std::endl;
      return 0;
    }
    require(!db_path.empty(), "usage: kestro-server --db FILE [--create] [--tcp HOST:PORT]... [--unix PATH]... [--socket-mode 600] [--users FILE | --token-file FILE] [--tls-cert PEM --tls-key PEM | --tls-pfx PFX [--tls-pfx-password-file FILE]] [--tls-client-ca PEM [--tls-client-cert required|optional]] [--max-connections N] [--max-connections-per-ip N] [--statement-timeout 30s] [--transaction-timeout 60s] [--idle-timeout 0] [--auth-timeout 10s] [--notify-queue 16] [--no-auth] [--sync-replicas N [--sync-timeout 5s] [--sync-strict]] [--backup-dir DIR --backup-every 1h --backup-keep 7] [--archive-dir DIR --archive-every 1s] [--replica-of tcp://LEADER:PORT --replica-token-file FILE [--replica-ca PEM] [--replica-cert PEM --replica-key PEM]] [--cluster tcp://A:P,tcp://B:P,tcp://C:P --cluster-self tcp://A:P --cluster-token-file FILE [--election-timeout 1500ms]] | --restore SOURCE --to NEW_FILE [--txid N | --time 2026-11-17T14:32:10Z] [--index-memory MB] [--ordered-memory MB] [--pread|--mapped] [--ordered-sync] [--verify-on-open] [--metrics HOST:PORT [--metrics-token-file FILE] [--metrics-public] [--ready-max-lag 10s]] [--slow-query-ms N [--slow-query-log FILE] [--slow-query-log-max MB] [--slow-query-params]]\n       kestro-server admin --help");
    if (!token_file.empty()) {
      auto bytes = read_config_file(token_file);
      token.assign(bytes.begin(), bytes.end()); while (!token.empty() && (token.back() == '\n' || token.back() == '\r' || token.back() == ' ')) token.pop_back();
      require(!token.empty(), "INPUT token file is empty");
    } else if (const char* t = std::getenv("KESTRODB_TOKEN")) token = t;
    require(users_file.empty() || token.empty(), "INPUT use --users or --token-file/KESTRODB_TOKEN, not both");
    std::vector<User> users;
    if (!users_file.empty()) users = load_users(users_file);
    else if (!token.empty()) { User u; u.name = "admin"; u.role = "admin"; u.hash = digest(token.data(), token.size()); users.push_back(u); }  // one token: an administrator
    require(tls_cert.empty() == tls_key.empty(), "INPUT --tls-cert and --tls-key go together");
    require(tls_pfx.empty() || tls_cert.empty(), "INPUT use --tls-cert/--tls-key or --tls-pfx, not both");
    require(tls_pfx_password.empty() || !tls_pfx.empty(), "INPUT --tls-pfx-password-file goes with --tls-pfx");
    // Client certificates: --tls-client-ca (the CAs they must lead to) turns them on, required
    // unless --tls-client-cert optional (then a client may show none, but never a bad one).
    require(tls_client_cert.empty() || tls_client_cert == "required" || tls_client_cert == "optional", "INPUT --tls-client-cert must be required or optional");
    require(tls_client_cert.empty() || !tls_client_ca.empty(), "INPUT --tls-client-cert needs --tls-client-ca (the CAs client certificates must lead to)");
    require(tls_client_ca.empty() || !tls_cert.empty() || !tls_pfx.empty(), "INPUT --tls-client-ca needs TLS: --tls-cert/--tls-key or --tls-pfx");
    limits.client_cert = tls_client_ca.empty() ? 0 : tls_client_cert == "optional" ? 1 : 2;
    for (const auto& u : users) if (!u.cert.empty() && !limits.client_cert) { std::cerr << "warning: users file: without --tls-client-ca, users with cert= (" << u.name << ", ...) cannot log in" << std::endl; break; }
    require(g_link_cert.cert.empty() == g_link_cert.key.empty(), "INPUT --replica-cert and --replica-key go together");
    require(g_link_cert.cert.empty() || !replica_of.empty() || !cluster_list.empty(), "INPUT --replica-cert goes with --replica-of or --cluster");
    if (users.empty()) {
      for (const auto& t : tcp) {
        auto host = t.substr(0, t.rfind(':'));
        bool local = host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1" || host.rfind("127.", 0) == 0;
        if (!local && !no_auth && limits.client_cert != 2) throw std::runtime_error("INPUT refusing to listen on " + t + " without authentication: give --users or --token-file, or require client certificates (--tls-client-ca), or --no-auth to allow it");
      }
      std::cerr << (limits.client_cert == 2 ? "warning: no users; every client with a valid certificate is an administrator" : "warning: no authentication; every client is an administrator") << std::endl;
    }
    // --metrics: the address is explicit, and a public one needs a token (or --metrics-public).
    require(metrics_token_file.empty() || !metrics_address.empty(), "INPUT --metrics-token-file goes with --metrics");
    if (!metrics_address.empty()) {
      auto colon = metrics_address.rfind(':'); auto host = colon == std::string::npos ? std::string() : metrics_address.substr(0, colon);
      require(!host.empty() && colon + 1 < metrics_address.size(), "INPUT --metrics expects HOST:PORT with the host given (127.0.0.1:9187 for this machine only)");
      bool local = host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1" || host.rfind("127.", 0) == 0;
      if (!local && metrics_token_file.empty() && !metrics_public) throw std::runtime_error("INPUT refusing to serve metrics on " + metrics_address + " without a token: give --metrics-token-file, or --metrics-public to allow it");
      if (!metrics_token_file.empty()) { g_metrics_server.token = read_token_file(metrics_token_file); require(g_metrics_server.token.size() >= 16, "INPUT the metrics token must be at least 16 characters"); }
    }
    g_slow.threshold_ns = slow_ms * 1000000; g_slow.params = slow_params; g_slow.path = slow_log; g_slow.max_bytes = slow_max_mb << 20;
    g_timing = !metrics_address.empty() || slow_ms > 0;
    { std::lock_guard<std::mutex> g(g_obs.m); g_obs.ready_lag_ms = ready_lag_ms; g_obs.path = db_path; g_obs.stopping = false; }
    void* tls_ctx = nullptr;
#ifdef KESTRO_WINTLS
    std::unique_ptr<wintls::Credentials> tls_credentials;  // freed after every connection ends
#endif
    if (!tls_cert.empty() || !tls_pfx.empty()) {
#ifdef KESTRO_TLS
      require(tls_pfx.empty(), "UNSUPPORTED --tls-pfx outside Windows; use --tls-cert and --tls-key (PEM)");
      SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
      require(ctx != nullptr, "IO cannot create the TLS context");
      SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
#ifdef SSL_OP_NO_RENEGOTIATION
      SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);  // a client cannot make the server redo handshakes
#endif
      if (SSL_CTX_use_certificate_chain_file(ctx, tls_cert.c_str()) != 1 || SSL_CTX_use_PrivateKey_file(ctx, tls_key.c_str(), SSL_FILETYPE_PEM) != 1 || SSL_CTX_check_private_key(ctx) != 1) {
        char buf[256]; ERR_error_string_n(ERR_get_error(), buf, sizeof buf); SSL_CTX_free(ctx);
        throw std::runtime_error(std::string("INPUT TLS certificate or key: ") + buf);
      }
      if (!tls_client_ca.empty()) {
        // Every client is asked for a certificate, and the CAs are named to it; the handshake
        // goes on whatever it shows, and serve() judges it (to tell the client why it is refused).
        STACK_OF(X509_NAME)* names = SSL_load_client_CA_file(tls_client_ca.c_str());
        if (!names || SSL_CTX_load_verify_locations(ctx, tls_client_ca.c_str(), nullptr) != 1) { if (names) sk_X509_NAME_pop_free(names, X509_NAME_free); SSL_CTX_free(ctx); throw std::runtime_error("INPUT --tls-client-ca " + tls_client_ca + " holds no PEM CERTIFICATE"); }
        SSL_CTX_set_client_CA_list(ctx, names);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, [](int, X509_STORE_CTX*) { return 1; });
        static const unsigned char sid[] = "kestrodb";
        SSL_CTX_set_session_id_context(ctx, sid, sizeof sid - 1);  // resumed sessions keep their client certificate
      }
      tls_ctx = ctx;
#elif defined(KESTRO_WINTLS)
      tls_credentials = tls_pfx.empty() ? wintls::load_pem(tls_cert, tls_key) : wintls::load_pfx(tls_pfx, tls_pfx_password);
      if (!tls_client_ca.empty()) wintls::load_client_ca(*tls_credentials, tls_client_ca);
      tls_ctx = tls_credentials.get();
#else
      throw std::runtime_error("UNSUPPORTED TLS in this build (it has no OpenSSL); put a TLS proxy such as stunnel or nginx in front, or build on Linux/macOS with OpenSSL");
#endif
    }
    if (!g_link_cert.cert.empty()) {
      // The follower's client certificate: checked (and on Windows imported) once, now.
#ifdef KESTRO_TLS
      SSL_CTX* c = SSL_CTX_new(TLS_client_method()); require(c != nullptr, "IO TLS context");
      bool ok = SSL_CTX_use_certificate_chain_file(c, g_link_cert.cert.c_str()) == 1 && SSL_CTX_use_PrivateKey_file(c, g_link_cert.key.c_str(), SSL_FILETYPE_PEM) == 1 && SSL_CTX_check_private_key(c) == 1;
      char buf[256] = {}; if (!ok) ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
      SSL_CTX_free(c); require(ok, std::string("INPUT --replica-cert or --replica-key: ") + buf);
#elif defined(KESTRO_WINTLS)
      g_link_cert.creds = wintls::load_pem(g_link_cert.cert, g_link_cert.key, false, "--replica-cert", "--replica-key");
#else
      throw std::runtime_error("UNSUPPORTED --replica-cert in this build (no OpenSSL)");
#endif
    }
    if (tcp.empty() && unix_paths.empty()) tcp.push_back("127.0.0.1:7820");
#ifdef _WIN32
    require(unix_paths.empty(), "UNSUPPORTED --unix on Windows; use --tcp");
#endif
    net::init();
    if (!metrics_address.empty()) g_metrics_server.start(metrics_address);  // live while the database opens (ready: 503)
    Replicator replicator;
    if (!replica_of.empty()) {
      std::string rtoken;
      if (!replica_token_file.empty()) {
        auto bytes = read_config_file(replica_token_file);
        rtoken.assign(bytes.begin(), bytes.end()); while (!rtoken.empty() && (rtoken.back() == '\n' || rtoken.back() == '\r' || rtoken.back() == ' ')) rtoken.pop_back();
      }
      g_readonly = true; g_leader = replica_of;
      replicator.leader = replica_of; replicator.token = rtoken; replicator.ca = replica_ca;
      // First start: copy the leader's log into a new file.
      if (!std::filesystem::exists(std::filesystem::u8path(db_path))) {
        LeaderLink link; link.open(replica_of, rtoken, replica_ca);
        Store::seed_file(db_path, [&](uint64_t from, const Hash& chain) { return link.fetch(from, chain, 32u << 20, 0x80000000u); });  // raw bytes
        std::cout << "KESTRODB SEEDED from " << replica_of << std::endl;
      }
      create = false;
    }
    // A cluster member: its term and vote, and (without its file) a copy of the leader's log.
    std::unique_ptr<Cluster> cluster;
    if (!cluster_list.empty()) {
      require(replica_of.empty(), "INPUT --cluster and --replica-of do not go together (cluster members follow the elected leader)");
      require(!cluster_self.empty(), "INPUT --cluster needs --cluster-self: this node's address as the list gives it");
      cluster = std::make_unique<Cluster>();
      std::vector<std::string> members; std::string cur;
      for (size_t i = 0; i <= cluster_list.size(); i++) { if (i == cluster_list.size() || cluster_list[i] == ',') { if (!cur.empty()) members.push_back(cur); cur.clear(); } else if (cluster_list[i] != ' ') cur += cluster_list[i]; }
      require(std::find(members.begin(), members.end(), cluster_self) != members.end(), "INPUT --cluster-self must be one of the --cluster addresses");
      for (const auto& mbr : members) if (mbr != cluster_self) { require(!cluster->is_peer(mbr), "INPUT a --cluster address appears twice"); cluster->peers.push_back(mbr); }
      auto tf = cluster_token_file.empty() ? replica_token_file : cluster_token_file;
      if (!tf.empty()) {
        auto bytes = read_config_file(tf);
        cluster->token.assign(bytes.begin(), bytes.end()); while (!cluster->token.empty() && (cluster->token.back() == '\n' || cluster->token.back() == '\r' || cluster->token.back() == ' ')) cluster->token.pop_back();
      }
      cluster->self = cluster_self; cluster->ca = replica_ca; cluster->election_ms = election_ms; cluster->state_path = db_path + ".cluster";
      cluster->load();
      g_cluster_mode = true; g_readonly = true;
      limits.cluster = true; limits.sync_replicas = int(cluster->majority() - 1); g_sync.replicas = size_t(limits.sync_replicas);
      bool missing = !std::filesystem::exists(std::filesystem::u8path(db_path));
      if (missing && (g_force_seed || !create)) {
        // A new member (or one whose log was moved aside): it copies the leader's. --create
        // instead starts an empty log (forming a new cluster).
        std::cout << "KESTRODB CLUSTER waiting for a leader to copy the log from" << std::endl;
        for (;;) {
          auto from = cluster->discover();
          if (!from.empty()) {
            try {
              LeaderLink link; link.open(from, cluster->token, replica_ca, 300, 60);
              Store::seed_file(db_path, [&](uint64_t pos, const Hash& chain) { return link.fetch(pos, chain, 32u << 20, 0x80000000u); });
              std::cout << "KESTRODB SEEDED from " << from << std::endl; break;
            } catch (const std::exception& e) { std::cerr << "KESTRODB CLUSTER copy from " << from << ": " << e.what() << std::endl; }
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        g_force_seed = false;
      }
    }
    // --create makes the file on the first start and opens it on later ones (a service can
    // keep the flag across restarts).
    if (create && std::filesystem::exists(std::filesystem::u8path(db_path))) create = false;
    Store db(db_path, create, options);
    replicator.db = &db; if (!replica_of.empty()) g_replica_json = [&replicator] { return replicator.json(); };
    replicator.start();
    // The cluster's threads stop before the store closes.
    struct StopCluster { Cluster* c; ~StopCluster() { if (c) c->finish(); } } stop_cluster{cluster.get()};
    if (cluster) {
      cluster->db = &db;
      g_cluster_json = [c = cluster.get()] { return c->json(); };
      g_cluster_handle = [c = cluster.get()](Store& st, Store::SqlSession& ss, const Config& cf, const unsigned char* b, size_t n) { return c->handle(st, ss, cf, b, n); };
    }
    if (!metrics_address.empty()) db.commit_observer = observe_commit;
    Committer committer(db);
    Maintenance maintenance{db, backup_dir, archive_dir, std::filesystem::u8path(db_path).stem().u8string(), backup_every, archive_every, backup_keep};
    maintenance.start();
    Config cfg = limits; cfg.users = users; cfg.options = &options; cfg.tls = tls_ctx; cfg.backup_dir = backup_dir;
    {
      std::lock_guard<std::mutex> g(listeners_m);
      for (const auto& t : tcp) { int port = 0; listeners.push_back(net::listen_tcp(t, port)); listener_tls.push_back(tls_ctx != nullptr); std::cout << "KESTRODB READY " << (tls_ctx ? "tls " : "tcp ") << t.substr(0, t.rfind(':')) << ":" << port << std::endl; }
#ifndef _WIN32
      for (const auto& u : unix_paths) { listeners.push_back(net::listen_unix(u, socket_mode)); listener_tls.push_back(false); std::cout << "KESTRODB READY unix " << u << std::endl; }
#endif
    }
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console, TRUE);
#else
    std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);
#endif
    static bool stdin_watched = false;  // once per process (the server may run again after a rejoin)
    if (stdin_stop && !stdin_watched) { stdin_watched = true; std::thread([] { char c; while (std::cin.get(c)) {} g_rejoin.requested = false; stop_server(); }).detach(); }
    if (cluster) cluster->start();
    // One accept thread per listener; one thread per connection.
    std::atomic<int> clients{0}; std::mutex cm; std::condition_variable ccv; std::set<net::Socket> open; std::map<std::string, int> per_ip;
    // What the metrics endpoint reads, until the server stops (before anything it names closes).
    { std::lock_guard<std::mutex> g(g_obs.m); g_obs.db = &db; g_obs.cfg = &cfg; g_obs.clients = &clients; g_obs.replica = &replicator; g_obs.cluster = cluster.get(); }
    struct Unobserve { ~Unobserve() { std::lock_guard<std::mutex> g(g_obs.m); g_obs.db = nullptr; g_obs.cfg = nullptr; g_obs.clients = nullptr; g_obs.replica = nullptr; g_obs.cluster = nullptr; g_obs.stopping = true; } } unobserve;
    std::vector<std::thread> acceptors;
    for (size_t li = 0; li < listeners.size(); li++) acceptors.emplace_back([&, l = listeners[li], tls = bool(listener_tls[li])] {
      while (!stopping) {
        net::Socket c = net::accept_one(l);
        if (c == net::invalid) { if (stopping) return; std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        std::unique_lock<std::mutex> g(cm);
        auto refused = [&](bool crowded) { (stopping ? g_counters.refused_stopping : crowded ? g_counters.refused_per_ip : g_counters.refused_limit).fetch_add(1, std::memory_order_relaxed); };
        if ((stopping || clients >= max_clients) && tls) { refused(false); g.unlock(); net::close(c); continue; }  // no plain-text reply on a TLS port
        std::string peer = net::peer_address(c);
        // One address may hold at most --max-connections-per-ip (loopback clients: no limit).
        bool crowded = !peer.empty() && !net::loopback_address(peer) && per_ip[peer] >= max_per_ip;
        if ((stopping || clients >= max_clients || crowded) && tls) { refused(crowded); g.unlock(); net::close(c); continue; }  // no plain-text reply on a TLS port
        if (stopping || clients >= max_clients || crowded) { refused(crowded); g.unlock(); g_rejecter.refuse(c, crowded ? "BUSY too many connections from this address" : "BUSY too many connections"); continue; }
        clients++; g_counters.accepted.fetch_add(1, std::memory_order_relaxed); open.insert(c); if (!peer.empty()) per_ip[peer]++; g.unlock();
        auto done = [&, c, peer] { std::lock_guard<std::mutex> d(cm); if (open.erase(c)) net::close(c); clients--; if (!peer.empty() && --per_ip[peer] <= 0) per_ip.erase(peer); ccv.notify_all(); };
        try {
          std::thread([&, c, peer, done] { try { serve(c, tls, db, committer, cfg, clients, peer); } catch (...) {} done(); }).detach();
        } catch (const std::exception&) { done(); }  // no thread (memory or thread limit): this client is dropped, the server goes on
      }
    });
    for (auto& a : acceptors) a.join();
    { std::lock_guard<std::mutex> g(g_obs.m); g_obs.stopping = true; }
    g_stopping_sync = true; g_sync.cv.notify_all();
    // Stopping: end every connection, wait for its thread, then close the store (which
    // writes its index snapshot).
    // On Windows a shutdown does not wake a thread blocked reading a client that keeps its end
    // open (a stop waited for such a client for minutes): after two seconds the sockets still
    // open are closed, which does.
    { std::unique_lock<std::mutex> g(cm); for (auto c : open) net::shutdown_both(c);
      if (!ccv.wait_for(g, std::chrono::seconds(2), [&] { return clients == 0; })) { for (auto c : open) net::close(c); open.clear(); }
      ccv.wait(g, [&] { return clients == 0; }); }
#ifndef _WIN32
    for (const auto& u : unix_paths) ::unlink(u.c_str());
#endif
    replicator.finish(); g_replica_json = nullptr;
    if (cluster) cluster->finish();
    g_cluster_json = nullptr; g_cluster_handle = nullptr;
    maintenance.finish();
    g_slow.drain();
    if (g_rejoin.requested) { std::cout << "KESTRODB CLUSTER restarting to rejoin" << std::endl; return RESTART_REJOIN; }
    std::cout << "KESTRODB STOPPED" << std::endl;
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
// Runs the server; a cluster member that must rejoin (its log does not continue the
// leader's) has its file cut back, or moved aside to be copied anew, and runs again.
int run_server(const std::vector<std::string>& args) {
  for (;;) {
    int r = server_main(args);
    if (r != RESTART_REJOIN) return r;
    std::string path; for (size_t i = 1; i + 1 < args.size(); i++) if (args[i] == "--db") path = args[i + 1];
    try {
      bool cut = !g_rejoin.reseed && Store::truncate_log(path, g_rejoin.txid);
      if (cut) std::cout << "KESTRODB CLUSTER log cut back to transaction " << g_rejoin.txid << std::endl;
      else {
        auto aside = path + ".diverged-" + utc_stamp();
        std::filesystem::rename(std::filesystem::u8path(path), std::filesystem::u8path(aside));
        for (auto ext : {".idx", ".sets", ".sort", ".sorting"}) platform::remove_noexcept(path + ext);
        g_force_seed = true;
        std::cout << "KESTRODB CLUSTER moved " << path << " to " << aside << "; copying the leader's log" << std::endl;
      }
    } catch (const std::exception& e) { std::cerr << "KESTRODB CLUSTER rejoin failed: " << e.what() << std::endl; return 1; }
  }
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { try { std::vector<std::string> args; for (int i = 0; i < argc; i++) args.push_back(platform::utf8(argv[i])); return args.size() > 1 && args[1] == "admin" ? admin_main(args) : run_server(args); } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; } }
#else
int main(int argc, char** argv) { std::vector<std::string> args(argv, argv + argc); return args.size() > 1 && args[1] == "admin" ? admin_main(args) : run_server(args); }
#endif

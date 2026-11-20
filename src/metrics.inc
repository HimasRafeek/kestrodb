// Included by server.cpp: what the server counts for --metrics, the admin requests and the
// slow-query log. The request path only adds to counters of its own connection (one writer
// each: a plain relaxed load and store, no locked instruction, no shared cache line) and,
// with --metrics or --slow-query-ms, reads the clock once per request. A connection's counts
// join the totals when it closes; a scrape adds the live connections' to them.
#ifndef KESTRO_VERSION
#define KESTRO_VERSION "0.9.0"
#endif
using Clock = std::chrono::steady_clock;
// A counter with one writer.
inline void bump(std::atomic<uint64_t>& a, uint64_t n = 1) { a.store(a.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }
inline uint64_t val(const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); }
int64_t unix_ms() { return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }
const int64_t g_started_ms = unix_ms(); const Clock::time_point g_started = Clock::now();
std::atomic<bool> g_timing{false};  // --metrics or --slow-query-ms: requests are timed

// Durations: buckets from 100 µs to 10 s (the le= labels), in nanoseconds.
struct Histogram {
  static constexpr int N = 16;
  static constexpr uint64_t bound[N] = {100000, 250000, 500000, 1000000, 2500000, 5000000, 10000000, 25000000, 50000000, 100000000, 250000000, 500000000, 1000000000, 2500000000ull, 5000000000ull, 10000000000ull};
  static constexpr const char* label[N] = {"0.0001", "0.00025", "0.0005", "0.001", "0.0025", "0.005", "0.01", "0.025", "0.05", "0.1", "0.25", "0.5", "1", "2.5", "5", "10"};
  std::atomic<uint64_t> bucket[N + 1]{}, sum_ns{0};  // bucket[N]: above 10 s
  void add(uint64_t ns) { int i = 0; while (i < N && ns > bound[i]) i++; bump(bucket[i]); bump(sum_ns, ns); }  // one writer at a time
  void add_to(Histogram& o) const { for (int i = 0; i <= N; i++) o.bucket[i].fetch_add(val(bucket[i]), std::memory_order_relaxed); o.sum_ns.fetch_add(val(sum_ns), std::memory_order_relaxed); }
};
enum Req { RqAuth, RqGet, RqGetMany, RqBatch, RqSql, RqStats, RqVerify, RqBackup, RqBackupCompacted, RqCompact, RqReplicate, RqPromote, RqCluster, RqQuit, RqConnections, RqKill, RqSlow, RqMetrics, RqOther, RqCount };
const char* const req_names[RqCount] = {"auth", "get", "get_many", "batch", "sql", "stats", "verify", "backup", "backup_compacted", "compact", "replicate", "promote", "cluster", "quit", "connections", "kill", "slow_queries", "metrics", "other"};
inline int req_of(char c) {
  switch (c) {
    case 'A': return RqAuth; case 'G': return RqGet; case 'M': return RqGetMany; case 'B': return RqBatch; case 'E': return RqSql; case 'S': return RqStats; case 'V': return RqVerify;
    case 'P': return RqBackup; case 'C': return RqBackupCompacted; case 'K': return RqCompact; case 'R': return RqReplicate; case 'X': return RqPromote; case 'Z': return RqCluster; case 'Q': return RqQuit;
    case 'L': return RqConnections; case 'D': return RqKill; case 'O': return RqSlow; case 'T': return RqMetrics; default: return RqOther;
  }
}
enum SqlKindLabel { SkSelect, SkInsert, SkUpdate, SkDelete, SkDdl, SkOther, SkCount };
const char* const sql_kind_names[SkCount] = {"select", "insert", "update", "delete", "ddl", "other"};
// The statement's first keyword (a few bytes: the request path).
inline int sql_kind_of(std::string_view t) {
  size_t i = 0; while (i < t.size() && (std::isspace(uint8_t(t[i])) || t[i] == '(')) i++;
  char w[9] = {}; size_t k = 0; while (i < t.size() && k < 8 && std::isalpha(uint8_t(t[i]))) w[k++] = char(std::toupper(uint8_t(t[i++])));
  std::string_view s(w, k);
  if (s == "SELECT" || s == "WITH" || s == "VALUES" || s == "EXPLAIN") return SkSelect;
  if (s == "INSERT" || s == "REPLACE" || s == "UPSERT") return SkInsert;
  if (s == "UPDATE") return SkUpdate;
  if (s == "DELETE") return SkDelete;
  if (s == "CREATE" || s == "DROP" || s == "ALTER" || s == "TRUNCATE") return SkDdl;
  return SkOther;
}
// The code of an error message ("CONFLICT unique ..." -> CONFLICT).
inline std::string error_code(std::string_view m) {
  size_t i = 0; while (i < m.size() && i < 24 && ((m[i] >= 'A' && m[i] <= 'Z') || m[i] == '_')) i++;
  return i >= 2 && (i == m.size() || m[i] == ' ' || m[i] == ':') ? std::string(m.substr(0, i)) : std::string("OTHER");
}
// Counts by error code (the error path only: a lock is fine). At most 64 codes; more are OTHER.
struct ErrorCounts {
  std::mutex m; std::map<std::string, uint64_t> by;
  void add(const std::string& code) { std::lock_guard<std::mutex> g(m); auto it = by.find(code); if (it != by.end()) it->second++; else by[by.size() < 64 ? code : "OTHER"]++; }
  std::map<std::string, uint64_t> copy() { std::lock_guard<std::mutex> g(m); return by; }
};
ErrorCounts g_request_errors, g_sql_errors;
// Counted outside any connection (accepting, logging in, maintenance).
struct ServerCounters {
  std::atomic<uint64_t> accepted{0}, refused_limit{0}, refused_per_ip{0}, refused_stopping{0}, killed{0};
  std::atomic<uint64_t> auth_token{0}, auth_cert{0}, auth_rate{0}, auth_missing{0};
  std::atomic<uint64_t> compactions{0}, compaction_failures{0}, backups{0}, backup_failures{0};
  std::atomic<int64_t> last_backup_ms{0}, last_compaction_ms{0};
  Histogram commit;  // written under the store's writer lock (one writer at a time)
} g_counters;
void observe_commit(uint64_t ns) { g_counters.commit.add(ns); }
void backup_done(bool ok) { (ok ? g_counters.backups : g_counters.backup_failures).fetch_add(1, std::memory_order_relaxed); if (ok) g_counters.last_backup_ms = unix_ms(); }

// One connection's counts (written by its own thread; notes by its LISTEN sender).
struct ConnStats {
  std::atomic<uint64_t> req[RqCount]{}, sql[SkCount]{}, bytes_in{0}, bytes_out{0}, notes{0}, note_bytes{0}, slow{0};
  Histogram statement;
  void add_to(ConnStats& o) const {
    for (int i = 0; i < RqCount; i++) o.req[i].fetch_add(val(req[i]), std::memory_order_relaxed);
    for (int i = 0; i < SkCount; i++) o.sql[i].fetch_add(val(sql[i]), std::memory_order_relaxed);
    auto add = [](const std::atomic<uint64_t>& a, std::atomic<uint64_t>& b) { b.fetch_add(val(a), std::memory_order_relaxed); };
    add(bytes_in, o.bytes_in); add(bytes_out, o.bytes_out); add(notes, o.notes); add(note_bytes, o.note_bytes); add(slow, o.slow);
    statement.add_to(o.statement);
  }
};
// A live connection, as the admin requests (connections, kill) and the metrics see it.
struct ConnInfo {
  uint64_t id = 0; std::string peer, user, role; bool tls = false; int64_t since_ms = 0; net::Socket sock = net::invalid;  // user/role: set under the registry's lock
  std::atomic<bool> in_tx{false}, listening{false}, replicating{false}, killed{false};
  ConnStats st;
};
struct Registry {
  std::mutex m; std::map<uint64_t, ConnInfo*> live; uint64_t next = 0; ConnStats closed;
  void add(ConnInfo& c) { std::lock_guard<std::mutex> g(m); c.id = ++next; c.since_ms = unix_ms(); live[c.id] = &c; }
  void remove(ConnInfo& c) { std::lock_guard<std::mutex> g(m); if (live.erase(c.id)) c.st.add_to(closed); }
  void set_user(ConnInfo& c, const std::string& name, const std::string& role) { std::lock_guard<std::mutex> g(m); c.user = name; c.role = role; }
  // Ends a connection: its thread sees the socket fail and cleans up (rolls back, unlistens).
  bool kill(uint64_t id) {
    std::lock_guard<std::mutex> g(m); auto it = live.find(id); if (it == live.end()) return false;
    it->second->killed = true; net::shutdown_both(it->second->sock);
#ifdef _WIN32
    CancelIoEx(reinterpret_cast<HANDLE>(it->second->sock), nullptr);  // a shutdown alone does not wake a blocked receive on Windows
#endif
    g_counters.killed.fetch_add(1, std::memory_order_relaxed); return true;
  }
} g_registry;

inline void json_escape(std::string& out, std::string_view s) {
  static const char* hex = "0123456789abcdef";
  for (char ch : s) {
    auto c = uint8_t(ch);
    if (c == '"' || c == '\\') { out += '\\'; out += ch; }
    else if (c == '\n') out += "\\n"; else if (c == '\r') out += "\\r"; else if (c == '\t') out += "\\t";
    else if (c < 32 || c == 127) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else out += ch;
  }
}
inline std::string json_str(std::string_view s) { std::string o = "\""; json_escape(o, s); o += '"'; return o; }
// At most n bytes, not cutting a UTF-8 sequence.
inline std::string_view clip(std::string_view s, size_t n) { if (s.size() <= n) return s; while (n > 0 && (uint8_t(s[n]) & 0xC0) == 0x80) n--; return s.substr(0, n); }
std::string iso_time(int64_t ms) {
  std::time_t t = std::time_t(ms / 1000); std::tm g{};
#ifdef _WIN32
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  char b[40]; std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &g);
  char f[48]; std::snprintf(f, sizeof f, "%s.%03dZ", b, int(ms % 1000)); return f;
}
// The statement with its literals ('text', numbers) replaced by ?: values never reach the log
// unless --slow-query-params.
std::string redact_literals(std::string_view t) {
  std::string o; o.reserve(t.size());
  auto ident = [](char c) { return std::isalnum(uint8_t(c)) || c == '_' || uint8_t(c) >= 0x80; };
  for (size_t i = 0; i < t.size();) {
    char c = t[i];
    if (c == '\'') { i++; while (i < t.size()) { if (t[i] == '\'') { if (i + 1 < t.size() && t[i + 1] == '\'') { i += 2; continue; } i++; break; } i++; } o += '?'; continue; }
    if (c == '"' || c == '`') { size_t j = t.find(c, i + 1); j = j == std::string_view::npos ? t.size() : j + 1; o.append(t.substr(i, j - i)); i = j; continue; }
    if (c == '-' && i + 1 < t.size() && t[i + 1] == '-') { size_t j = t.find('\n', i); j = j == std::string_view::npos ? t.size() : j; o.append(t.substr(i, j - i)); i = j; continue; }
    if (std::isdigit(uint8_t(c)) && (o.empty() || !ident(o.back()))) { while (i < t.size() && (std::isalnum(uint8_t(t[i])) || t[i] == '.')) i++; o += '?'; continue; }
    if (ident(c)) { while (i < t.size() && ident(t[i])) o += t[i++]; continue; }
    o += c; i++;
  }
  return o;
}
// The rows a SQL result returned, or the rows it changed (only for a slow statement).
uint64_t result_rows(std::string_view r) {
  const char* p = r.data(); const char* e = p + r.size();
  if (r.rfind("{\"rows\":[", 0) == 0) {
    p += 9; json::skip_ws(p, e); if (p < e && *p == ']') return 0;
    uint64_t n = 0; while (p < e) { if (!json::skip_value(p, e)) break; n++; json::skip_ws(p, e); if (p < e && *p == ',') { p++; continue; } break; }
    return n;
  }
  if (r.rfind("{\"changes\":", 0) == 0) return std::strtoull(p + 11, nullptr, 10);
  return 0;
}

// --slow-query-ms: statements slower than it are written as JSON lines to --slow-query-log
// (standard error by default) by a background thread. The request path only queues a line: a
// full queue (a slow disk) drops it, counted. The newest 128 stay in memory (admin slow).
struct SlowLog {
  uint64_t threshold_ns = 0; bool params = false; std::string path; uint64_t max_bytes = uint64_t(64) << 20;
  std::mutex m; std::condition_variable cv, idle; std::deque<std::string> q; size_t q_bytes = 0; bool writing = false, started = false;
  std::deque<std::string> ring;
  std::atomic<uint64_t> written{0}, dropped{0}, rotations{0};
  void submit(std::string line) {
    std::lock_guard<std::mutex> g(m);
    ring.push_back(line); if (ring.size() > 128) ring.pop_front();
    if (q.size() >= 4096 || q_bytes + line.size() > (size_t(8) << 20)) { dropped.fetch_add(1, std::memory_order_relaxed); return; }
    q_bytes += line.size(); q.push_back(std::move(line));
    if (!started) { started = true; std::thread([this] { run(); }).detach(); }  // never joined: the process ends with it
    cv.notify_one();
  }
  std::vector<std::string> recent(size_t n) { std::lock_guard<std::mutex> g(m); n = std::min(n, ring.size()); return std::vector<std::string>(ring.end() - std::ptrdiff_t(n), ring.end()); }
  // Waits (a moment at most) until the queued lines are written: a stopping server.
  void drain() { std::unique_lock<std::mutex> g(m); idle.wait_for(g, std::chrono::seconds(2), [&] { return q.empty() && !writing; }); }
  void run() {
    std::FILE* f = nullptr; uint64_t size = 0;
    auto open = [&] {
      if (path.empty()) return;
#ifdef _WIN32
      f = _wfopen(platform::path(path).c_str(), L"ab");
#else
      f = std::fopen(path.c_str(), "ab");
#endif
      if (!f) { std::cerr << "KESTRODB SLOW LOG cannot open " << path << std::endl; return; }
      std::error_code ec; size = std::filesystem::file_size(std::filesystem::u8path(path), ec); if (ec) size = 0;
    };
    open();
    std::unique_lock<std::mutex> g(m);
    for (;;) {
      cv.wait(g, [&] { return !q.empty(); });
      std::deque<std::string> batch; batch.swap(q); q_bytes = 0; writing = true; g.unlock();
      for (auto& line : batch) {
        std::FILE* out = path.empty() ? stderr : f;
        if (out) { std::fwrite(line.data(), 1, line.size(), out); size += line.size(); }
        written.fetch_add(1, std::memory_order_relaxed);
        // Past --slow-query-log-max: the file becomes FILE.1 (replacing the one before).
        if (f && max_bytes && size >= max_bytes) {
          std::fclose(f); f = nullptr; std::error_code ec;
          std::filesystem::rename(std::filesystem::u8path(path), std::filesystem::u8path(path + ".1"), ec);
          rotations.fetch_add(1, std::memory_order_relaxed); open();
        }
      }
      std::fflush(path.empty() ? stderr : f);
      g.lock(); writing = false; idle.notify_all();
    }
  }
};
// Never destroyed: its writer waits on cv until the process ends (destroying a condition variable
// with a waiter blocks forever on Linux).
SlowLog& g_slow = *new SlowLog;
// A slow statement's line: {time, ms, user, peer, kind, rows, error?, sql, params?}.
void log_slow(const ConnInfo& ci, uint64_t ns, int kind, std::string_view sql, std::string_view params, std::string_view result, const std::string& error) {
  std::string line = "{\"time\":\"" + iso_time(unix_ms()) + "\",\"ms\":";
  char ms[32]; std::snprintf(ms, sizeof ms, "%.3f", double(ns) / 1e6); line += ms;
  std::string user; { std::lock_guard<std::mutex> g(g_registry.m); user = ci.user; }
  line += ",\"user\":" + (user.empty() ? std::string("null") : json_str(user)) + ",\"peer\":" + json_str(ci.peer.empty() ? "unix" : ci.peer) + ",\"connection\":" + std::to_string(ci.id) +
    ",\"kind\":\"" + sql_kind_names[kind] + "\",\"rows\":" + std::to_string(error.empty() ? result_rows(result) : 0);
  if (!error.empty()) line += ",\"error\":" + json_str(clip(error, 512));
  std::string text = g_slow.params ? std::string(sql) : redact_literals(sql);
  line += ",\"sql\":" + json_str(clip(text, 4096));
  if (text.size() > 4096) line += ",\"truncated\":true";
  if (g_slow.params) line += ",\"params\":" + json_str(clip(params, 4096));
  line += "}\n";
  g_slow.submit(std::move(line));
}
// Admin request L: the live connections.
std::string connections_json(uint64_t self) {
  std::lock_guard<std::mutex> g(g_registry.m); std::string o = "{\"connections\":[";
  for (const auto& [id, c] : g_registry.live) {
    if (o.back() != '[') o += ',';
    uint64_t requests = 0; for (const auto& r : c->st.req) requests += val(r);
    o += "{\"id\":" + std::to_string(id) + ",\"user\":" + (c->user.empty() ? std::string("null") : json_str(c->user)) + ",\"role\":" + (c->role.empty() ? std::string("null") : json_str(c->role)) +
      ",\"peer\":" + json_str(c->peer.empty() ? "unix" : c->peer) + ",\"tls\":" + (c->tls ? "true" : "false") + ",\"since\":\"" + iso_time(c->since_ms) + "\",\"sinceMs\":" + std::to_string(c->since_ms) +
      ",\"inTransaction\":" + (c->in_tx ? "true" : "false") + ",\"listening\":" + (c->listening ? "true" : "false") + ",\"replicating\":" + (c->replicating ? "true" : "false") +
      ",\"requests\":" + std::to_string(requests) + ",\"bytesIn\":" + std::to_string(val(c->st.bytes_in)) + ",\"bytesOut\":" + std::to_string(val(c->st.bytes_out)) + ",\"self\":" + (id == self ? "true" : "false") + "}";
  }
  return o + "]}";
}
// Admin request O: the newest slow statements, oldest first.
std::string slow_json(size_t n) {
  std::string o = "{\"thresholdMs\":" + std::to_string(g_slow.threshold_ns / 1000000) + ",\"logged\":" + std::to_string(g_slow.written.load()) + ",\"dropped\":" + std::to_string(g_slow.dropped.load()) + ",\"entries\":[";
  bool first = true; for (auto& l : g_slow.recent(n)) { if (!first) o += ','; first = false; o.append(l, 0, l.size() - 1); }  // each line ends with \n
  return o + "]}";
}

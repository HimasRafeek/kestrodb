// Included by server.cpp: --metrics HOST:PORT, a small HTTP/1.1 endpoint beside the database
// port for Prometheus and health checks:
//   GET /metrics        the metrics (text exposition format 0.0.4)
//   GET /health/live    200 while the process runs
//   GET /health/ready   200 when the database is open and healthy, synchronous replication is
//                       not degraded, a cluster has a leader, and a follower is within
//                       --ready-max-lag of its leader; 503 otherwise (the reason in the JSON)
// With --metrics-token-file every request needs "Authorization: Bearer TOKEN". A request is at
// most 8 KB, read within 5 s; one per connection (Connection: close); four at a time.

// What the server serves now (set while it runs; a scrape takes the lock).
struct Observed {
  std::mutex m; Store* db = nullptr; const Config* cfg = nullptr; std::atomic<int>* clients = nullptr;
  Replicator* replica = nullptr; Cluster* cluster = nullptr; std::string path; bool stopping = false;
  uint64_t ready_lag_ms = 10000;  // --ready-max-lag
};
Observed& g_obs = *new Observed;  // never destroyed: an endpoint thread may still read it as the process ends

struct Prom {
  std::string o;
  void family(const char* name, const char* type, const char* help) { o += "# HELP "; o += name; o += ' '; o += help; o += "\n# TYPE "; o += name; o += ' '; o += type; o += '\n'; }
  static std::string label(std::initializer_list<std::pair<const char*, std::string_view>> ls) {
    std::string s = "{";
    for (const auto& [k, v] : ls) {
      if (s.size() > 1) s += ',';
      s += k; s += "=\"";
      for (char c : v) { if (c == '\\' || c == '"') { s += '\\'; s += c; } else if (c == '\n') s += "\\n"; else s += c; }
      s += '"';
    }
    return s + "}";
  }
  void sample(const char* name, const std::string& labels, double v) {
    o += name; o += labels; o += ' ';
    char b[40]; if (v == double(int64_t(v)) && std::fabs(v) < 9e15) std::snprintf(b, sizeof b, "%lld", (long long)v); else std::snprintf(b, sizeof b, "%.15g", v);
    o += b; o += '\n';
  }
  void one(const char* name, const char* type, const char* help, double v) { family(name, type, help); sample(name, "", v); }
  void histogram(const char* name, const char* help, const Histogram& h) {
    family(name, "histogram", help); uint64_t c = 0; std::string b = std::string(name) + "_bucket";
    for (int i = 0; i < Histogram::N; i++) { c += val(h.bucket[i]); sample(b.c_str(), label({{"le", Histogram::label[i]}}), double(c)); }
    c += val(h.bucket[Histogram::N]); sample(b.c_str(), label({{"le", "+Inf"}}), double(c));
    sample((std::string(name) + "_sum").c_str(), "", double(val(h.sum_ns)) / 1e9); sample((std::string(name) + "_count").c_str(), "", double(c));
  }
};

// Ready? (g_obs.m held) {ready, reason or role}.
std::pair<bool, std::string> readiness() {
  if (!g_obs.db) return {false, g_obs.stopping ? "stopping" : "opening the database"};
  if (stopping) return {false, "stopping"};
  try { g_obs.db->txid(); } catch (const std::exception& e) { return {false, std::string("database failed: ") + e.what()}; }
  const Config& cfg = *g_obs.cfg; double bound = double(g_obs.ready_lag_ms) / 1000;
  if (cfg.sync_replicas > 0 && !cfg.cluster) { std::lock_guard<std::mutex> l(g_sync.m); if (g_sync.degraded) return {false, "synchronous replication is degraded (no follower confirmed within --sync-timeout)"}; }
  if (auto* c = g_obs.cluster) {
    std::lock_guard<std::mutex> l(c->m);
    if (c->role == Cluster::Leader) return {true, "leader"};
    if (c->leader.empty()) return {false, "the cluster has no leader"};
    double behind = std::chrono::duration<double>(Clock::now() - c->caught_up).count();
    if (behind > bound) return {false, "behind the cluster's leader for " + std::to_string(int64_t(behind)) + " s"};
    return {true, "follower"};
  }
  if (g_obs.replica && !g_obs.replica->leader.empty() && g_readonly) {
    auto l = g_obs.replica->lag();
    if (!l.connected) return {false, "not connected to the leader"};
    if (l.seconds > bound) return {false, "behind the leader for " + std::to_string(int64_t(l.seconds)) + " s (" + std::to_string(l.transactions) + " transactions)"};
    return {true, "follower"};
  }
  return {true, "leader"};
}

std::string render_metrics() {
  std::lock_guard<std::mutex> lock(g_obs.m);
  Prom p; auto L = Prom::label;
  p.family("kestrodb_build_info", "gauge", "Build of this kestro-server (always 1).");
#if defined(KESTRO_TLS)
  const char* tls_lib = "openssl";
#elif defined(KESTRO_WINTLS)
  const char* tls_lib = "schannel";
#else
  const char* tls_lib = "none";
#endif
  p.sample("kestrodb_build_info", L({{"version", KESTRO_VERSION}, {"platform", platform::name}, {"tls", tls_lib}}), 1);
  p.one("kestrodb_start_time_seconds", "gauge", "When the process started (Unix time).", double(g_started_ms) / 1000);
  p.one("kestrodb_uptime_seconds", "gauge", "Seconds since the process started.", std::chrono::duration<double>(Clock::now() - g_started).count());
  auto ready = readiness();
  p.one("kestrodb_ready", "gauge", "1 when /health/ready answers 200.", ready.first ? 1 : 0);
  // Connections and requests: the closed connections' totals plus the live ones'.
  ConnStats all; size_t live = 0, listening = 0, replicating = 0, in_tx = 0;
  { std::lock_guard<std::mutex> g(g_registry.m); g_registry.closed.add_to(all);
    for (const auto& [id, c] : g_registry.live) { c->st.add_to(all); live++; listening += c->listening; replicating += c->replicating; in_tx += c->in_tx; } }
  p.one("kestrodb_connections", "gauge", "Client connections open now.", double(live));
  p.one("kestrodb_connections_in_transaction", "gauge", "Connections with an open SQL transaction.", double(in_tx));
  p.one("kestrodb_connections_total", "counter", "Client connections accepted.", double(g_counters.accepted.load()));
  p.family("kestrodb_connections_refused_total", "counter", "Client connections refused, by reason.");
  p.sample("kestrodb_connections_refused_total", L({{"reason", "max_connections"}}), double(g_counters.refused_limit.load()));
  p.sample("kestrodb_connections_refused_total", L({{"reason", "per_ip"}}), double(g_counters.refused_per_ip.load()));
  p.sample("kestrodb_connections_refused_total", L({{"reason", "stopping"}}), double(g_counters.refused_stopping.load()));
  p.one("kestrodb_connections_killed_total", "counter", "Connections closed by an administrator (kill).", double(g_counters.killed.load()));
  p.family("kestrodb_auth_failures_total", "counter", "Failed logins and requests refused before login, by reason.");
  p.sample("kestrodb_auth_failures_total", L({{"reason", "invalid_token"}}), double(g_counters.auth_token.load()));
  p.sample("kestrodb_auth_failures_total", L({{"reason", "certificate"}}), double(g_counters.auth_cert.load()));
  p.sample("kestrodb_auth_failures_total", L({{"reason", "rate_limited"}}), double(g_counters.auth_rate.load()));
  p.sample("kestrodb_auth_failures_total", L({{"reason", "not_logged_in"}}), double(g_counters.auth_missing.load()));
  p.family("kestrodb_requests_total", "counter", "Requests served, by type (a batch counts each pipelined frame).");
  for (int i = 0; i < RqCount; i++) p.sample("kestrodb_requests_total", L({{"type", req_names[i]}}), double(val(all.req[i])));
  p.family("kestrodb_request_errors_total", "counter", "Requests answered with an error, by error code.");
  for (const auto& [code, n] : g_request_errors.copy()) p.sample("kestrodb_request_errors_total", L({{"code", code}}), double(n));
  p.family("kestrodb_sql_statements_total", "counter", "SQL statements, by kind (their first keyword).");
  for (int i = 0; i < SkCount; i++) p.sample("kestrodb_sql_statements_total", L({{"kind", sql_kind_names[i]}}), double(val(all.sql[i])));
  p.family("kestrodb_sql_errors_total", "counter", "SQL statements that failed, by error code.");
  for (const auto& [code, n] : g_sql_errors.copy()) p.sample("kestrodb_sql_errors_total", L({{"code", code}}), double(n));
  p.histogram("kestrodb_sql_statement_duration_seconds", "Time to execute a SQL statement and reply (with --metrics or --slow-query-ms).", all.statement);
  p.one("kestrodb_slow_queries_total", "counter", "SQL statements slower than --slow-query-ms.", double(val(all.slow)));
  p.one("kestrodb_slow_query_log_dropped_total", "counter", "Slow-query log lines dropped because the writer fell behind.", double(g_slow.dropped.load()));
  p.one("kestrodb_slow_query_log_rotations_total", "counter", "Times the slow-query log reached --slow-query-log-max and was rotated.", double(g_slow.rotations.load()));
  p.one("kestrodb_network_receive_bytes_total", "counter", "Request bytes read from client connections (after TLS).", double(val(all.bytes_in)));
  p.one("kestrodb_network_transmit_bytes_total", "counter", "Reply and notification bytes written to client connections (before TLS).", double(val(all.bytes_out) + val(all.note_bytes)));
  // LISTEN/NOTIFY.
  p.one("kestrodb_listeners", "gauge", "Connections listening for notifications.", double(listening));
  p.one("kestrodb_notifications_sent_total", "counter", "Notifications sent to listening connections.", double(val(all.notes)));
  p.one("kestrodb_listeners_dropped_total", "counter", "Listening connections closed for falling behind (--notify-queue).", double(g_listeners_dropped.load()));
  // The database.
  if (Store* db = g_obs.db) {
    try {
      p.one("kestrodb_database_bytes", "gauge", "Committed bytes of the database log.", double(db->bytes()));
      std::error_code ec; auto fs = std::filesystem::file_size(std::filesystem::u8path(g_obs.path), ec);
      if (!ec) p.one("kestrodb_database_file_size_bytes", "gauge", "Size of the database file (with its preallocated tail).", double(fs));
      p.one("kestrodb_keys", "gauge", "Live keys (SQL rows included).", double(db->count()));
      p.one("kestrodb_transaction_id", "gauge", "The last committed transaction ID.", double(db->txid()));
      p.one("kestrodb_commits_total", "counter", "Transactions committed (replicated ones included).", double(db->commits()));
      p.one("kestrodb_commit_groups_total", "counter", "Commit groups (one write and flush each).", double(db->commit_groups()));
      p.one("kestrodb_file_flushes_total", "counter", "File flushes made by commits (none with write-through).", double(db->flushes()));
      p.histogram("kestrodb_commit_duration_seconds", "Time to make a commit group durable and visible (with --metrics).", g_counters.commit);
      p.one("kestrodb_index_memory_bytes", "gauge", "Memory of the key index.", double(db->index_memory()));
      p.one("kestrodb_index_file_bytes", "gauge", "Bytes of the paged key index file.", double(db->index_file_bytes()));
      p.one("kestrodb_ordered_memory_bytes", "gauge", "Memory of the ordered (range) index.", double(db->ordered_memory()));
      p.one("kestrodb_ordered_file_bytes", "gauge", "Bytes of the ordered index file.", double(db->ordered_file_bytes()));
      p.one("kestrodb_snapshot_undo_memory_bytes", "gauge", "Undo memory kept for open MVCC snapshots.", double(db->snapshot_memory()));
      p.one("kestrodb_snapshot_yields_total", "counter", "Times a writer yielded to an open snapshot.", double(db->snapshot_yields()));
      p.one("kestrodb_database_failed", "gauge", "1 when the store failed (an I/O error) and refuses requests.", 0);
    } catch (const std::exception&) { p.one("kestrodb_database_failed", "gauge", "1 when the store failed (an I/O error) and refuses requests.", 1); }
  }
  p.one("kestrodb_compactions_total", "counter", "Online compactions completed.", double(g_counters.compactions.load()));
  p.one("kestrodb_compaction_failures_total", "counter", "Online compactions that failed.", double(g_counters.compaction_failures.load()));
  p.one("kestrodb_last_compaction_timestamp_seconds", "gauge", "When the last compaction completed (Unix time; 0: never).", double(g_counters.last_compaction_ms.load()) / 1000);
  p.one("kestrodb_backups_total", "counter", "Backups written (scheduled and requested).", double(g_counters.backups.load()));
  p.one("kestrodb_backup_failures_total", "counter", "Backups that failed.", double(g_counters.backup_failures.load()));
  p.one("kestrodb_last_backup_success_timestamp_seconds", "gauge", "When the last backup completed (Unix time; 0: never).", double(g_counters.last_backup_ms.load()) / 1000);
  // Replication.
  std::string role = g_readonly ? "follower" : "leader";
  if (g_obs.cluster) { std::lock_guard<std::mutex> l(g_obs.cluster->m); role = Cluster::role_name(g_obs.cluster->role); }
  p.family("kestrodb_role", "gauge", "This server's role (1 for the current one).");
  for (const char* r : {"leader", "follower", "candidate"}) p.sample("kestrodb_role", L({{"role", r}}), role == r ? 1 : 0);
  p.one("kestrodb_read_only", "gauge", "1 when the server refuses writes (a follower).", g_readonly ? 1 : 0);
  p.one("kestrodb_replication_followers", "gauge", "Connections replicating from this server (followers, cluster members).", double(replicating));
  uint64_t lag_tx = 0, lag_bytes = 0; double lag_s = 0; int connected = -1;
  if (g_obs.replica && !g_obs.replica->leader.empty() && g_readonly) { auto l = g_obs.replica->lag(); lag_tx = l.transactions; lag_bytes = l.bytes; lag_s = l.seconds; connected = l.connected; }
  else if (g_obs.cluster && g_obs.db) {
    uint64_t tx = g_obs.db->txid(), end = g_obs.db->bytes(); std::lock_guard<std::mutex> l(g_obs.cluster->m);
    if (g_obs.cluster->role != Cluster::Leader) {
      lag_tx = g_obs.cluster->seen_txid > tx ? g_obs.cluster->seen_txid - tx : 0; lag_bytes = g_obs.cluster->seen_end > end ? g_obs.cluster->seen_end - end : 0;
      lag_s = std::chrono::duration<double>(Clock::now() - g_obs.cluster->caught_up).count(); connected = !g_obs.cluster->leader.empty();
    }
  }
  p.one("kestrodb_replication_lag_transactions", "gauge", "A follower: transactions the leader has that it has not applied.", double(lag_tx));
  p.one("kestrodb_replication_lag_bytes", "gauge", "A follower: log bytes the leader has that it has not applied.", double(lag_bytes));
  p.one("kestrodb_replication_lag_seconds", "gauge", "A follower: seconds since it last had everything the leader had (about 1 s at most while streaming).", lag_s);
  if (connected >= 0) p.one("kestrodb_replication_connected", "gauge", "A follower: 1 while it streams from its leader.", connected);
  if (g_obs.cfg && g_obs.cfg->sync_replicas > 0) {
    std::lock_guard<std::mutex> l(g_sync.m);
    p.one("kestrodb_sync_replication_degraded", "gauge", "1 while synchronous replication goes on without its followers (--sync-timeout passed).", g_sync.degraded ? 1 : 0);
    p.one("kestrodb_sync_replication_waits_total", "counter", "Writes that waited for followers to confirm.", double(g_sync.waits));
    p.one("kestrodb_sync_replication_timeouts_total", "counter", "Waits for followers that timed out.", double(g_sync.timeouts));
  }
  if (auto* c = g_obs.cluster) {
    std::lock_guard<std::mutex> l(c->m);
    p.one("kestrodb_cluster_term", "gauge", "The cluster's current term at this member.", double(c->term));
    p.one("kestrodb_cluster_is_leader", "gauge", "1 when this member leads the cluster.", c->role == Cluster::Leader ? 1 : 0);
    p.one("kestrodb_cluster_has_leader", "gauge", "1 when this member knows a leader.", c->role == Cluster::Leader || !c->leader.empty() ? 1 : 0);
    p.one("kestrodb_cluster_members", "gauge", "Members of the cluster (this one included).", double(c->peers.size() + 1));
    p.one("kestrodb_cluster_elections_total", "counter", "Elections this member started.", double(c->elections));
  }
  return p.o;
}

struct MetricsServer {
  std::string token; std::atomic<int> busy{0}; bool running = false;
  static void respond(net::Socket c, int code, const char* reason, const char* type, const std::string& body, bool head, const char* extra = "") {
    std::string r = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
      "\r\nCache-Control: no-store\r\nConnection: close\r\n" + extra + "\r\n";
    if (!head) r += body;
    net::send_all(c, reinterpret_cast<const unsigned char*>(r.data()), r.size());
    // Half-close, then read what the client still sends: closing with unread bytes would
    // reset the connection, which can destroy the reply.
    net::shutdown_send(c); char b[512]; while (net::recv_some(c, b, sizeof b) > 0) {}
  }
  void serve(net::Socket c) {
    net::set_timeout(c, 2);
    std::string req; char buf[2048]; auto deadline = Clock::now() + std::chrono::seconds(5);
    for (;;) {
      auto e = req.find("\r\n\r\n");
      if (e != std::string::npos ? e > 8192 : req.size() > 8192) { respond(c, 431, "Request Header Fields Too Large", "text/plain", "request too large\n", false); return; }
      if (e != std::string::npos) break;
      if (Clock::now() > deadline) return;
      auto got = net::recv_some(c, buf, sizeof buf); if (got <= 0) return;
      req.append(buf, size_t(got));
    }
    auto line_end = req.find("\r\n"); std::string line = req.substr(0, line_end);
    auto sp1 = line.find(' '), sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 <= sp1) { respond(c, 400, "Bad Request", "text/plain", "bad request\n", false); return; }
    std::string method = line.substr(0, sp1), path = line.substr(sp1 + 1, sp2 - sp1 - 1); path = path.substr(0, path.find('?'));
    bool head = method == "HEAD";
    if (method != "GET" && !head) { respond(c, 405, "Method Not Allowed", "text/plain", "GET or HEAD\n", false, "Allow: GET, HEAD\r\n"); return; }
    if (!token.empty()) {
      std::string given;
      for (size_t p = line_end + 2; p < req.size();) {
        auto e = req.find("\r\n", p); if (e == std::string::npos || e == p) break;
        std::string h = req.substr(p, e - p); p = e + 2;
        auto colon = h.find(':'); if (colon == std::string::npos) continue;
        std::string name = h.substr(0, colon); for (auto& ch : name) ch = char(std::tolower(uint8_t(ch)));
        if (name != "authorization") continue;
        std::string v = h.substr(colon + 1); size_t i = 0; while (i < v.size() && v[i] == ' ') i++; v = v.substr(i);
        if (v.size() > 7 && (v.compare(0, 7, "Bearer ") == 0 || v.compare(0, 7, "bearer ") == 0)) { given = v.substr(7); while (!given.empty() && given.back() == ' ') given.pop_back(); }
      }
      if (given.empty() || !same_token(given, token)) { respond(c, 401, "Unauthorized", "text/plain", "a bearer token is required (--metrics-token-file)\n", head, "WWW-Authenticate: Bearer realm=\"kestrodb\"\r\n"); return; }
    }
    if (path == "/metrics") { respond(c, 200, "OK", "text/plain; version=0.0.4; charset=utf-8", render_metrics(), head); return; }
    if (path == "/health/live") { respond(c, 200, "OK", "application/json", "{\"status\":\"live\"}\n", head); return; }
    if (path == "/health/ready") {
      std::pair<bool, std::string> r; { std::lock_guard<std::mutex> l(g_obs.m); r = readiness(); }
      if (r.first) respond(c, 200, "OK", "application/json", "{\"status\":\"ready\",\"role\":" + json_str(r.second) + "}\n", head);
      else respond(c, 503, "Service Unavailable", "application/json", "{\"status\":\"unavailable\",\"reason\":" + json_str(r.second) + "}\n", head);
      return;
    }
    respond(c, 404, "Not Found", "text/plain", "not found: /metrics, /health/live, /health/ready\n", head);
  }
  // Once per process (a cluster member that rejoins runs the server again; the endpoint stays).
  void start(const std::string& address) {
    if (running) return;
    int port = 0; net::Socket l = net::listen_tcp(address, port); running = true;
    std::cout << "KESTRODB METRICS http://" << address.substr(0, address.rfind(':')) << ":" << port << std::endl;
    std::thread([this, l] {
      for (;;) {
        net::Socket c = net::accept_one(l);
        if (c == net::invalid) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        if (busy.load() >= 4) { net::set_timeout(c, 1); respond(c, 503, "Service Unavailable", "text/plain", "busy\n", false); net::close(c); continue; }
        busy++;
        try { std::thread([this, c] { try { serve(c); } catch (...) {} net::close(c); busy--; }).detach(); } catch (...) { net::close(c); busy--; }
      }
    }).detach();
  }
} g_metrics_server;

// Included by server.cpp: kestro-server admin, the operator's command line. It connects like
// a client (tcp://, unix://, tls://) and logs in with a token; the commands it sends are
// admin-only on a server with users (status and stats need any role).
// Exit codes: 0 done, 1 failed (the server's error on standard error), 2 usage.
const char* const admin_usage =
  "usage: kestro-server admin [--server tcp://HOST:PORT|unix://PATH|tls://HOST:PORT] [--token-file FILE] [--user NAME]\n"
  "                           [--ca PEM] [--cert PEM --key PEM] [--timeout 5m] [--metrics-url http://HOST:PORT [--metrics-token-file FILE]] COMMAND\n"
  "commands:\n"
  "  status               role, version, uptime, keys, size, connections, replication\n"
  "  stats [--json]       the server's statistics (JSON)\n"
  "  verify               check the whole database file\n"
  "  backup NAME          an online backup, written as NAME in the server's --backup-dir\n"
  "  compact              online compaction of the database file\n"
  "  metrics              the Prometheus metrics (from --metrics-url, or over the connection)\n"
  "  connections [--json] the live connections\n"
  "  kill ID              close connection ID (rolls back its transaction)\n"
  "  slow [N] [--json]    the newest N slow statements (20; at most 128)\n"
  "The token comes from --token-file or KESTRODB_TOKEN; the server from --server or KESTRODB_SERVER (tcp://127.0.0.1:7820).\n";

std::string read_token_file(const std::string& file) {
  FD f(platform::open_file(file)); auto bytes = read_at(f.n, size_t(platform::size(f.n)), 0);
  std::string t(bytes.begin(), bytes.end()); while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  require(!t.empty(), "INPUT token file " + file + " is empty"); return t;
}
// GET http://HOST:PORT/PATH (plain HTTP: the metrics endpoint) -> the body; throws unless 200.
std::string http_get(const std::string& url, const std::string& token, int seconds) {
  require(url.rfind("http://", 0) == 0, "INPUT --metrics-url must be http://HOST:PORT (for https, use a proxy's own client)");
  std::string rest = url.substr(7), hp = rest.substr(0, rest.find('/')), path = rest.size() > hp.size() ? rest.substr(hp.size()) : "/metrics";
  if (path == "/") path = "/metrics";
  net::Socket s = net::connect_tcp(hp, 5000); net::set_timeout(s, seconds);
  struct Close { net::Socket s; ~Close() { net::close(s); } } closer{s};
  std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + hp + "\r\nConnection: close\r\n" + (token.empty() ? "" : "Authorization: Bearer " + token + "\r\n") + "\r\n";
  require(net::send_all(s, reinterpret_cast<const unsigned char*>(req.data()), req.size()), "IO cannot send to " + hp);
  std::string r; char b[16384]; for (;;) { auto got = net::recv_some(s, b, sizeof b); if (got <= 0) break; r.append(b, size_t(got)); require(r.size() < (size_t(64) << 20), "LIMIT metrics reply"); }
  auto he = r.find("\r\n\r\n"); require(he != std::string::npos && r.rfind("HTTP/1.", 0) == 0, "IO not an HTTP reply from " + hp);
  int code = std::atoi(r.c_str() + 9); std::string body = r.substr(he + 4);
  if (code != 200) throw std::runtime_error((code == 401 ? "AUTH " : "IO ") + url + " answered " + r.substr(9, r.find("\r\n") - 9) + (body.empty() ? "" : ": " + body));
  return body;
}
std::string human_bytes(double b) { const char* u[] = {"B", "KB", "MB", "GB", "TB"}; int i = 0; while (b >= 1024 && i < 4) { b /= 1024; i++; } char s[32]; std::snprintf(s, sizeof s, i ? "%.1f %s" : "%.0f %s", b, u[i]); return s; }
std::string human_duration(int64_t ms) {
  int64_t s = ms / 1000, d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60; s %= 60; std::string o;
  if (d) o += std::to_string(d) + "d "; if (d || h) o += std::to_string(h) + "h "; if (d || h || m) o += std::to_string(m) + "m "; return o + std::to_string(s) + "s";
}
std::string jtext(const json::Json* j) { if (!j || j->kind == json::Json::Null) return "-"; if (j->is_string()) return j->str; if (j->kind == json::Json::True) return "yes"; if (j->kind == json::Json::False) return "no"; return j->raw; }
double jnum(const json::Json* j) { return j && j->is_number() ? j->num : 0; }

int admin_main(const std::vector<std::string>& args) {
  std::string server, token_file, user, ca, cert, key, metrics_url, metrics_token_file; int seconds = 300; std::vector<std::string> words; bool as_json = false;
  try {
    for (size_t i = 2; i < args.size(); i++) {
      const auto& a = args[i];
      auto value = [&]() -> std::string { if (i + 1 >= args.size()) throw std::invalid_argument(a + " needs a value"); return args[++i]; };
      if (a == "--server" || a == "-s") server = value();
      else if (a == "--token-file") token_file = value();
      else if (a == "--user") user = value();
      else if (a == "--ca") ca = value();
      else if (a == "--cert") cert = value();
      else if (a == "--key") key = value();
      else if (a == "--timeout") seconds = int(std::max<uint64_t>(1, parse_duration(value()) / 1000));
      else if (a == "--metrics-url") metrics_url = value();
      else if (a == "--metrics-token-file") metrics_token_file = value();
      else if (a == "--json") as_json = true;
      else if (a == "--help" || a == "-h") { std::cout << admin_usage; return 0; }
      else if (a.size() > 1 && a[0] == '-' && !std::isdigit(uint8_t(a[1]))) throw std::invalid_argument("unknown option " + a);
      else words.push_back(a);
    }
    if (words.empty()) throw std::invalid_argument("no command");
    const std::string& cmd = words[0];
    size_t want = cmd == "backup" || cmd == "kill" ? 2 : cmd == "slow" ? words.size() : 1;
    static const std::set<std::string> known = {"status", "stats", "verify", "backup", "compact", "metrics", "connections", "kill", "slow"};
    if (!known.count(cmd)) throw std::invalid_argument("unknown command " + cmd);
    if (words.size() != want || (cmd == "slow" && words.size() > 2)) throw std::invalid_argument(cmd + ": wrong number of arguments");
    if (cmd == "kill" && words[1].find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("kill ID: a connection id (admin connections)");
    if (cmd == "slow" && words.size() == 2 && words[1].find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("slow N: a number");
    if (cert.empty() != key.empty()) throw std::invalid_argument("--cert and --key go together");
  } catch (const std::exception& e) { std::cerr << "kestro-server admin: " << e.what() << "\n" << admin_usage; return 2; }
  try {
    net::init();
    const std::string& cmd = words[0];
    if (cmd == "metrics" && !metrics_url.empty()) { std::cout << http_get(metrics_url, metrics_token_file.empty() ? std::string() : read_token_file(metrics_token_file), seconds); return 0; }
    if (server.empty()) { const char* e = std::getenv("KESTRODB_SERVER"); server = e && *e ? e : "tcp://127.0.0.1:7820"; }
    std::string token;
    if (!token_file.empty()) token = read_token_file(token_file); else if (const char* t = std::getenv("KESTRODB_TOKEN")) token = t;
    if (!cert.empty()) {
      g_link_cert.cert = cert; g_link_cert.key = key;
#ifdef KESTRO_WINTLS
      g_link_cert.creds = wintls::load_pem(cert, key, false, "--cert", "--key");
#endif
    }
    LeaderLink link; link.who = "the server";
    link.open(server, token, ca, 5000, seconds);  // no token: an open server, or a user bound to the client certificate
    auto text = [](const Bytes& r) { return std::string(r.begin() + 1, r.end()); };
    if (!user.empty()) {  // --user: the token must be that user's
      auto st = json::parse(text(link.call('S', {}))); auto* u = st.get("user");
      if (!u || !u->is_string() || u->str != user) throw std::runtime_error("AUTH the token is not user " + user + "'s (it is " + (u && u->is_string() ? u->str : std::string("an open server's")) + ")");
    }
    if (cmd == "stats") { std::cout << text(link.call('S', {})) << std::endl; return 0; }
    if (cmd == "verify") { link.call('V', {}); std::cout << "verified: the database file is intact" << std::endl; return 0; }
    if (cmd == "backup") { link.call('P', Bytes(words[1].begin(), words[1].end())); std::cout << "backup written: " << words[1] << " (in the server's --backup-dir)" << std::endl; return 0; }
    if (cmd == "compact") {
      auto j = json::parse(text(link.call('K', {})));
      std::cout << "compacted: " << human_bytes(jnum(j.get("before"))) << " -> " << human_bytes(jnum(j.get("after"))) << ", " << uint64_t(jnum(j.get("keys"))) << " keys" << std::endl; return 0;
    }
    if (cmd == "metrics") { std::cout << text(link.call('T', {})); return 0; }
    if (cmd == "kill") { Bytes id(8); put64(id.data(), std::stoull(words[1])); link.call('D', id); std::cout << "closed connection " << words[1] << std::endl; return 0; }
    if (cmd == "slow") {
      Bytes n(4); put32(n.data(), uint32_t(words.size() == 2 ? std::min<uint64_t>(std::stoull(words[1]), 128) : 20));
      auto r = text(link.call('O', n)); if (as_json) { std::cout << r << std::endl; return 0; }
      auto j = json::parse(r); auto* es = j.get("entries");
      if (jnum(j.get("thresholdMs")) == 0) std::cerr << "note: the server's slow-query log is off (--slow-query-ms)" << std::endl;
      if (es) for (const auto& e : es->items) std::cout << e.raw << "\n";
      std::cout << std::flush; return 0;
    }
    if (cmd == "connections") {
      auto r = text(link.call('L', {})); if (as_json) { std::cout << r << std::endl; return 0; }
      auto j = json::parse(r); auto* cs = j.get("connections");
      std::printf("%-6s %-16s %-10s %-40s %-24s %-5s %-6s %s\n", "ID", "USER", "ROLE", "PEER", "SINCE", "TX", "LISTEN", "REQUESTS");
      if (cs) for (const auto& c : cs->items)
        std::printf("%-6s %-16s %-10s %-40s %-24s %-5s %-6s %s%s\n", jtext(c.get("id")).c_str(), jtext(c.get("user")).c_str(), jtext(c.get("role")).c_str(), jtext(c.get("peer")).c_str(), jtext(c.get("since")).c_str(),
                    jtext(c.get("inTransaction")).c_str(), jtext(c.get("listening")).c_str(), jtext(c.get("requests")).c_str(), c.get("self") && c.get("self")->kind == json::Json::True ? "  (this one)" : "");
      std::fflush(stdout); return 0;
    }
    // status
    auto st = json::parse(text(link.call('S', {})));
    auto line = [](const char* k, const std::string& v) { std::printf("%-13s %s\n", k, v.c_str()); };
    line("server", server + (st.get("tls") && st.get("tls")->kind == json::Json::True ? " (TLS)" : ""));
    line("version", jtext(st.get("version")) + " (" + jtext(st.get("platform")) + ")");
    line("role", jtext(st.get("serverRole")) + (st.get("readOnly") && st.get("readOnly")->kind == json::Json::True ? ", read-only" : ""));
    line("uptime", st.get("uptimeMs") ? human_duration(int64_t(jnum(st.get("uptimeMs")))) : "-");
    line("keys", std::to_string(uint64_t(jnum(st.get("keys")))));
    line("size", human_bytes(jnum(st.get("bytes"))) + " (transaction " + std::to_string(uint64_t(jnum(st.get("txid")))) + ")");
    line("connections", std::to_string(uint64_t(jnum(st.get("connections")))));
    line("user", st.get("user") && st.get("user")->is_string() ? st.get("user")->str + " (" + jtext(st.get("role")) + ")" : "- (an open server: every client is an administrator)");
    if (auto* rp = st.get("replica"); rp && rp->kind == json::Json::Object)
      line("replication", "follows " + jtext(rp->get("leader")) + ", " + jtext(rp->get("state")) + ", lag " + jtext(rp->get("lagTransactions")) + " transactions" + (jtext(rp->get("lastError")).empty() ? "" : " (last error: " + jtext(rp->get("lastError")) + ")"));
    if (auto* sy = st.get("sync"); sy && sy->kind == json::Json::Object)
      line("sync", jtext(sy->get("followers")) + " followers for " + jtext(sy->get("replicas")) + " required" + (sy->get("degraded") && sy->get("degraded")->kind == json::Json::True ? ", DEGRADED" : ""));
    if (auto* cl = st.get("cluster"); cl && cl->kind == json::Json::Object)
      line("cluster", jtext(cl->get("role")) + " in term " + jtext(cl->get("term")) + ", leader " + jtext(cl->get("leader")) + ", " + jtext(cl->get("members")) + " members, " + jtext(cl->get("elections")) + " elections here");
    line("verification", jtext(st.get("verification")));
    if (st.get("slowQueryMs") && jnum(st.get("slowQueryMs")) > 0) line("slow queries", "logged over " + jtext(st.get("slowQueryMs")) + " ms");
    std::fflush(stdout);
    return 0;
  } catch (const std::exception& e) { std::cerr << "kestro-server admin: " << e.what() << std::endl; return 1; }
}

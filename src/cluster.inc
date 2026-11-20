// Included by server.cpp: a cluster of kestro-servers with automatic failover.
//
// Every node lists every member (--cluster) and names itself (--cluster-self). One node is
// the leader; the others follow it, pulling its log as --replica-of followers do, and serve
// reads. The design follows Raft:
//   - Terms. Each election starts a new term; a node votes at most once per term, and its
//     term and vote reach the disk (<db>.cluster) before it answers.
//   - Elections. A follower that has not heard from a leader within a random time between
//     --election-timeout and twice it first asks for a pre-vote (would a majority vote for it?
//     no term changes, so a node that was cut off cannot depose a working leader when it
//     returns), then for votes. A node votes only for a candidate whose log is at least as
//     up to date as its own: a later last term, or the same last term and at least as many
//     transactions. A node that has heard from its leader recently refuses to vote at all.
//   - Terms of transactions. A log group does not record its term; each node keeps the term
//     history (term, first transaction) beside its file, taken from the leader it follows
//     before it applies that leader's transactions, so it knows the term of its last one.
//   - Commits. The leader answers a write once a majority (itself included) has it durably:
//     synchronous replication to (members / 2) followers. A write that a majority has can
//     only be in a leader elected later (its voters include one of that majority).
//   - Lease. A leader that has not heard from a majority within the election timeout steps
//     down: writes waiting for confirmation fail with UNAVAILABLE (outcome unknown).
//   - Rejoining. A node whose log no longer continues the leader's (a former leader with
//     transactions no majority confirmed) cuts its log back to where the two term histories
//     agree, restarts in-process, and follows; if that is not possible it moves its file
//     aside and copies the leader's.
// Clients connect to any member: followers refuse writes with READONLY naming the leader.

struct TermEntry { uint64_t term = 0, first_txid = 0; bool operator==(const TermEntry& o) const { return term == o.term && first_txid == o.first_txid; } };
using TermHistory = std::vector<TermEntry>;
// The term of transaction txid: that of the last entry starting at or before it (0: before
// the cluster's first leader).
uint64_t term_of(const TermHistory& h, uint64_t txid) { uint64_t t = 0; for (const auto& e : h) { if (e.first_txid <= txid) t = e.term; else break; } return t; }
uint64_t term_start(const TermHistory& h, uint64_t txid) { uint64_t s = 0; for (const auto& e : h) { if (e.first_txid <= txid) s = e.first_txid; else break; } return s; }
// The last transaction two logs share: the highest txid both have whose term agrees (two logs
// with the same transaction in the same term are identical up to it).
uint64_t common_prefix(const TermHistory& a, uint64_t a_txid, const TermHistory& b, uint64_t b_txid) {
  uint64_t d = std::min(a_txid, b_txid);
  while (d > 0) {
    if (term_of(a, d) == term_of(b, d)) return d;
    uint64_t s = std::max(term_start(a, d), term_start(b, d));
    d = s ? s - 1 : 0;
  }
  return 0;
}
void put_history(Bytes& out, const TermHistory& h) {
  size_t at = out.size(); out.resize(at + 4 + 16 * h.size()); put32(out.data() + at, uint32_t(h.size()));
  for (size_t i = 0; i < h.size(); i++) { put64(out.data() + at + 4 + 16 * i, h[i].term); put64(out.data() + at + 12 + 16 * i, h[i].first_txid); }
}
TermHistory get_history(const unsigned char* p, size_t n, size_t& used) {
  require(n >= 4, "IO short cluster reply"); uint32_t k = get32(p); require(k <= 1000000 && n - 4 >= size_t(k) * 16, "IO bad term history");
  TermHistory h(k); for (uint32_t i = 0; i < k; i++) { h[i].term = get64(p + 4 + 16 * i); h[i].first_txid = get64(p + 12 + 16 * i); }
  used = 4 + size_t(k) * 16; return h;
}

// Set by the cluster for the in-process restart (server_main returns RESTART_REJOIN).
constexpr int RESTART_REJOIN = 3;
struct Rejoin { bool requested = false, reseed = false; uint64_t txid = 0; } g_rejoin;
bool g_force_seed = false;  // the next start copies the leader's log (the file was moved aside)

struct Cluster {
  enum Role { Follower, Candidate, Leader };
  std::string self, token, ca, state_path; std::vector<std::string> peers;  // peers: every member but this one
  int election_ms = 1500;
  Store* db = nullptr;
  using Clock = std::chrono::steady_clock;
  std::mutex m;
  uint64_t term = 0; std::string voted_for; TermHistory history;
  Role role = Follower; std::string leader;
  Clock::time_point last_contact{}, deadline{};
  std::map<std::string, Clock::time_point> acks;  // leader: when each follower last asked for the log
  uint64_t elections = 0;
  uint64_t seen_end = 0, seen_txid = 0; Clock::time_point caught_up = Clock::now();  // a follower: the leader's log as last heard (metrics, readiness)
  std::atomic<bool> stop{false}; std::thread ticker, follower;
  std::mt19937_64 rnd{std::random_device{}()};

  bool enabled() const { return !self.empty(); }
  size_t majority() const { return (peers.size() + 1) / 2 + 1; }
  bool is_peer(const std::string& a) const { return std::find(peers.begin(), peers.end(), a) != peers.end(); }
  Clock::time_point next_deadline() { return Clock::now() + std::chrono::milliseconds(election_ms + int64_t(rnd() % uint64_t(election_ms))); }
  static const char* role_name(Role r) { return r == Leader ? "leader" : r == Candidate ? "candidate" : "follower"; }

  // <db>.cluster: term, vote and term history, replaced atomically and flushed.
  void load() {
    std::ifstream f(std::filesystem::u8path(state_path)); if (!f) return;
    std::string word; f >> word; require(word == "kestro-cluster", "FORMAT cluster state file");
    int version = 0; f >> version; require(version == 1, "FORMAT cluster state version");
    while (f >> word) {
      if (word == "term") f >> term;
      else if (word == "vote") { f >> voted_for; if (voted_for == "-") voted_for.clear(); }
      else if (word == "history") { TermEntry e; f >> e.term >> e.first_txid; history.push_back(e); }
      else throw std::runtime_error("FORMAT cluster state: " + word);
    }
  }
  void persist_locked() {
    std::string s = "kestro-cluster 1\nterm " + std::to_string(term) + "\nvote " + (voted_for.empty() ? "-" : voted_for) + "\n";
    for (const auto& e : history) s += "history " + std::to_string(e.term) + " " + std::to_string(e.first_txid) + "\n";
    auto temp = state_path + ".tmp"; platform::remove_noexcept(temp);
    { FD f(platform::open_file(temp, true)); write_at(f.n, s.data(), s.size(), 0); sync_file(f.n); }
#ifdef _WIN32
    if (!MoveFileExW(platform::path(temp).c_str(), platform::path(state_path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("IO cannot replace " + state_path);
#else
    std::filesystem::rename(std::filesystem::u8path(temp), std::filesystem::u8path(state_path)); platform::sync_parent(state_path);
#endif
  }
  void publish_leader_locked() { set_leader_hint(role == Leader ? std::string() : leader); }
  void step_down_locked(const std::string& why) {
    bool was = role == Leader; role = Follower; leader.clear(); deadline = next_deadline();
    g_readonly = true; g_cluster_leader = false; publish_leader_locked(); g_sync.cv.notify_all();
    if (was) std::cerr << "KESTRODB CLUSTER stepped down (term " << term << "): " << why << std::endl;
  }
  void adopt_term_locked(uint64_t t) {  // a newer term seen: follower, no vote yet
    if (t <= term) return;
    term = t; voted_for.clear();
    if (role != Follower) step_down_locked("term " + std::to_string(t) + " began elsewhere"); else { leader.clear(); publish_leader_locked(); }
    persist_locked();
  }
  void become_leader_locked() {
    role = Leader; leader = self; history.push_back({term, db->txid() + 1}); persist_locked();
    auto now = Clock::now(); for (const auto& p : peers) acks[p] = now;  // the followers have one timeout to find it
    g_readonly = false; g_cluster_leader = true; publish_leader_locked();
    std::cout << "KESTRODB CLUSTER LEADER term " << term << " " << self << std::endl;
  }

  // A short-lived connection to a member, for votes and questions.
  Bytes ask(const std::string& peer, const Bytes& q, int io_seconds = 2) const {
    LeaderLink l; l.open(peer, token, ca, 300, io_seconds); return l.call('Z', q);
  }
  // Votes (or pre-votes) from the other members, at most half an election timeout.
  bool poll_votes(uint64_t t, uint64_t txid, uint64_t last_term, bool pre) {
    struct State { std::mutex m; std::condition_variable cv; size_t votes = 1, pending = 0; uint64_t higher = 0; };
    auto st = std::make_shared<State>(); st->pending = peers.size();
    const size_t need = majority();
    if (st->votes >= need) return true;
    Bytes q(1 + 24); q[0] = pre ? 'p' : 'v'; put64(q.data() + 1, t); put64(q.data() + 9, txid); put64(q.data() + 17, last_term); q.insert(q.end(), self.begin(), self.end());
    for (const auto& p : peers) {
      std::thread([st, p, q, token = token, ca = ca] {
        bool granted = false; uint64_t rt = 0;
        try { LeaderLink l; l.open(p, token, ca, 300, 2); auto r = l.call('Z', q); if (r.size() >= 10) { rt = get64(r.data() + 1); granted = r[9] != 0; } } catch (...) {}
        std::lock_guard<std::mutex> g(st->m); if (granted) st->votes++; st->higher = std::max(st->higher, rt); st->pending--; st->cv.notify_all();
      }).detach();
    }
    size_t votes; uint64_t higher;
    { std::unique_lock<std::mutex> g(st->m); st->cv.wait_for(g, std::chrono::milliseconds(election_ms / 2), [&] { return st->votes >= need || st->pending == 0; }); votes = st->votes; higher = st->higher; }
    if (!pre && higher > t) { std::lock_guard<std::mutex> g(m); adopt_term_locked(higher); return false; }
    return votes >= need;
  }
  void elect() {
    uint64_t t, txid = db->txid(), last;
    { std::lock_guard<std::mutex> g(m); if (role == Leader) return; t = term + 1; last = term_of(history, txid); deadline = next_deadline(); }
    if (!poll_votes(t, txid, last, true)) return;  // no majority would vote: the term stays
    {
      std::lock_guard<std::mutex> g(m);
      if (role == Leader || term >= t) return;
      term = t; voted_for = self; role = Candidate; leader.clear(); publish_leader_locked(); persist_locked(); deadline = next_deadline(); elections++;
    }
    std::cerr << "KESTRODB CLUSTER election for term " << t << " (" << self << ")" << std::endl;
    bool won = poll_votes(t, txid, last, false);
    std::lock_guard<std::mutex> g(m);
    if (term == t && role == Candidate) { if (won) become_leader_locked(); }
  }
  // Election timer and the leader's lease.
  void tick() {
    while (!stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      bool start = false;
      {
        std::lock_guard<std::mutex> g(m); auto now = Clock::now();
        if (role == Leader) {
          size_t live = 1; for (const auto& p : peers) if (now - acks[p] < std::chrono::milliseconds(election_ms)) live++;
          if (live < majority()) step_down_locked("no contact with a majority for " + std::to_string(election_ms) + " ms");
        } else if (now >= deadline) start = true;
      }
      if (start) { try { elect(); } catch (const std::exception& e) { std::cerr << "KESTRODB CLUSTER election failed: " << e.what() << std::endl; } }
    }
  }
  // Who leads? Asks the members (a follower that lost its leader, or a node starting).
  std::string discover() {
    std::string hint;
    for (const auto& p : peers) {
      if (stop) break;
      try {
        auto r = ask(p, Bytes{'i'}); require(r.size() >= 20, "IO short cluster reply");
        uint64_t rt = get64(r.data() + 1); Role rr = Role(r[9]); uint16_t ln = uint16_t(r[18] | (r[19] << 8));
        require(r.size() >= 20u + ln, "IO short cluster reply"); std::string rl(reinterpret_cast<const char*>(r.data() + 20), ln);
        std::lock_guard<std::mutex> g(m);
        if (rt < term) continue;
        if (rr == Leader) return p;
        if (!rl.empty() && rl != self) hint = rl;
      } catch (...) {}
    }
    return hint;
  }
  // Follower: streams the leader's log; finds a new leader when it is lost.
  void follow() {
    std::unique_ptr<LeaderLink> link; std::string linked;
    while (!stop) {
      std::string target; Role r;
      { std::lock_guard<std::mutex> g(m); r = role; target = leader; }
      if (r == Leader) { link.reset(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
      if (target.empty()) {
        link.reset();
        auto found = discover();
        if (!found.empty()) { std::lock_guard<std::mutex> g(m); if (role != Leader && leader.empty()) leader = found; publish_leader_locked(); }
        else std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      try {
        if (!link || linked != target) { link = std::make_unique<LeaderLink>(); link->open(target, token, ca, 300, 5); linked = target; }
        uint64_t t; { std::lock_guard<std::mutex> g(m); t = term; }
        Bytes q(1 + 56); q[0] = 'f'; put64(q.data() + 1, t); put64(q.data() + 9, db->bytes()); auto ch = db->chain(); memcpy(q.data() + 17, ch.data(), 32);
        put32(q.data() + 49, 8u << 20); put32(q.data() + 53, uint32_t(std::max(50, election_ms / 6))); q.insert(q.end(), self.begin(), self.end());
        auto rep = link->call('Z', q);
        require(rep.size() >= 41, "IO short cluster reply");
        uint64_t rt = get64(rep.data() + 1); size_t used; auto hist = get_history(rep.data() + 41, rep.size() - 41, used);
        {
          std::lock_guard<std::mutex> g(m);
          if (rt < term) { if (leader == target) leader.clear(); publish_leader_locked(); continue; }  // a deposed leader
          adopt_term_locked(rt);
          if (role != Follower) step_down_locked("a leader for term " + std::to_string(rt) + " exists");
          leader = target; publish_leader_locked(); last_contact = Clock::now(); deadline = next_deadline();
          if (hist != history) { history = hist; persist_locked(); }  // before this term's transactions are applied
        }
        size_t at = 41 + used;
        if (rep.size() > at) db->apply_replicated(rep.data() + at, rep.size() - at);
        { bool level = db->bytes() >= get64(rep.data() + 9); std::lock_guard<std::mutex> g(m); seen_end = get64(rep.data() + 9); seen_txid = get64(rep.data() + 17); if (level) caught_up = Clock::now(); }
      } catch (const std::exception& e) {
        std::string what = e.what(); link.reset();
        if (what.rfind("NOTLEADER", 0) == 0) {
          auto hint = what.size() > 10 ? what.substr(10) : std::string();
          std::lock_guard<std::mutex> g(m); leader = (hint == "-" || hint == self || !is_peer(hint)) ? std::string() : hint; publish_leader_locked();
        } else if (what.rfind("STALE", 0) == 0 || what.rfind("DIVERGED", 0) == 0) {
          if (rejoin(target, what)) return;
        } else {
          std::lock_guard<std::mutex> g(m); if (leader == target) { leader.clear(); publish_leader_locked(); }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  }
  // This node's log does not continue the leader's: cut it back to where their term
  // histories agree (or copy the leader's), by restarting the server in-process.
  bool rejoin(const std::string& target, const std::string& why) {
    try {
      auto r = ask(target, Bytes{'i'}); require(r.size() >= 20, "IO short cluster reply");
      uint64_t lt = get64(r.data() + 1), ltx = get64(r.data() + 10); uint16_t ln = uint16_t(r[18] | (r[19] << 8));
      size_t used; auto lh = get_history(r.data() + 20 + ln, r.size() - 20 - ln, used);
      TermHistory mine; uint64_t my_term; { std::lock_guard<std::mutex> g(m); mine = history; my_term = term; }
      if (lt < my_term || Role(r[9]) != Leader) return false;  // not the leader any more: look again
      uint64_t mytx = db->txid(), d = common_prefix(mine, mytx, lh, ltx);
      g_rejoin.requested = true; g_rejoin.txid = d; g_rejoin.reseed = d >= mytx;
      std::cerr << "KESTRODB CLUSTER REJOIN " << why << ": " << (g_rejoin.reseed ? std::string("copying the leader's log") : "cutting the log back from transaction " + std::to_string(mytx) + " to " + std::to_string(d)) << std::endl;
      stop_server();
      return true;
    } catch (const std::exception& e) { std::cerr << "KESTRODB CLUSTER rejoin: " << e.what() << std::endl; return false; }
  }

  // Requests from other members ('Z' frames; replica or admin users).
  Bytes handle(Store& store, Store::SqlSession& session, const Config& cfg, const unsigned char* b, size_t n) {
    require(n >= 2, "INPUT cluster request"); char op = char(b[1]); const unsigned char* p = b + 2; size_t len = n - 2;
    Bytes out(1, 0);
    if (op == 'i') {
      std::lock_guard<std::mutex> g(m);
      out.resize(20); put64(out.data() + 1, term); out[9] = uint8_t(role); put64(out.data() + 10, store.txid());
      std::string l = role == Leader ? self : leader; out[18] = uint8_t(l.size() & 255); out[19] = uint8_t(l.size() >> 8);
      out.insert(out.end(), l.begin(), l.end()); put_history(out, history);
      return out;
    }
    if (op == 'v' || op == 'p') {
      require(len > 24, "INPUT vote request");
      uint64_t t = get64(p), ctx = get64(p + 8), clast = get64(p + 16); std::string cand(reinterpret_cast<const char*>(p + 24), len - 24);
      bool pre = op == 'p', granted = false;
      std::lock_guard<std::mutex> g(m); auto now = Clock::now();
      // A member that hears from its leader refuses: a node that was cut off cannot disrupt it.
      bool sticky = (role == Follower && !leader.empty() && now - last_contact < std::chrono::milliseconds(election_ms)) || role == Leader;
      if (!sticky && t >= term && is_peer(cand)) {
        uint64_t mytx = store.txid(), mylast = term_of(history, mytx);
        bool up_to_date = clast > mylast || (clast == mylast && ctx >= mytx);
        if (pre) granted = t > term && up_to_date;
        else {
          adopt_term_locked(t);
          if ((voted_for.empty() || voted_for == cand) && up_to_date) { voted_for = cand; persist_locked(); granted = true; deadline = next_deadline(); }
        }
      }
      out.resize(10); put64(out.data() + 1, term); out[9] = granted ? 1 : 0;
      return out;
    }
    if (op == 'f') {
      require(len > 56, "INPUT cluster fetch");
      uint64_t t = get64(p), from = get64(p + 8); Hash chain; memcpy(chain.data(), p + 16, 32);
      uint32_t max = get32(p + 48), wait = get32(p + 52); std::string who(reinterpret_cast<const char*>(p + 56), len - 56);
      uint64_t my_term; TermHistory h;
      {
        std::lock_guard<std::mutex> g(m);
        if (t > term) adopt_term_locked(t);
        if (role != Leader) throw std::runtime_error("NOTLEADER " + (leader.empty() ? std::string("-") : leader));
        if (is_peer(who)) acks[who] = Clock::now();  // the lease: a member that follows this leader
        my_term = term; h = history;
      }
      // The follower has every transaction before from (its log continues this one up to
      // there: read_log checks): that is its confirmation, counted before waiting for news.
      auto c = store.read_log(from, chain, std::min<size_t>(max, 32u << 20), 0);
      if (cfg.sync_replicas > 0) g_sync.ack(&session, from, store.bytes());
      if (c.bytes.empty() && wait) c = store.read_log(from, chain, std::min<size_t>(max, 32u << 20), std::min<uint32_t>(wait, 2000));
      out.resize(41); put64(out.data() + 1, my_term); put64(out.data() + 9, c.leader_end); put64(out.data() + 17, c.leader_txid); memcpy(out.data() + 25, c.uuid.data(), 16);
      put_history(out, h); out.insert(out.end(), c.bytes.begin(), c.bytes.end());
      return out;
    }
    throw std::runtime_error("INPUT unknown cluster request");
  }
  std::string json() {
    std::lock_guard<std::mutex> g(m);
    auto ago = last_contact == Clock::time_point{} ? int64_t(-1) : int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - last_contact).count());
    return "{\"self\":\"" + self + "\",\"term\":" + std::to_string(term) + ",\"role\":\"" + role_name(role) + "\",\"leader\":" + (role == Leader ? "\"" + self + "\"" : leader.empty() ? std::string("null") : "\"" + leader + "\"") +
      ",\"members\":" + std::to_string(peers.size() + 1) + ",\"majority\":" + std::to_string(majority()) + ",\"elections\":" + std::to_string(elections) +
      ",\"lastLeaderContactMs\":" + std::to_string(ago) + "}";
  }
  void start() {
    { std::lock_guard<std::mutex> g(m); deadline = next_deadline(); publish_leader_locked(); }
    g_readonly = true; g_cluster_leader = false;
    ticker = std::thread([this] { tick(); }); follower = std::thread([this] { follow(); });
  }
  void finish() { stop = true; if (ticker.joinable()) ticker.join(); if (follower.joinable()) follower.join(); }
};

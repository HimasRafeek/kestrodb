// Included inside class Store (kestro.hpp): the SQL executor. See SQL.md.
//
// Tables are collections whose rows are JSON objects keyed by their "id" (stored as the
// order-preserving encoding of the id, so key order is id order). Every statement is
// atomic: SELECT reads one committed state (readers_ shared); INSERT, UPDATE, DELETE and
// DDL run under the writer lock and commit all their changes as one transaction.
public:
 // Runs one statement. params: a JSON array for the ? placeholders. Returns JSON:
 //   SELECT -> {"rows":[{...},...]}; INSERT/UPDATE/DELETE -> {"changes":n[,"lastInsertId":id]};
 //   CREATE/DROP -> {"changes":0}.
 // A connection's SQL session: the state of BEGIN ... COMMIT. Use one per connection or
 // handle, from one thread at a time. Without a session every statement commits alone.
 struct SqlSession {
  bool active=false;
  std::map<std::string,std::optional<std::string>,std::less<>> overlay;  // key -> new value (nullopt: deleted)
  std::unordered_map<SecIndex*,UniqueGroup> unique;                     // unique values claimed so far
  bool track=false;std::set<std::string,std::less<>> dirty;            // group commit: keys changed since the last commit
  size_t bytes=0;                                                       // changes kept (an upper bound)
  struct Save {std::string name;std::map<std::string,std::optional<std::string>,std::less<>> overlay;std::unordered_map<SecIndex*,UniqueGroup> unique;bool began=false;size_t notes=0;};
  std::vector<Save> saves;                                              // SAVEPOINTs, oldest first
  bool concurrent=false;Snap snap;                                      // BEGIN CONCURRENT: its snapshot (store_mvcc.inc)
  ReadSet reads;                                                        // and what it read, unless ISOLATION LEVEL SNAPSHOT
  Listener*listener=nullptr;                                            // LISTEN's receiver (store_notify.inc); none: refused
  std::vector<Note> notes;std::set<std::pair<std::string,std::string>> noted;  // NOTIFY: sent at COMMIT, each distinct one once
 };
 std::string sql(std::string_view text,std::string_view params_json="[]"){return sql(nullptr,text,params_json);}
 std::string sql(SqlSession*session,std::string_view text,std::string_view params_json){
  auto pp=parsed(text);const auto*st=&pp->st;
  Params params=bind_params(*pp,params_json);
  SessionScope scope(session);
  struct TxnSnap{Snap*prev=nullptr;bool set=false;explicit TxnSnap(SqlSession*s){if(s&&s->active&&s->concurrent){prev=tl_snap();tl_snap()=&s->snap;set=true;}}~TxnSnap(){if(set)tl_snap()=prev;}} txn_snap(session);
  auto run=[&]{return std::visit([&](const auto&s){return run_sql(s,params);},*st);};
  // Row writes outside a transaction share commits with writes from other threads.
  if(!active_session()&&(std::holds_alternative<sql::Insert>(*st)||std::holds_alternative<sql::Update>(*st)||std::holds_alternative<sql::Delete>(*st)))return grouped(run);
  return run();
 }
 // What a statement does and every table it touches (joins and subqueries included), for
 // permission checks before running it.
 enum class SqlKind {Read,Write,Schema,Transaction};
 // lists_tables: SHOW TABLES (the server shows a table-limited user only its own).
 // unscoped: a statement naming no table it acts on (DROP INDEX name): refused to such a user.
 struct SqlAccess {SqlKind kind=SqlKind::Read;std::vector<std::string> tables;bool lists_tables=false,unscoped=false;};
 SqlAccess sql_access(std::string_view text){
  auto pp=parsed(text);const auto*st=&pp->st;SqlAccess a;
  auto add=[&](const std::string&t){if(std::find(a.tables.begin(),a.tables.end(),t)==a.tables.end())a.tables.push_back(t);};
  // Every place a statement can name a table: FROM and joins, compound SELECTs, and
  // subqueries anywhere (select list, WHERE, HAVING, GROUP BY, ORDER BY, LIMIT, OFFSET,
  // window clauses).
  std::function<void(const sql::Select&)> select;
  std::function<void(const sql::Expr&)> expr=[&](const sql::Expr&e){
   if(e.sub)select(*e.sub);for(const auto&x:e.args)expr(x);
   if(e.over){for(const auto&p:e.over->partition)expr(p);for(const auto&o:e.over->order)expr(o.first);}
  };
  // WITH names in scope are not tables, but the tables their bodies and derived tables read
  // are. All names of a WITH are in scope in all its bodies, as when it runs (with_select).
  std::vector<std::string> ctes;
  select=[&](const sql::Select&q){
   size_t scope=ctes.size();
   for(const auto&c:q.with)ctes.push_back(c.name);
   for(const auto&c:q.with)select(*c.q);
   for(const auto&f:q.from){
    if(f.sub)select(*f.sub);
    else if(!sql::internal_table(f.table)&&std::find(ctes.begin(),ctes.end(),f.table)==ctes.end())add(f.table);
    if(f.on)expr(*f.on);
   }
   for(const auto&it:q.items)if(!it.star)expr(it.e);
   if(q.where)expr(*q.where);if(q.having)expr(*q.having);
   for(const auto&g:q.group)expr(g);for(const auto&o:q.order)expr(o.e);
   if(q.limit)expr(*q.limit);if(q.offset)expr(*q.offset);
   for(const auto&p:q.compound)select(*p.q);
   ctes.resize(scope);
  };
  std::visit([&](const auto&s){
   using T=std::decay_t<decltype(s)>;
   if constexpr(std::is_same_v<T,sql::Select>){a.kind=SqlKind::Read;select(s);}
   else if constexpr(std::is_same_v<T,sql::Insert>){a.kind=SqlKind::Write;add(s.table);for(const auto&r:s.rows)for(const auto&e:r)expr(e);for(const auto&st:s.on_conflict_set)expr(st.second);}
   else if constexpr(std::is_same_v<T,sql::Update>){a.kind=SqlKind::Write;add(s.table);if(s.where)expr(*s.where);for(const auto&st:s.sets)expr(st.second);}
   else if constexpr(std::is_same_v<T,sql::Delete>){a.kind=SqlKind::Write;add(s.table);if(s.where)expr(*s.where);}
   else if constexpr(std::is_same_v<T,sql::Begin>||std::is_same_v<T,sql::Commit>||std::is_same_v<T,sql::Rollback>||std::is_same_v<T,sql::Savepoint>||std::is_same_v<T,sql::Release>||std::is_same_v<T,sql::RollbackTo>)a.kind=SqlKind::Transaction;
   else if constexpr(std::is_same_v<T,sql::CreateTable>){a.kind=SqlKind::Schema;add(s.table);for(const auto&f:s.rules.fks)add(f.table);}
   else if constexpr(std::is_same_v<T,sql::AlterTable>){a.kind=SqlKind::Schema;add(s.table);if(s.action==sql::AlterTable::RenameTable)add(s.to);for(const auto&f:s.rules.fks)add(f.table);}
   else if constexpr(std::is_same_v<T,sql::ShowTables>){a.kind=SqlKind::Read;a.lists_tables=true;}
   else if constexpr(std::is_same_v<T,sql::Listen>){a.kind=SqlKind::Read;if(changes_channel(s.channel))add(s.channel.substr(CHANGES_PREFIX.size()));}  // a table's changes: read access to it
   else if constexpr(std::is_same_v<T,sql::Notify>){a.kind=SqlKind::Read;}
   else if constexpr(std::is_same_v<T,sql::Describe>||std::is_same_v<T,sql::ShowIndexes>){a.kind=SqlKind::Read;add(s.table);}
   else{a.kind=SqlKind::Schema;if constexpr(!std::is_same_v<T,sql::DropIndex>)add(s.table);else if(!s.table.empty())add(s.table);else a.unscoped=true;}
  },*st);
  // Writes reach further through foreign keys: the children of a changed table (actions
  // write them, checks read them; recursively), and the parents of a written table (read).
  if(a.kind==SqlKind::Write&&!a.tables.empty()){
   {WriterLock w(*this);materialize();load_catalog_locked();}
   std::shared_lock<ReadWriteMutex>r(readers_);
   for(size_t i=0;i<a.tables.size();i++){
    const std::string t=a.tables[i];
    for(const auto&[ct,rr]:rules_)for(const auto&f:rr.c.fks){
     if(f.table==t)add(ct);  // actions write its rows; RESTRICT checks read them
     if(ct==t)add(f.table);
    }
   }
  }
  return a;
 }
 // Whether a key/value collection is a SQL table (the server keeps non-administrators'
 // key/value writes out of SQL tables, whose constraints only SQL statements check).
 bool is_sql_table(std::string_view collection){
  {WriterLock w(*this);materialize();load_catalog_locked();}
  std::shared_lock<ReadWriteMutex>r(readers_);return table_exists(std::string(collection));
 }
 // A time limit for the statements this thread runs (the server sets one per statement):
 // long scans and joins check it and fail with LIMIT. Zero: none.
 struct StatementDeadline{
  std::chrono::steady_clock::time_point prev;
  explicit StatementDeadline(std::chrono::milliseconds limit):prev(deadline()){deadline()=limit.count()>0?std::chrono::steady_clock::now()+limit:std::chrono::steady_clock::time_point{};}
  ~StatementDeadline(){deadline()=prev;}
  static std::chrono::steady_clock::time_point&deadline(){static thread_local std::chrono::steady_clock::time_point d{};return d;}
 };
 // A closed connection: an open transaction is rolled back.
 void end_session(SqlSession&s){s.saves.clear();s.bytes=0;s.notes.clear();s.noted.clear();if(s.active){s.overlay.clear();s.unique.clear();s.active=false;if(s.concurrent){end_snapshot(s.snap);s.concurrent=false;s.reads=ReadSet{};}else gate_exit(&s);}}
private:
 using Params=std::vector<json::Scalar>;
 // ---------- Group commit of SQL writes ----------
 // Autocommit INSERT, UPDATE and DELETE statements from threads writing at the same time
 // share commits. One thread (the leader) runs the waiting statements in arrival order
 // inside one session, so each sees the changes of the ones before it, and commits them
 // together. While a group's data is being written to disk, the leader already runs the
 // next group's statements (they see the in-flight group through the session; readers
 // see it only once it is durable). A statement that fails changes nothing and gets its
 // own error. No result is returned before its group is durable; if a commit fails, its
 // statements and the next group's fail.
 struct GroupItem {const std::function<std::string()>*run;std::string result;std::exception_ptr error;bool done=false;};
 std::mutex gq_m_;std::condition_variable gq_cv_;std::deque<GroupItem*> gq_;bool gq_leader_=false;
 static constexpr size_t GROUP_STATEMENTS=256,GROUP_ROWS=16384;
 static constexpr size_t MAX_RESULT=size_t(64)<<20,MAX_TRANSACTION_BYTES=size_t(1)<<30;  // a reply's size; one transaction's changes
 // The statement's time limit (StatementDeadline), checked every 1,024 calls.
 static void check_deadline(){
  static thread_local unsigned n=0;if((++n&1023)!=0)return;
  auto d=StatementDeadline::deadline();
  if(d!=std::chrono::steady_clock::time_point{}&&std::chrono::steady_clock::now()>d)throw std::runtime_error("LIMIT the statement ran past its time limit; add an index, narrow the WHERE, or LIMIT the rows");
 }
 struct Deferred{};  // a statement that must not run while a group is being written
 static bool&tl_overlap(){static thread_local bool o=false;return o;}
 static const Store*&tl_writer_held(){static thread_local const Store*h=nullptr;return h;}  // the store whose writer lock this thread holds
 // The writer lock, unless this thread (a group leader mid-commit, or a statement around
 // this one) already holds it.
 struct WriterLock{std::unique_lock<std::mutex> l;const Store*prev=nullptr;bool mine=false;explicit WriterLock(Store&s){if(tl_writer_held()!=&s){l=std::unique_lock<std::mutex>(s.writer_);prev=tl_writer_held();tl_writer_held()=&s;mine=true;}}~WriterLock(){if(mine)tl_writer_held()=prev;}WriterLock(const WriterLock&)=delete;WriterLock&operator=(const WriterLock&)=delete;};
 struct WriterHeld{const Store*prev;explicit WriterHeld(const Store&s):prev(tl_writer_held()){tl_writer_held()=&s;}~WriterHeld(){tl_writer_held()=prev;}WriterHeld(const WriterHeld&)=delete;WriterHeld&operator=(const WriterHeld&)=delete;};
 std::string grouped(const std::function<std::string()>&run){
  GroupItem me{&run,{},{},false};
  std::unique_lock<std::mutex>l(gq_m_);
  gq_.push_back(&me);
  for(;;){
   if(me.done){if(me.error)std::rethrow_exception(me.error);return std::move(me.result);}
   if(!gq_leader_){gq_leader_=true;l.unlock();try{lead_groups();}catch(...){}l.lock();gq_leader_=false;gq_cv_.notify_all();continue;}
   gq_cv_.wait(l);
  }
 }
 std::vector<GroupItem*> take_batch(){std::lock_guard<std::mutex>l(gq_m_);std::vector<GroupItem*> b;while(!gq_.empty()&&b.size()<GROUP_STATEMENTS){b.push_back(gq_.front());gq_.pop_front();}return b;}
 void finish_items(const std::vector<GroupItem*>&b){if(b.empty())return;{std::lock_guard<std::mutex>l(gq_m_);for(auto*x:b)x->done=true;}gq_cv_.notify_all();}
 static void run_alone(GroupItem*x){try{x->result=(*x->run)();}catch(...){x->error=std::current_exception();}}
 void lead_groups(){
  auto batch=take_batch();if(batch.empty())return;
  if(batch.size()==1){run_alone(batch[0]);finish_items(batch);return;}
  std::vector<GroupItem*> later;  // statements that did not fit: run alone afterwards
  {
   SqlSession S;S.track=true;
   gate_enter(&S);
   struct Exit{Store&s;SqlSession&g;~Exit(){g.overlay.clear();g.unique.clear();g.dirty.clear();g.active=false;s.gate_exit(&g);}}exit{*this,S};
   S.active=true;SessionScope scope(&S);
   struct Round{std::vector<GroupItem*> items,ok;};
   std::vector<GroupItem*> deferred;
   auto run_batch=[&](const std::vector<GroupItem*>&b,Round&r,bool overlapping){
    for(auto*x:b){
     if(S.overlay.size()>=GROUP_ROWS){later.push_back(x);continue;}
     tl_overlap()=overlapping;
     try{x->result=(*x->run)();r.items.push_back(x);r.ok.push_back(x);}
     catch(const Deferred&){deferred.push_back(x);}
     catch(...){x->error=std::current_exception();r.items.push_back(x);}
     tl_overlap()=false;
    }
   };
   Round cur;run_batch(batch,cur,false);
   for(int round=0;;round++){
    std::vector<Op> ops;
    for(const auto&k:S.dirty){auto it=S.overlay.find(k);if(it==S.overlay.end())continue;if(it->second)ops.push_back({k,*it->second,ANY,false});else if(current_value(k))ops.push_back({k,"",ANY,true});}
    S.dirty.clear();
    bool more=round<7&&S.overlay.size()<GROUP_ROWS;
    std::vector<GroupItem*> nb=more?take_batch():std::vector<GroupItem*>{};
    Round next;std::exception_ptr err;bool overlapped=false;
    if(!ops.empty()){
     // Runs the waiting statements, and keeps taking newly arrived ones while the write is
     // still in flight (the disk time is free for them).
     std::function<void(const std::function<bool()>&)> ov=[&](const std::function<bool()>&io_done){
      overlapped=true;run_batch(nb,next,true);
      while(more&&!io_done()&&S.overlay.size()<GROUP_ROWS){auto m=take_batch();if(m.empty()){std::this_thread::yield();continue;}nb.insert(nb.end(),m.begin(),m.end());run_batch(m,next,true);}
     };
     std::lock_guard<std::mutex>w(writer_);WriterHeld held(*this);
     try{auto out=commit_group_locked({&ops},true,&ov);if(out[0].error)err=out[0].error;}catch(...){err=std::current_exception();}
    }
    if(err){
     for(auto*x:cur.ok)x->error=err;for(auto*x:next.ok)x->error=err;
     finish_items(cur.items);finish_items(next.items);
     for(auto*x:nb)if(std::find(next.items.begin(),next.items.end(),x)==next.items.end()&&std::find(deferred.begin(),deferred.end(),x)==deferred.end()&&std::find(later.begin(),later.end(),x)==later.end())later.push_back(x);
     for(auto*x:deferred)later.push_back(x);deferred.clear();
     break;
    }
    finish_items(cur.items);
    if(!overlapped)run_batch(nb,next,false);
    {auto d=std::move(deferred);deferred.clear();run_batch(d,next,false);}
    if(next.items.empty()&&S.dirty.empty())break;
    cur=std::move(next);
   }
  }
  for(auto*x:later)run_alone(x);
  finish_items(later);
 }
 // ---------- Transactions ----------
 // The write gate: held by one writer at a time, either a transaction (from BEGIN to
 // COMMIT/ROLLBACK, across calls and threads) or a single commit. Readers never take it.
 std::mutex gate_m_;std::condition_variable gate_cv_;const void*gate_owner_=nullptr;std::thread::id gate_thread_;
 bool gate_enter(const void*who){
  std::unique_lock<std::mutex>l(gate_m_);
  if(gate_owner_==who)return false;
  if(gate_owner_&&gate_thread_==std::this_thread::get_id())throw std::runtime_error("INPUT a SQL transaction is open on this connection; COMMIT or ROLLBACK it before writing outside it");
  gate_cv_.wait(l,[&]{return gate_owner_==nullptr;});
  gate_owner_=who;gate_thread_=std::this_thread::get_id();return true;
 }
 void gate_exit(const void*who){{std::lock_guard<std::mutex>l(gate_m_);if(gate_owner_==who)gate_owner_=nullptr;}gate_cv_.notify_all();}
 struct GateHold {Store&s;const void*who;bool held;GateHold(Store&st,const void*w):s(st),who(w),held(st.gate_enter(w)){}~GateHold(){if(held)s.gate_exit(who);}GateHold(const GateHold&)=delete;GateHold&operator=(const GateHold&)=delete;};
 // The session of the statement running on this thread (set by sql()).
 static SqlSession*&tl_session(){static thread_local SqlSession*s=nullptr;return s;}
 struct SessionScope {SqlSession*prev;explicit SessionScope(SqlSession*s):prev(tl_session()){tl_session()=s;}~SessionScope(){tl_session()=prev;}};
 static SqlSession*active_session(){auto*s=tl_session();return s&&s->active?s:nullptr;}
 static const void*gate_token(const void*own){auto*s=active_session();return s?static_cast<const void*>(s):own;}
 // A key's value as this statement sees it: the transaction's own changes first.
 std::optional<std::string> cur(std::string_view key){
  if(auto*s=active_session()){auto it=s->overlay.find(key);if(it!=s->overlay.end())return it->second;}
  return current_value(key);
 }
 static bool overlay_touches(const std::string&table){
  auto*s=active_session();if(!s)return false;std::string p=table+'\0';
  auto it=s->overlay.lower_bound(p);return it!=s->overlay.end()&&it->first.compare(0,p.size(),p)==0;
 }
 // Rows of a table matching WHERE as this session sees them: committed rows the transaction
 // has not changed, then the transaction's own rows. Callers hold readers_ (shared) or the
 // writer lock, after prepare.
 template<class F> void scan_rows(const std::string&table,const std::optional<sql::Expr>&where,const Params&params,F&&cb){
  if(const auto*rows=virtual_rows(table)){  // a WITH table or derived table (rows in memory)
   for(const auto&doc:*rows){check_deadline();if(where){sql::Row row{doc,&params};if(!sql::truthy(sql::eval(*where,row)))continue;}cb(std::string_view(),uint64_t(0),std::string_view(doc));}
   return;
  }
  query::Spec q=base_spec(table,where,params);q.unlimited=true;
  std::string prefix=table+'\0';auto*ses=active_session();bool ov=overlay_touches(table);
  q.sink=[&](std::string_view user,uint64_t v,std::string_view doc){if(ov&&ses->overlay.count(prefix+std::string(user)))return;cb(user,v,doc);};
  query_core(q);
  if(ov)for(auto it=ses->overlay.lower_bound(prefix);it!=ses->overlay.end()&&it->first.compare(0,prefix.size(),prefix)==0;++it){
   check_deadline();
   if(!it->second)continue;
   if(where){sql::Row row{*it->second,&params};if(!sql::truthy(sql::eval(*where,row)))continue;}
   cb(std::string_view(it->first).substr(prefix.size()),uint64_t(0),std::string_view(*it->second));
  }
 }
 // A statement's writes: inside a transaction they are kept (unique values checked now
 // against the table and the transaction's earlier changes); otherwise committed.
 void finish_write(std::vector<Op>&ops){
  if(ops.empty())return;
  if(auto*ses=active_session()){
   {std::unordered_set<std::string_view> seen;for(const auto&o:ops)require(seen.insert(o.key).second,"INPUT duplicate row in one statement");}
   std::vector<IndexDelta> deltas;
   index_deltas(ops,[ses,this](std::string_view k)->std::optional<std::string>{auto it=ses->overlay.find(k);if(it!=ses->overlay.end())return it->second;return current_value(k);},deltas,ses->unique,false);
   require(ses->overlay.size()+ops.size()<=65536,"LIMIT a transaction can change at most 65,536 rows");
   {size_t add=0;for(const auto&o:ops)add+=o.key.size()+o.value.size()+64;ses->bytes+=add;
    require(ses->bytes<=MAX_TRANSACTION_BYTES,"LIMIT a transaction can change at most "+std::to_string(MAX_TRANSACTION_BYTES>>20)+" MB");}
   merge_unique(deltas,ops,ses->unique);
   if(ses->track)for(const auto&o:ops)ses->dirty.insert(o.key);
   for(auto&o:ops)ses->overlay[o.key]=o.erase?std::nullopt:std::optional<std::string>(std::move(o.value));
   return;
  }
  auto out=commit_group_locked({&ops},true);if(out[0].error)std::rethrow_exception(out[0].error);
 }
 static void no_transaction(const char*what){if(active_session())throw std::runtime_error(std::string("UNSUPPORTED SQL: ")+what+" inside a transaction; run it before BEGIN or after COMMIT");}
 // ---------- Subqueries ----------
 // Uncorrelated ones run once, before the statement, and become literals. Correlated ones
 // (naming a column of an outer table by its name or alias, e.g. o.user = u.id) run for each
 // row with those columns as literals, remembered per distinct value, inside the outer
 // statement's locks: every table they read is prepared first, and their queries take no
 // locks of their own (tl_nested).
 static bool&tl_nested(){static thread_local bool n=false;return n;}
 struct NestedScope{bool prev;NestedScope():prev(tl_nested()){tl_nested()=true;}~NestedScope(){tl_nested()=prev;}};
 // A statement's read lock; the outermost one is also the statement's snapshot (store_mvcc.inc).
 struct ReadHold{
  Store&st;std::shared_lock<ReadWriteMutex> l;Snap snap;bool own=false,attached=false;
  explicit ReadHold(Store&s):st(s){
   if(tl_nested())return;
   l=std::shared_lock<ReadWriteMutex>(s.readers_);
   if(!tl_snap()){snap.store=&s;snap.lock=&l;snap.end=s.meta_.end;snap.epoch=s.catalog_epoch_;tl_snap()=&snap;own=true;}
   else if(auto*t=tl_snap();t->txn&&t->store==&s&&!t->lock){
    // A statement of a BEGIN CONCURRENT transaction: its snapshot, with this lock to yield.
    if(t->epoch!=s.catalog_epoch_)throw std::runtime_error("CONFLICT the schema changed during this transaction; roll it back and run it again");
    t->lock=&l;t->since={};attached=true;
   }
  }
  ~ReadHold(){if(own){tl_snap()=nullptr;st.end_snapshot(snap);}if(attached)tl_snap()->lock=nullptr;}
  ReadHold(const ReadHold&)=delete;ReadHold&operator=(const ReadHold&)=delete;
 };
 struct HookScope{const sql::SubqueryHook*prev;explicit HookScope(const sql::SubqueryHook*h):prev(sql::subquery_hook()){sql::subquery_hook()=h;}~HookScope(){sql::subquery_hook()=prev;}};
 static void from_names(const sql::Select&q,std::set<std::string>&out){for(const auto&f:q.from)out.insert(f.alias.empty()?f.table:f.alias);}  // an aliased table is known only by its alias
 // fn on every column of q (and of subqueries inside it) qualified by a name of outer that q
 // does not define itself. clone: copies nested subqueries first (they are shared).
 static void outer_columns(sql::Select&q,const std::set<std::string>&outer,bool clone,const std::function<void(sql::Expr&)>&fn){
  std::set<std::string> inner;from_names(q,inner);
  std::set<std::string> visible;for(const auto&n:outer)if(!inner.count(n))visible.insert(n);
  if(visible.empty())return;
  std::function<void(sql::Expr&)> ex=[&](sql::Expr&e){
   if(e.kind==sql::Expr::Col&&e.path.size()>1&&visible.count(e.path[0])){fn(e);return;}
   if(e.sub){if(clone)e.sub=std::make_shared<sql::Select>(*e.sub);outer_columns(*e.sub,visible,clone,fn);}
   for(auto&a:e.args)ex(a);
  };
  for(auto&it:q.items)if(!it.star)ex(it.e);
  if(q.where)ex(*q.where);if(q.having)ex(*q.having);
  for(auto&g:q.group)ex(g);for(auto&o:q.order)ex(o.e);
  for(auto&f:q.from)if(f.on)ex(*f.on);
  for(auto&p:q.compound){if(clone)p.q=std::make_shared<sql::Select>(*p.q);outer_columns(*p.q,outer,clone,fn);}
 }
 static bool is_correlated(const sql::Select&q,const std::set<std::string>&outer){
  sql::Select copy=q;bool any=false;outer_columns(copy,outer,false,[&](sql::Expr&){any=true;});return any;
 }
 static void subquery_tables(const sql::Expr&e,std::set<std::string>&out){
  if(e.sub)read_tables(*e.sub,out);
  for(const auto&a:e.args)subquery_tables(a,out);
 }
 struct CorrelatedHook:sql::SubqueryHook{
  Store&st;const Params&params;std::set<std::string> outer;
  mutable std::map<std::pair<const sql::Expr*,std::string>,std::vector<json::Scalar>> memo;
  CorrelatedHook(Store&s,const Params&p,std::set<std::string> o):st(s),params(p),outer(std::move(o)){}
  std::vector<json::Scalar> values(const sql::Expr&node,const sql::Row&r,bool exists)const override{
   sql::Select q=*node.sub;std::string key;
   outer_columns(q,outer,true,[&](sql::Expr&c){
    sql::Expr x=c;if(!r.aliases)x.path.erase(x.path.begin());  // a single-table row has no qualifiers
    auto v=sql::eval(x,r);json::encode_into(key,v);
    sql::Expr l;l.kind=sql::Expr::Lit;l.lit=std::move(v);l.text=c.text;c=std::move(l);
   });
   auto k=std::make_pair(&node,std::move(key));
   if(auto it=memo.find(k);it!=memo.end())return it->second;
   if(exists&&!q.limit&&q.compound.empty()){sql::Expr one;one.kind=sql::Expr::Lit;one.lit=sql::number(1);q.limit=std::move(one);}
   NestedScope nest;
   auto out=st.subquery_values(q,params,exists);
   if(memo.size()<100000)memo.emplace(std::move(k),out);
   return out;
  }
 };
 static void subquery_tables(const sql::Select&q,std::set<std::string>&out){
  for(const auto&it:q.items)if(!it.star)subquery_tables(it.e,out);
  if(q.where)subquery_tables(*q.where,out);if(q.having)subquery_tables(*q.having,out);
  for(const auto&o:q.order)subquery_tables(o.e,out);for(const auto&f:q.from)if(f.on)subquery_tables(*f.on,out);
  for(const auto&p:q.compound)read_tables(*p.q,out);
 }
 // Runs fn (the statement) with its correlated subqueries evaluated per row.
 std::string with_correlated(const std::set<std::string>&outer,const std::set<std::string>&tables,const Params&params,const std::function<std::string()>&fn){
  for(const auto&t:tables)if(table_exists(t))prepare_query(t);
  CorrelatedHook hook(*this,params,outer);HookScope scope(&hook);
  return fn();
 }
 static bool select_has_subquery(const sql::Select&q){
  for(const auto&it:q.items)if(!it.star&&sql::has_subquery(it.e))return true;
  if(q.where&&sql::has_subquery(*q.where))return true;
  if(q.having&&sql::has_subquery(*q.having))return true;
  for(const auto&o:q.order)if(sql::has_subquery(o.e))return true;
  for(const auto&f:q.from)if(f.on&&sql::has_subquery(*f.on))return true;
  return false;
 }
 // Runs a subquery; values of its single column.
 std::vector<json::Scalar> subquery_values(const sql::Select&q,const Params&params,bool exists){
  auto text=run_sql(q,params);auto j=json::parse(text);std::vector<json::Scalar> out;
  const auto*rows=j.get("rows");if(!rows)return out;
  for(const auto&row:rows->items){
   if(exists){out.emplace_back();out.back().tag=json::TRUE_;break;}
   if(row.fields.size()!=1)throw std::runtime_error("INPUT SQL: a subquery used as a value must select exactly one column");
   out.push_back(row.fields[0].second.scalar());
  }
  return out;
 }
 // outer: names of the statement's tables; correlated subqueries stay (left = true).
 void resolve(sql::Expr&e,const Params&params,const std::set<std::string>*outer=nullptr,bool*left=nullptr){
  using E=sql::Expr;
  if(e.sub&&outer&&is_correlated(*e.sub,*outer)){*left=true;return;}
  if(e.kind==E::In&&e.args.size()==2&&e.args[1].kind==E::Subquery){
   if(outer&&is_correlated(*e.args[1].sub,*outer)){*left=true;resolve(e.args[0],params,outer,left);return;}
   auto vals=subquery_values(*e.args[1].sub,params,false);
   resolve(e.args[0],params,outer,left);e.args.resize(1);
   for(auto&v:vals){E l;l.kind=E::Lit;l.lit=std::move(v);e.args.push_back(std::move(l));}
   return;
  }
  if(e.kind==E::Subquery){
   auto vals=subquery_values(*e.sub,params,false);
   if(vals.size()>1)throw std::runtime_error("INPUT SQL: a subquery used as a single value returned more than one row");
   E l;l.kind=E::Lit;if(vals.empty())l.lit.tag=json::NUL;else l.lit=vals[0];l.text=e.text;e=std::move(l);return;
  }
  if(e.kind==E::Exists){bool any=!subquery_values(*e.sub,params,true).empty();E l;l.kind=E::Lit;l.lit.tag=any?json::TRUE_:json::FALSE_;l.text=e.text;e=std::move(l);return;}
  for(auto&a:e.args)resolve(a,params,outer,left);
 }
 sql::Select resolved_select(const sql::Select&q,const Params&params,bool*left){
  sql::Select c=q;std::set<std::string> outer;from_names(c,outer);
  for(auto&it:c.items)if(!it.star)resolve(it.e,params,&outer,left);
  if(c.where)resolve(*c.where,params,&outer,left);
  if(c.having)resolve(*c.having,params,&outer,left);
  for(auto&f:c.from)if(f.on)resolve(*f.on,params,&outer,left);
  for(auto&o:c.order)resolve(o.e,params,&outer,left);
  return c;
 }
 // ---------- WITH tables and derived tables ----------
 // A WITH table (common table expression) or a derived table (FROM (SELECT ...) alias) is
 // computed into JSON rows in memory once per statement, and FROM reads it like a table:
 // scan_rows, select_core and require_table look a name up here before the stored tables.
 // Names are per thread and scoped (the latest definition wins). rows == nullptr: a WITH
 // table not computed yet, which nothing may read (a circular reference).
 struct VirtualTable {std::string name;std::shared_ptr<const std::vector<std::string>> rows;};
 static std::vector<VirtualTable>&tl_virtual(){static thread_local std::vector<VirtualTable> v;return v;}
 struct VirtualScope {size_t n;VirtualScope():n(tl_virtual().size()){}~VirtualScope(){tl_virtual().resize(n);}VirtualScope(const VirtualScope&)=delete;VirtualScope&operator=(const VirtualScope&)=delete;};
 static constexpr size_t MAX_RECURSIVE_ROWS=1000000;
 static const std::vector<std::string>*virtual_rows(const std::string&t){
  if(t.size()==1&&t[0]=='\x01'){static const std::vector<std::string> one{"{}"};return &one;}  // SELECT without FROM: one row, no columns
  const auto&v=tl_virtual();
  for(size_t i=v.size();i-->0;)if(v[i].name==t){
   if(!v[i].rows)throw std::runtime_error("INPUT SQL: circular reference: WITH table "+t+" is used before it is computed");
   return v[i].rows.get();
  }
  return nullptr;
 }
 // How often q names table t in a FROM, anywhere inside it (every place sql_access looks).
 static size_t table_refs(const sql::Select&q,const std::string&t){
  size_t n=0;
  std::function<void(const sql::Expr&)> ex=[&](const sql::Expr&e){
   if(e.sub)n+=table_refs(*e.sub,t);for(const auto&a:e.args)ex(a);
   if(e.over){for(const auto&p:e.over->partition)ex(p);for(const auto&o:e.over->order)ex(o.first);}
  };
  for(const auto&c:q.with)n+=table_refs(*c.q,t);
  for(const auto&f:q.from){if(f.sub)n+=table_refs(*f.sub,t);else if(f.table==t)n++;if(f.on)ex(*f.on);}
  for(const auto&it:q.items)if(!it.star)ex(it.e);
  if(q.where)ex(*q.where);if(q.having)ex(*q.having);
  for(const auto&g:q.group)ex(g);for(const auto&o:q.order)ex(o.e);
  if(q.limit)ex(*q.limit);if(q.offset)ex(*q.offset);
  for(const auto&p:q.compound)n+=table_refs(*p.q,t);
  return n;
 }
 // The tables a SELECT reads, to prepare before taking its read lock (WITH names included:
 // preparing a name that is no table does nothing).
 static void read_tables(const sql::Select&q,std::set<std::string>&out){
  for(const auto&c:q.with)read_tables(*c.q,out);
  for(const auto&f:q.from)if(f.sub)read_tables(*f.sub,out);else if(!sql::internal_table(f.table))out.insert(f.table);
  subquery_tables(q,out);
 }
 // WITH tables or derived tables to compute first. A SELECT without FROM also runs nested
 // (in with_select), where no plan mistakes its one-row table for a stored one.
 static bool needs_with_select(const sql::Select&s){
  if(!s.with.empty())return true;
  for(const auto&f:s.from)if(f.sub||(!tl_nested()&&sql::internal_table(f.table)))return true;
  return false;
 }
 // Every table the statement reads is prepared first; then one read lock and snapshot cover
 // the whole statement and its parts run nested (no locks of their own), as correlated
 // subqueries do. A WITH table is computed only if the statement uses it, after the WITH
 // tables its own body uses (in any order, as in SQLite).
 std::string with_select(const sql::Select&s,const Params&params){
  if(!tl_nested()){std::set<std::string> tables;read_tables(s,tables);for(const auto&t:tables)prepare_query(t);}
  ReadHold rl(*this);healthy();NestedScope nest;VirtualScope scope;
  const size_t base=tl_virtual().size();
  for(const auto&c:s.with)tl_virtual().push_back({c.name,nullptr});
  std::vector<int> state(s.with.size(),0);  // 0: not computed, 1: being computed, 2: done
  std::function<void(size_t)> define=[&](size_t i){
   const auto&c=s.with[i];
   if(state[i]==2)return;
   if(state[i]==1)throw std::runtime_error("INPUT SQL: circular reference: WITH table "+c.name);
   state[i]=1;
   for(size_t j=0;j<s.with.size();j++)if(j!=i&&table_refs(*c.q,s.with[j].name))define(j);
   auto rows=table_refs(*c.q,c.name)?recursive_rows(c,params):virtual_table(*c.q,c.cols,c.name,params);
   tl_virtual()[base+i].rows=std::move(rows);state[i]=2;
  };
  sql::Select q=s;q.with.clear();
  for(size_t i=0;i<s.with.size();i++)if(table_refs(q,s.with[i].name))define(i);
  for(auto&f:q.from)if(f.sub){auto rows=virtual_table(*f.sub,{},f.alias,params);tl_virtual().push_back({f.table,std::move(rows)});f.sub.reset();}
  return run_sql(q,params);
 }
 static std::string renamed(const std::vector<std::string>&names,const json::Json&row){
  std::string d="{";for(size_t i=0;i<names.size();i++){if(i)d+=',';d+=json::quote(names[i]);d+=':';d+=row.fields[i].second.raw;}return d+"}";
 }
 // The rows of a SELECT as documents; cols (a WITH column list) renames them by position.
 std::shared_ptr<const std::vector<std::string>> virtual_table(const sql::Select&q,const std::vector<std::string>&cols,const std::string&name,const Params&params){
  auto mismatch=[&](size_t n){throw std::runtime_error("INPUT SQL: WITH "+name+" names "+std::to_string(cols.size())+" columns but its SELECT returns "+std::to_string(n));};
  if(!cols.empty()){bool star=false;for(const auto&it:q.items)star|=it.star;if(!star&&q.items.size()!=cols.size())mismatch(q.items.size());}
  auto res=json::parse(run_sql(q,params));auto rows=std::make_shared<std::vector<std::string>>();
  if(const auto*list=res.get("rows"))for(const auto&r:list->items){
   if(cols.empty()){rows->push_back(r.raw);continue;}
   if(r.fields.size()!=cols.size())mismatch(r.fields.size());
   rows->push_back(renamed(cols,r));
  }
  return rows;
 }
 // A WITH table that names itself: base SELECTs, then UNION [ALL] recursive SELECTs that
 // read it. Each round runs the recursive SELECTs over the rows the round before added (the
 // name reads only those) until a round adds none. UNION drops rows seen before (so cycles
 // end), UNION ALL keeps them. A LIMIT on the body stops it early; past MAX_RECURSIVE_ROWS
 // rows or the result size limit it fails with LIMIT, so runaway recursion ends cleanly.
 std::shared_ptr<const std::vector<std::string>> recursive_rows(const sql::Cte&c,const Params&params){
  const sql::Select&body=*c.q;
  auto bad=[&](const std::string&m){throw std::runtime_error("INPUT SQL: WITH "+c.name+": "+m);};
  if(body.compound.empty())bad("circular reference (a WITH table that names itself is base SELECT UNION [ALL] recursive SELECT)");
  if(!body.with.empty())bad("a recursive WITH table cannot have a WITH of its own");
  if(!body.order.empty())bad("ORDER BY in a recursive WITH is not supported");
  sql::Select first=body;first.compound.clear();first.limit.reset();first.offset.reset();
  std::vector<const sql::Select*> parts{&first};for(const auto&p:body.compound)parts.push_back(p.q.get());
  size_t nb=0;while(nb<parts.size()&&!table_refs(*parts[nb],c.name))nb++;
  if(nb==0||nb==parts.size())bad("circular reference (the first SELECT starts the recursion and does not name "+c.name+")");
  bool dedup=false;
  for(size_t k=nb;k<parts.size();k++){
   const auto&p=body.compound[k-1];
   if(p.op!=sql::SetPart::Union)bad("recursive SELECTs follow UNION or UNION ALL");
   if(!p.all)dedup=true;
   size_t top=0;for(const auto&f:parts[k]->from)if(!f.sub&&f.table==c.name)top++;
   if(top!=1||table_refs(*parts[k],c.name)!=1)bad("each recursive SELECT names "+c.name+" once, in its FROM (not in a subquery), after the base SELECTs");
   if(is_aggregate(*parts[k])||select_has_window(*parts[k]))bad("recursive SELECTs cannot use aggregates or window functions");
  }
  sql::Select base=first;for(size_t k=1;k<nb;k++)base.compound.push_back(body.compound[k-1]);
  long long limit=body.limit?const_int(*body.limit,params,"LIMIT"):-1,offset=body.offset?const_int(*body.offset,params,"OFFSET"):0;
  const size_t want=limit<0?SIZE_MAX:size_t(offset+limit);
  std::vector<std::string> names=c.cols;std::unordered_set<std::string> seen;
  auto out=std::make_shared<std::vector<std::string>>();std::vector<std::string> fresh;size_t bytes=0;
  auto run=[&](const sql::Select&q){
   auto res=json::parse(run_sql(q,params));const auto*list=res.get("rows");if(!list)return;
   for(const auto&r:list->items){
    check_deadline();  // a recursion can grow for long: the statement time limit stops it
    if(out->size()+fresh.size()>=want)return;
    if(names.empty())for(const auto&f:r.fields)names.push_back(f.first);
    if(r.fields.size()!=names.size())bad("its SELECTs return "+std::to_string(r.fields.size())+" columns, not "+std::to_string(names.size()));
    if(dedup){std::string key;for(const auto&f:r.fields){key+=f.second.raw;key+='\x1f';}if(!seen.insert(std::move(key)).second)continue;}
    if(out->size()+fresh.size()>=MAX_RECURSIVE_ROWS)throw std::runtime_error("LIMIT WITH "+c.name+" produced more than 1,000,000 rows; end the recursion with a WHERE condition or a LIMIT");
    fresh.push_back(renamed(names,r));bytes+=fresh.back().size();
    require(bytes<=MAX_RESULT,"LIMIT WITH "+c.name+" is larger than "+std::to_string(MAX_RESULT>>20)+" MB; end the recursion sooner");
   }
  };
  run(base);
  while(!fresh.empty()){
   auto level=std::make_shared<std::vector<std::string>>(std::move(fresh));fresh.clear();
   out->insert(out->end(),level->begin(),level->end());
   if(out->size()>=want)break;
   check_deadline();
   VirtualScope round;tl_virtual().push_back({c.name,level});
   for(size_t k=nb;k<parts.size();k++)run(*parts[k]);
  }
  out->erase(out->begin(),out->begin()+std::ptrdiff_t(std::min(out->size(),size_t(offset))));
  return out;
 }
 // A SELECT over one WITH table or derived table: its rows are in memory, so every plan
 // reads them through scan_rows (never query_core or the column cache).
 std::string virtual_select(const sql::Select&s,const Params&params){
  long long limit=s.limit?const_int(*s.limit,params,"LIMIT"):-1,offset=s.offset?const_int(*s.offset,params,"OFFSET"):0;
  if(is_aggregate(s))return aggregate_select(s,params,limit,offset,[&](const RowSink&sink){
   scan_rows(s.table,s.where,params,[&](std::string_view,uint64_t,std::string_view doc){sql::Row row{doc,&params};sink(row,doc,0);});
  });
  if(limit==0)return "{\"rows\":[]}";
  std::vector<std::string> docs;
  scan_rows(s.table,s.where,params,[&](std::string_view,uint64_t,std::string_view doc){docs.emplace_back(doc);});
  return finish_rows(s,params,limit,offset,std::move(docs),nullptr);
 }
 // Parsed statements by text, like prepared statements (cleared when it grows large).
 std::mutex sql_cache_m_;std::unordered_map<std::string,std::shared_ptr<const sql::Parsed>> sql_cache_;
 // Parameters as a JSON array (by position) or, for :name parameters, a JSON object
 // (keys with or without the leading : or @).
 static Params bind_params(const sql::Parsed&p,std::string_view params_json){
  size_t i=0;while(i<params_json.size()&&(params_json[i]==' '||params_json[i]=='\n'||params_json[i]=='\t'||params_json[i]=='\r'))i++;
  if(i<params_json.size()&&params_json[i]=='{'){
   if(p.names.empty())throw std::runtime_error("INPUT SQL: parameters given by name, but the statement has no :name parameters");
   auto obj=json::parse(params_json);Params out;out.reserve(p.names.size());
   for(const auto&n:p.names){
    const json::Json*v=obj.get(n);if(!v)v=obj.get(":"+n);if(!v)v=obj.get("@"+n);
    if(!v)throw std::runtime_error("INPUT SQL: missing parameter :"+n);
    out.push_back(v->scalar());
   }
   return out;
  }
  if(params_json.empty()||params_json=="[]")return {};
  return json::parse_scalars(params_json);
 }
 std::shared_ptr<const sql::Parsed> parsed(std::string_view text){
  std::string key(text);
  {std::lock_guard<std::mutex>l(sql_cache_m_);auto it=sql_cache_.find(key);if(it!=sql_cache_.end())return it->second;}
  auto st=std::make_shared<const sql::Parsed>(sql::parse_named(text));
  std::lock_guard<std::mutex>l(sql_cache_m_);if(sql_cache_.size()>=1024)sql_cache_.clear();sql_cache_.emplace(std::move(key),st);return st;
 }
 static std::string seq_key(const std::string&table){std::string k("\x01seq",4);k+='\0';k+=table;return k;}
 static std::string row_key(const std::string&table,const json::Scalar&id){
  std::string k=table;k+='\0';json::encode_into(k,id);require(k.size()<=1024,"LIMIT id too long");return k;
 }
 bool table_exists(const std::string&t)const{return std::find(tables_.begin(),tables_.end(),t)!=tables_.end();}
 void require_table(const std::string&t)const{if(!table_exists(t)&&!virtual_rows(t))throw std::runtime_error("INPUT SQL: no such table: "+t);}
 static long long const_int(const sql::Expr&e,const Params&params,const char*what){
  sql::Row ctx{"{}",&params};auto v=sql::eval(e,ctx);
  if(v.tag!=json::NUMBER||v.num<0||v.num!=std::floor(v.num)||v.num>1e15)throw std::runtime_error(std::string("INPUT SQL: ")+what+" must be a non-negative integer");
  return (long long)v.num;
 }
 // Conditions an index (or the id key) can use. They only narrow the scan: the whole WHERE
 // is still evaluated for every candidate row, so they never need to be exact.
 // exact stays true while every AND term became a condition with the same meaning as the
 // SQL (then WHERE need not be evaluated again per row).
 static void index_conds(const sql::Expr&e,const Params&params,std::vector<query::Cond>&out,bool&exact){
  using E=sql::Expr;using C=query::Cond;
  if(e.kind==E::And){index_conds(e.args[0],params,out,exact);index_conds(e.args[1],params,out,exact);return;}
  size_t before=out.size();bool term_exact=true;
  auto constant=[&](const E&x,json::Scalar&v){
   if(x.kind==E::Lit){v=x.lit;return true;}
   if(x.kind==E::Param){if(x.param>=params.size())return false;v=params[x.param];return true;}
   if(x.kind==E::Neg&&x.args[0].kind==E::Lit&&x.args[0].lit.tag==json::NUMBER){v=sql::number(-x.args[0].lit.num);return true;}
   return false;};
  auto add=[&](const E&col,C::Op op,const json::Scalar&v,std::vector<json::Scalar> list={}){
   if(sql::is_null(v)&&op!=C::In){term_exact=false;return;}
   C c;c.op=op;
   if(col.path.size()==1&&col.path[0]=="id"){
    if(op!=C::Eq&&op!=C::In)term_exact=false;  // key order mixes types; SQL comparisons do not
    c.is_key=true;c.field="_key";c.v.tag=json::STRING;c.v.str=json::encode(v);
    for(auto&x:list){json::Scalar s;s.tag=json::STRING;s.str=json::encode(x);c.list.push_back(s);}
   }else{
    for(size_t i=0;i<col.path.size();i++)c.field+=(i?".":"")+col.path[i];
    c.path=col.path;c.v=v;c.list=std::move(list);
   }
   out.push_back(std::move(c));};
  json::Scalar v;
  if(e.kind==E::Bin){
   static const std::pair<const char*,C::Op> ops[]={{"=",C::Eq},{"<",C::Lt},{"<=",C::Lte},{">",C::Gt},{">=",C::Gte}};
   static const std::pair<const char*,C::Op> flipped[]={{"=",C::Eq},{"<",C::Gt},{"<=",C::Gte},{">",C::Lt},{">=",C::Lte}};
   for(size_t k=0;k<5;k++){
    if(e.op!=ops[k].first)continue;
    if(e.args[0].kind==E::Col&&constant(e.args[1],v))add(e.args[0],ops[k].second,v);
    else if(e.args[1].kind==E::Col&&constant(e.args[0],v))add(e.args[1],flipped[k].second,v);
   }
  }else if(e.kind==E::In&&!e.negate&&e.args[0].kind==E::Col){
   std::vector<json::Scalar> list;
   for(size_t k=1;k<e.args.size();k++){if(!constant(e.args[k],v)){exact=false;return;}if(!sql::is_null(v))list.push_back(v);}
   json::Scalar none;none.tag=json::NUL;add(e.args[0],C::In,none,std::move(list));
  }else if(e.kind==E::Between&&!e.negate&&e.args[0].kind==E::Col){
   json::Scalar lo,hi;if(constant(e.args[1],lo)&&constant(e.args[2],hi)){add(e.args[0],C::Gte,lo);add(e.args[0],C::Lte,hi);}
  }else if(e.kind==E::Like&&!e.negate&&!e.insensitive&&e.args[0].kind==E::Col&&constant(e.args[1],v)&&v.tag==json::STRING){
   auto pct=v.str.find_first_of("%_");
   if(pct!=std::string::npos&&pct>0&&v.str[pct]=='%'&&pct==v.str.size()-1&&!(e.args[0].path.size()==1&&e.args[0].path[0]=="id")){json::Scalar p;p.tag=json::STRING;p.str=v.str.substr(0,pct);add(e.args[0],C::Prefix,p);}
  }
  if(out.size()==before||!term_exact)exact=false;
 }
 static query::Spec base_spec(const std::string&table,const std::optional<sql::Expr>&where,const Params&params){
  query::Spec q;q.collection=table;
  if(where){
   bool exact=true;index_conds(*where,params,q.where,exact);
   const sql::Expr*w=&*where;
   if(!exact)q.residual=[w,&params](std::string_view,std::string_view doc){sql::Row row{doc,&params};return sql::truthy(sql::eval(*w,row));};
   if(sql::fulltext_hook())q.keys=fulltext_keys(*where,table,params);  // MATCH ... AGAINST: the rows its index found
  }
  return q;
 }
 static std::string raw_field(std::string_view doc,const std::vector<std::string>&path){std::string_view raw;return json::find_path(doc,path,raw)?std::string(raw):std::string("null");}
 static std::string_view object_inner(std::string_view doc){
  while(!doc.empty()&&doc.front()!='{')doc.remove_prefix(1);while(!doc.empty()&&doc.back()!='}')doc.remove_suffix(1);
  return doc.size()>2?doc.substr(1,doc.size()-2):std::string_view();
 }
 // A column's raw JSON; in a join an unqualified column is looked up in each table in order.
 static std::string raw_column(std::string_view doc,const std::vector<std::string>&path,const std::vector<std::string>*aliases){
  if(aliases){
   for(const auto&a:*aliases)if(path.size()>1&&path[0]==a)return raw_field(doc,path);
   std::string_view raw;std::vector<std::string> p;
   for(const auto&a:*aliases){p.assign(1,a);p.insert(p.end(),path.begin(),path.end());if(json::find_path(doc,p,raw))return std::string(raw);}
   return "null";
  }
  return raw_field(doc,path);
 }
 using Values=std::unordered_map<const sql::Expr*,json::Scalar>;
 static bool select_has_window(const sql::Select&s){
  for(const auto&it:s.items)if(!it.star&&sql::has_window(it.e))return true;
  for(const auto&o:s.order)if(sql::has_window(o.e))return true;
  return false;
 }
 static void collect_windows(const sql::Expr&e,std::vector<const sql::Expr*>&out){
  if(e.over){out.push_back(&e);return;}
  for(const auto&a:e.args)collect_windows(a,out);
 }
 // Window function values for every row (docs: the rows WHERE kept, as documents).
 static std::vector<Values> window_values(const sql::Select&s,const std::vector<std::string>&docs,const Params&params,const std::vector<std::string>*aliases){
  return window_rows(s,docs.size(),params,[&](size_t i,const sql::Expr&x){sql::Row row{docs[i],&params};row.aliases=aliases;return sql::eval(x,row);});
 }
 // Window function values for count rows (documents, or the groups of an aggregate); value(i, x)
 // evaluates x on row i. Frames follow SQLite: ROWS counts rows, GROUPS counts peer groups, and
 // RANGE n PRECEDING / n FOLLOWING takes the rows whose (single) ORDER BY value is a number
 // within n of the current row's; a row whose value is not a number gets its peers.
 template<class F> static std::vector<Values> window_rows(const sql::Select&s,size_t count,const Params&params,const F&value){
  using W=sql::Window;
  std::vector<const sql::Expr*> wins;
  for(const auto&it:s.items)if(!it.star)collect_windows(it.e,wins);
  for(const auto&o:s.order)collect_windows(order_expr(s,o.e),wins);
  std::vector<Values> out(count);
  auto bad=[](const std::string&m){return std::runtime_error("INPUT SQL: "+m);};
  for(const auto*w:wins){
   const auto&win=*w->over;const auto&f=w->op;
   sql::Row none{std::string_view("{}"),&params};
   // Frame offsets are constants; ROWS and GROUPS count whole rows or groups.
   auto offset=[&](const std::optional<sql::Expr>&x,const char*which){
    if(!x)return 0.0;auto v=sql::eval(*x,none);bool whole=win.unit!=W::Range;
    if(v.tag!=json::NUMBER||v.num<0||(whole&&v.num!=std::floor(v.num)))throw bad(std::string("frame ")+which+" offset must be a non-negative "+(whole?"integer":"number"));
    return v.num;};
   const double start_n=offset(win.start_n,"starting"),end_n=offset(win.end_n,"ending");
   if(win.unit==W::Range&&(win.start_n||win.end_n)&&win.order.size()!=1)throw bad("RANGE with offset PRECEDING/FOLLOWING requires one ORDER BY expression");
   // Partitions, in order of first appearance.
   std::unordered_map<std::string,size_t> pid;std::vector<std::vector<size_t>> parts;
   for(size_t i=0;i<count;i++){
    std::string k;for(const auto&p:win.partition)json::encode_into(k,value(i,p));
    auto it=pid.find(k);if(it==pid.end()){it=pid.emplace(k,parts.size()).first;parts.emplace_back();}
    parts[it->second].push_back(i);
   }
   std::vector<std::vector<json::Scalar>> keys(count);  // ORDER BY values, once per row
   if(!win.order.empty())for(size_t i=0;i<count;i++)for(const auto&o:win.order)keys[i].push_back(value(i,o.first));
   for(auto&idx:parts){
    check_deadline();
    auto less=[&](size_t a,size_t b){for(size_t k=0;k<win.order.size();k++){const auto&x=keys[a][k],&y=keys[b][k];if(scalar_less(x,y))return !win.order[k].second;if(scalar_less(y,x))return win.order[k].second;}return false;};
    std::stable_sort(idx.begin(),idx.end(),less);
    const size_t n=idx.size();const long long N=(long long)n;
    // Peer groups: rows equal in every ORDER BY key (all rows are peers without ORDER BY).
    std::vector<size_t> group_end(n),group_start(n),group_no(n),firsts;
    for(size_t a=0;a<n;){size_t b=a+1;while(b<n&&!less(idx[a],idx[b])&&!less(idx[b],idx[a]))b++;for(size_t k=a;k<b;k++){group_start[k]=a;group_end[k]=b-1;group_no[k]=firsts.size();}firsts.push_back(a);a=b;}
    // One bound of row k's frame, as a position in the partition (may lie outside it).
    auto bound=[&](size_t k,W::Bound b,double off,bool start)->long long{
     if(b==W::UnboundedPreceding)return 0;
     if(b==W::UnboundedFollowing)return N-1;
     if(b==W::CurrentRow)return win.unit==W::Rows?(long long)k:(long long)(start?group_start[k]:group_end[k]);
     const bool back=b==W::Preceding;
     if(win.unit==W::Rows)return back?(long long)k-(long long)off:(long long)k+(long long)off;
     if(win.unit==W::Groups){
      long long g=(long long)group_no[k]+(back?-(long long)off:(long long)off);
      if(g<0)return start?0:-1;if(g>=(long long)firsts.size())return start?N:N-1;
      return start?(long long)firsts[size_t(g)]:(long long)group_end[firsts[size_t(g)]];
     }
     const auto&v=keys[idx[k]][0];
     if(v.tag!=json::NUMBER)return (long long)(start?group_start[k]:group_end[k]);
     const bool desc=win.order[0].second;const double t=back!=desc?v.num-off:v.num+off;
     // Where a row's value sorts against t; values that are not numbers sort before or after every number.
     auto rel=[&](size_t j){const auto&x=keys[idx[j]][0];if(x.tag==json::NUMBER)return x.num<t?-1:x.num>t?1:0;return sql::is_null(x)||x.tag<json::NUMBER?-1:1;};
     auto first_not=[&](auto pred){size_t a=0,z=n;while(a<z){size_t m=(a+z)/2;if(pred(m))a=m+1;else z=m;}return (long long)a;};
     if(start)return first_not([&](size_t j){int r=rel(j);return desc?r>0:r<0;});  // first row at or past t
     return first_not([&](size_t j){int r=rel(j);return desc?r>=0:r<=0;})-1;      // last row at or before t
    };
    // Row k's frame [fs, fe], clipped to the partition; empty when fs > fe.
    auto frame=[&](size_t k,long long&fs,long long&fe){fs=std::max(0LL,bound(k,win.start,start_n,true));fe=std::min(N-1,bound(k,win.end,end_n,false));};
    auto arg=[&](size_t k,size_t a)->json::Scalar{if(a>=w->args.size())throw bad(f+" needs more arguments");return value(idx[k],w->args[a]);};
    auto num=[&](size_t a,long long d)->long long{if(a>=w->args.size())return d;auto v=sql::eval(w->args[a],none);if(v.tag!=json::NUMBER)throw bad(f+" needs a number");return (long long)v.num;};
    if(f=="ROW_NUMBER"){for(size_t k=0;k<n;k++)out[idx[k]][w]=sql::number(double(k+1));continue;}
    if(f=="RANK"){for(size_t k=0;k<n;k++)out[idx[k]][w]=sql::number(double(group_start[k]+1));continue;}
    if(f=="DENSE_RANK"){for(size_t k=0;k<n;k++)out[idx[k]][w]=sql::number(double(group_no[k]+1));continue;}
    if(f=="PERCENT_RANK"){for(size_t k=0;k<n;k++)out[idx[k]][w]=sql::number(n>1?double(group_start[k])/double(n-1):0.0);continue;}
    if(f=="CUME_DIST"){for(size_t k=0;k<n;k++)out[idx[k]][w]=sql::number(double(group_end[k]+1)/double(n));continue;}
    if(f=="NTILE"){long long b=num(0,1);if(b<1)throw bad("NTILE needs a positive number");
     size_t buckets=size_t(b),small=n/buckets,big=n%buckets;size_t k=0;
     for(size_t t=0;t<buckets&&k<n;t++){size_t sz=small+(t<big?1:0);for(size_t q=0;q<sz&&k<n;q++,k++)out[idx[k]][w]=sql::number(double(t+1));}
     continue;}
    if(f=="LAG"||f=="LEAD"){long long off=num(1,1);
     for(size_t k=0;k<n;k++){long long j=f=="LAG"?(long long)k-off:(long long)k+off;
      json::Scalar v;if(j>=0&&j<(long long)n)v=arg(size_t(j),0);else if(w->args.size()>2)v=arg(k,2);else v.tag=json::NUL;
      out[idx[k]][w]=v;}
     continue;}
    if(f=="FIRST_VALUE"||f=="LAST_VALUE"||f=="NTH_VALUE"){
     long long nth=1;if(f=="NTH_VALUE"){if(w->args.size()!=2)throw bad("NTH_VALUE takes two arguments");auto v=sql::eval(w->args[1],none);if(v.tag!=json::NUMBER||v.num<1||v.num!=std::floor(v.num))throw bad("second argument to NTH_VALUE must be a positive integer");nth=(long long)v.num;}
     for(size_t k=0;k<n;k++){long long fs,fe;frame(k,fs,fe);long long j=f=="LAST_VALUE"?fe:fs+nth-1;json::Scalar v;v.tag=json::NUL;if(fs<=fe&&j<=fe)v=arg(size_t(j),0);out[idx[k]][w]=v;}
     continue;}
    // Aggregates over each row's frame. Frames only move forward, so rows join at the end and
    // leave at the start: COUNT, SUM and AVG add and subtract, MIN and MAX keep a queue of the
    // rows that can still be the answer.
    if(w->args.size()!=1+size_t(w->filter))throw bad(f+" takes one argument");
    const bool star=w->args[0].kind==sql::Expr::Star,adds=f=="SUM"||f=="AVG",lowest=f=="MIN",ranks=lowest||f=="MAX";
    std::vector<json::Scalar> av(n);std::vector<char> counted(n,0);  // each row's argument; whether it counts (not NULL, kept by FILTER)
    for(size_t k=0;k<n;k++){
     if(w->filter&&!sql::truthy(value(idx[k],w->args.back())))continue;
     if(star){counted[k]=1;continue;}
     av[k]=value(idx[k],w->args[0]);if(sql::is_null(av[k]))continue;
     if(adds&&av[k].tag!=json::NUMBER)throw bad(f+" needs numbers");
     counted[k]=1;
    }
    long long cnt=0;KSum sum;std::deque<size_t> best;size_t lo=0,hi=0;  // rows [lo, hi) are in the running state
    auto join=[&](size_t j){if(!counted[j])return;cnt++;if(adds)sum.add(av[j].num);else if(ranks){while(!best.empty()&&(lowest?!scalar_less(av[best.back()],av[j]):!scalar_less(av[j],av[best.back()])))best.pop_back();best.push_back(j);}};
    auto leave=[&](size_t j){if(!counted[j])return;cnt--;if(adds)sum.add(-av[j].num);};
    for(size_t k=0;k<n;k++){
     check_deadline();
     long long fs,fe;frame(k,fs,fe);
     size_t from=size_t(fs),to=size_t(std::max(fe+1,fs));  // [from, to)
     if(from<lo||to<hi){cnt=0;sum=KSum();best.clear();lo=hi=from;}  // a frame that moved back: start over
     for(;lo<from;lo++)if(lo<hi)leave(lo);
     if(hi<lo)hi=lo;
     for(;hi<to;hi++)join(hi);
     while(!best.empty()&&best.front()<lo)best.pop_front();
     json::Scalar r;r.tag=json::NUL;
     if(f=="COUNT")r=sql::number(double(cnt));
     else if(f=="SUM"){if(cnt)r=sql::number(sum.value());}
     else if(f=="AVG"){if(cnt)r=sql::number(sum.value()/double(cnt));}
     else if(!best.empty())r=av[best.front()];
     out[idx[k]][w]=r;
    }
   }
  }
  return out;
 }
 static std::string project(const sql::Select&s,std::string_view doc,const Params&params,const std::vector<std::string>*aliases=nullptr,const Values*values=nullptr){
  if(!aliases&&s.items.size()==1&&s.items[0].star)return std::string(doc);
  std::string out="{";bool first=true;
  auto append=[&](std::string_view inner){if(inner.empty())return;if(!first)out+=',';out.append(inner);first=false;};
  for(const auto&it:s.items){
   if(it.star){
    if(!aliases){append(object_inner(doc));continue;}
    std::string_view raw;
    for(const auto&a:*aliases)if(it.star_alias.empty()||it.star_alias==a){if(json::find_path(doc,{a},raw))append(object_inner(raw));}
    continue;
   }
   if(!first)out+=',';first=false;
   out+=json::quote(it.name);out+=':';
   if(it.e.kind==sql::Expr::Col)out+=raw_column(doc,it.e.path,aliases);
   else{sql::Row row{doc,&params};row.aliases=aliases;row.aggregates=values;out+=sql::to_json(sql::eval(it.e,row));}
  }
  return out+"}";
 }
 // ORDER BY may name an output column (alias) or its 1-based position.
 static const sql::Expr&order_expr(const sql::Select&s,const sql::Expr&e){
  if(e.kind==sql::Expr::Col&&e.path.size()==1)for(const auto&it:s.items)if(!it.star&&it.name==e.path[0]&&it.e.kind!=sql::Expr::Col)return it.e;
  if(e.kind==sql::Expr::Lit&&e.lit.tag==json::NUMBER){size_t k=size_t(e.lit.num);if(k>=1&&k<=s.items.size()&&!s.items[k-1].star)return s.items[k-1].e;throw std::runtime_error("INPUT SQL: ORDER BY position out of range");}
  return e;
 }
 static bool scalar_less(const json::Scalar&a,const json::Scalar&b){
  bool an=sql::is_null(a),bn=sql::is_null(b);if(an||bn)return an&&!bn;  // NULLs first
  return json::compare(a,b)<0;
 }
 static std::string rows_json(const std::vector<std::string>&rows){
  size_t n=16;for(const auto&r:rows)n+=r.size()+1;
  require(n<=MAX_RESULT,"LIMIT the result is larger than "+std::to_string(MAX_RESULT>>20)+" MB; select fewer rows (LIMIT/OFFSET) or columns");
  std::string out;out.reserve(n);out="{\"rows\":[";
  for(size_t i=0;i<rows.size();i++){if(i)out+=',';out+=rows[i];}
  return out+"]}";
 }
 static void distinct_rows(std::vector<std::string>&rows){std::unordered_set<std::string> seen;std::vector<std::string> out;for(auto&r:rows)if(seen.insert(r).second)out.push_back(std::move(r));rows=std::move(out);}

 static const json::Scalar&null_scalar(){static const json::Scalar n=[]{json::Scalar x;x.tag=json::NUL;return x;}();return n;}
 static bool is_aggregate(const sql::Select&s){
  if(!s.group.empty()||s.having)return true;
  for(const auto&it:s.items)if(!it.star&&sql::has_aggregate(it.e))return true;
  return false;
 }
 std::string run_sql(const sql::Select&s0,const Params&params){
  if(needs_with_select(s0))return with_select(s0,params);
  if(!s0.compound.empty())return compound_select(s0,params);
  if(select_has_subquery(s0)){
   bool left=false;auto c=resolved_select(s0,params,&left);
   if(!left)return run_select(c,params);
   std::set<std::string> outer,tables;from_names(c,outer);subquery_tables(c,tables);
   return with_correlated(outer,tables,params,[&]{return run_select(c,params);});
  }
  if(auto r=point_select(s0,params))return std::move(*r);
  return run_select(s0,params);
 }
 // SELECT ... FROM t WHERE id = v, or WHERE col = v on a UNIQUE index: at most one row, read
 // directly (no plan, no scan, no writer lock). Anything else, or a state the general path
 // must prepare first (catalog not loaded, lazy index, index not built), goes there.
 std::optional<std::string> point_select(const sql::Select&s,const Params&params){
  using E=sql::Expr;
  if(s.from.size()>1||!s.where||!s.group.empty()||s.having||s.distinct||s.where->kind!=E::Bin||s.where->op!="=")return std::nullopt;
  if(tl_nested()||active_session()||sql::subquery_hook())return std::nullopt;
  if(auto*sn=tl_snap();sn&&sn->store==this&&sn->registered)return std::nullopt;
  for(const auto&it:s.items)if(!it.star&&(sql::has_aggregate(it.e)||sql::has_window(it.e)||sql::has_match(it.e)))return std::nullopt;
  for(const auto&o:s.order)if(sql::has_window(o.e)||sql::has_match(o.e))return std::nullopt;
  const E*col=nullptr;json::Scalar v;
  auto constant=[&](const E&x){
   if(x.kind==E::Lit){v=x.lit;return true;}
   if(x.kind==E::Param){if(x.param>=params.size())return false;v=params[x.param];return true;}
   return false;};
  const auto&a=s.where->args[0],&b=s.where->args[1];
  if(a.kind==E::Col&&constant(b))col=&a;else if(b.kind==E::Col&&constant(a))col=&b;else return std::nullopt;
  if(sql::is_null(v))return std::nullopt;
  long long limit=s.limit?const_int(*s.limit,params,"LIMIT"):-1,offset=s.offset?const_int(*s.offset,params,"OFFSET"):0;
  std::shared_lock<ReadWriteMutex>r(readers_);healthy();
  if(lazy_||!cat_loaded_)return std::nullopt;
  require_table(s.table);
  std::string key;
  if(col->path.size()==1&&col->path[0]=="id")key=row_key(s.table,v);
  else{
   std::string field;for(size_t i=0;i<col->path.size();i++)field+=(i?".":"")+col->path[i];
   const SecIndex*ix=nullptr;for(const auto&x:sec_)if(x->collection==s.table&&x->field==field&&x->cols.empty()){ix=x.get();break;}
   if(!ix||!ix->unique||!ix->built)return std::nullopt;
   auto enc=json::encode(v);
   auto p=ix->set.seek(enc);
   if(ix->set.valid(p)){auto e=ix->set.get(p);if(e.size()>enc.size()&&!e.compare(0,enc.size(),enc)&&json::encoded_length(e)==enc.size())key.assign(e.substr(enc.size()));}
  }
  std::string row;bool found=false;
  if(!key.empty()){Ref ref;auto h=index_hash(key_,key);if(lookup(shard_of(h),key,h,ref)){std::string buf;row=project(s,view_ref(ref,buf),params);found=true;}}
  if(!found||limit==0||offset>0)return std::string("{\"rows\":[]}");
  std::string out;out.reserve(row.size()+12);out="{\"rows\":[";out+=row;out+="]}";return out;
 }
 // UNION / INTERSECT / EXCEPT: each SELECT runs on its own; rows are matched by position
 // (names from the first SELECT); ORDER BY (a name or a position) and LIMIT apply to the whole.
 std::string compound_select(const sql::Select&s0,const Params&params){
  struct Row {std::vector<std::string> cells;std::string key;};
  std::vector<std::string> names;
  auto run_part=[&](const sql::Select&q,bool first){
   sql::Select part=q;part.compound.clear();part.order.clear();part.limit.reset();part.offset.reset();
   auto res=json::parse(run_sql(part,params));std::vector<Row> rows;
   const auto*list=res.get("rows");
   if(first){for(const auto&it:part.items)names.push_back(it.name);}
   if(list)for(const auto&r:list->items){
    Row row;
    if(first&&rows.empty()&&!r.fields.empty()){names.clear();for(const auto&f:r.fields)names.push_back(f.first);}
    for(const auto&f:r.fields){row.cells.push_back(f.second.raw);row.key+=f.second.raw;row.key+='\x1f';}
    rows.push_back(std::move(row));
   }
   return rows;
  };
  auto out=run_part(s0,true);
  auto distinct=[&](std::vector<Row>&v){std::unordered_set<std::string> seen;std::vector<Row> o;for(auto&r:v)if(seen.insert(r.key).second)o.push_back(std::move(r));v=std::move(o);};
  bool any_distinct=false;
  for(const auto&p:s0.compound){
   auto rows=run_part(*p.q,false);
   for(const auto&r:rows)if(!out.empty()&&r.cells.size()!=out[0].cells.size())throw std::runtime_error("INPUT SQL: each SELECT of a UNION, INTERSECT or EXCEPT needs the same number of columns");
   if(!out.empty()&&!rows.empty()&&rows[0].cells.size()!=names.size()&&names.size()==out[0].cells.size())throw std::runtime_error("INPUT SQL: each SELECT of a UNION, INTERSECT or EXCEPT needs the same number of columns");
   if(p.op==sql::SetPart::Union){for(auto&r:rows)out.push_back(std::move(r));if(!p.all){distinct(out);any_distinct=true;}}
   else{
    std::unordered_map<std::string,size_t> right;for(const auto&r:rows)right[r.key]++;
    std::vector<Row> kept;
    if(!p.all)distinct(out);
    for(auto&r:out){auto it=right.find(r.key);bool in=it!=right.end()&&it->second>0;
     if(p.op==sql::SetPart::Intersect){if(in){kept.push_back(std::move(r));if(p.all)it->second--;}}
     else{if(in){if(p.all)it->second--;}else kept.push_back(std::move(r));}}
    out=std::move(kept);
   }
  }
  (void)any_distinct;
  if(!out.empty()&&names.size()!=out[0].cells.size()){names.clear();for(size_t i=0;i<out[0].cells.size();i++)names.push_back("column"+std::to_string(i+1));}
  auto object=[&](const Row&r){std::string t="{";for(size_t i=0;i<r.cells.size();i++){if(i)t+=',';t+=json::quote(names[i]);t+=':';t+=r.cells[i];}return t+"}";};
  std::vector<std::string> docs;docs.reserve(out.size());for(const auto&r:out)docs.push_back(object(r));
  if(!s0.order.empty()){
   std::vector<std::vector<json::Scalar>> keys(docs.size());
   for(size_t i=0;i<docs.size();i++){
    for(const auto&o:s0.order){
     if(o.e.kind==sql::Expr::Lit&&o.e.lit.tag==json::NUMBER){size_t k=size_t(o.e.lit.num);if(k<1||k>out[i].cells.size())throw std::runtime_error("INPUT SQL: ORDER BY position out of range");keys[i].push_back(json::scalar_of(out[i].cells[k-1]));}
     else{sql::Row row{docs[i],&params};keys[i].push_back(sql::eval(o.e,row));}
    }
   }
   std::vector<size_t> idx(docs.size());for(size_t i=0;i<idx.size();i++)idx[i]=i;
   std::stable_sort(idx.begin(),idx.end(),[&](size_t a,size_t b){for(size_t k=0;k<s0.order.size();k++){const auto&x=keys[a][k],&y=keys[b][k];if(scalar_less(x,y))return !s0.order[k].desc;if(scalar_less(y,x))return s0.order[k].desc;}return false;});
   std::vector<std::string> sorted;sorted.reserve(docs.size());for(size_t i:idx)sorted.push_back(std::move(docs[i]));docs=std::move(sorted);
  }
  long long limit=s0.limit?const_int(*s0.limit,params,"LIMIT"):-1,offset=s0.offset?const_int(*s0.offset,params,"OFFSET"):0;
  size_t from=std::min(docs.size(),size_t(offset)),to=limit<0?docs.size():std::min(docs.size(),from+size_t(limit));
  return rows_json(std::vector<std::string>(std::make_move_iterator(docs.begin()+std::ptrdiff_t(from)),std::make_move_iterator(docs.begin()+std::ptrdiff_t(to))));
 }
 std::string run_select(const sql::Select&s,const Params&params){
  const bool fts=select_has_match(s);
  if(s.from.size()>1){if(fts)throw std::runtime_error("INPUT SQL: MATCH ... AGAINST is not supported in joins; select the matching rows in a subquery (WHERE id IN (SELECT id FROM t WHERE MATCH ...))");return join_select(s,params);}
  FtsScope ft;
  if(select_has_window(s)&&!is_aggregate(s)){  // with aggregates, windows run over the groups (aggregate_select)
   prepare_query(s.table);ReadHold rl(*this);healthy();require_table(s.table);
   if(fts)fulltext_prepare(ft,s,params);
   long long limit=s.limit?const_int(*s.limit,params,"LIMIT"):-1,offset=s.offset?const_int(*s.offset,params,"OFFSET"):0;
   std::vector<std::string> docs;
   scan_rows(s.table,s.where,params,[&](std::string_view,uint64_t,std::string_view doc){docs.emplace_back(doc);});
   return finish_rows(s,params,limit,offset,std::move(docs),nullptr);
  }
  if(is_aggregate(s)&&!fts&&!overlay_touches(s.table)){
   long long limit=s.limit?const_int(*s.limit,params,"LIMIT"):-1,offset=s.offset?const_int(*s.offset,params,"OFFSET"):0;
   if(auto r=column_aggregate(s,params,limit,offset))return *r;
  }
  prepare_query(s.table);
  ReadHold r(*this);healthy();
  require_table(s.table);
  if(fts)fulltext_prepare(ft,s,params);
  return select_core(s,params);
 }
 // ORDER BY a, b, ... (plain columns, one direction) on a table with a composite index: the
 // engine sorts, streaming from that index when the WHERE fixes the columns before them.
 // A last "id" breaks ties by the row key. Fills q's order; false: the general path.
 bool multi_order_spec(const sql::Select&s,query::Spec&q){
  bool any=false;for(const auto&ix:sec_)if(ix->collection==s.table&&!ix->cols.empty()&&!ix->fulltext)any=true;
  if(!any)return false;
  std::vector<std::vector<std::string>> paths;
  for(size_t k=0;k<s.order.size();k++){
   const auto&o=order_expr(s,s.order[k].e);
   if(o.kind!=sql::Expr::Col||s.order[k].desc!=s.order[0].desc)return false;
   if(o.path.size()==1&&o.path[0]=="id"){if(k+1<s.order.size())return false;q.order_more_key=true;continue;}
   paths.push_back(o.path);
  }
  if(paths.size()<2){q.order_more_key=false;return false;}
  auto name=[](const std::vector<std::string>&p){std::string f;for(size_t i=0;i<p.size();i++)f+=(i?".":"")+p[i];return f;};
  q.desc=s.order[0].desc;q.order_key=false;q.order_path=paths[0];q.order_field=name(paths[0]);
  for(size_t k=1;k<paths.size();k++){q.order_more.push_back(name(paths[k]));q.order_more_paths.push_back(paths[k]);}
  return true;
 }
 std::string select_core(const sql::Select&s,const Params&params){
  if(virtual_rows(s.table))return virtual_select(s,params);
  long long limit=s.limit?const_int(*s.limit,params,"LIMIT"):-1,offset=s.offset?const_int(*s.offset,params,"OFFSET"):0;
  bool aggregate=!s.group.empty()||s.having.has_value();
  for(const auto&it:s.items)if(!it.star&&sql::has_aggregate(it.e))aggregate=true;
  if(aggregate)return doc_aggregate(s,params,limit,offset);
  if(limit==0)return "{\"rows\":[]}";
  query::Spec q=base_spec(s.table,s.where,params);
  std::vector<std::string> rows;
  // One ORDER BY column (or none): the engine sorts, streaming from an index or the id
  // order when it can, and stops at OFFSET + LIMIT.
  bool simple=s.order.size()<=1&&!s.distinct;
  if(s.order.size()==1){
   const auto&o=order_expr(s,s.order[0].e);q.desc=s.order[0].desc;
   if(o.kind!=sql::Expr::Col)simple=false;
   else if(o.path.size()==1&&o.path[0]=="id")q.order_key=true;
   else{q.order_key=false;q.order_path=o.path;for(size_t i=0;i<o.path.size();i++)q.order_field+=(i?".":"")+o.path[i];}
  }
  if(s.order.size()>1&&!s.distinct)simple=multi_order_spec(s,q);
  if(simple&&!overlay_touches(s.table)&&(limit<0||offset+limit<=10000)){
   if(limit<0)q.unlimited=true;else q.limit=size_t(offset+limit);
   auto res=query_core(q);
   for(size_t i=size_t(offset);i<res.items.size();i++)rows.push_back(project(s,res.items[i].value,params));
   return rows_json(rows);
  }
  // General case: every match, sorted here by each ORDER BY expression.
  std::vector<std::string> docs;
  scan_rows(s.table,s.where,params,[&](std::string_view,uint64_t,std::string_view doc){docs.emplace_back(doc);});
  return finish_rows(s,params,limit,offset,std::move(docs),nullptr);
 }
 // Sorts, projects, de-duplicates and pages materialized rows (documents, or join rows).
 std::string finish_rows(const sql::Select&s,const Params&params,long long limit,long long offset,std::vector<std::string> docs,const std::vector<std::string>*aliases){
  std::vector<std::string> rows;
  std::vector<size_t> idx(docs.size());for(size_t i=0;i<idx.size();i++)idx[i]=i;
  std::vector<Values> wv;if(select_has_window(s))wv=window_values(s,docs,params,aliases);
  auto vals=[&](size_t i)->const Values*{return wv.empty()?nullptr:&wv[i];};
  if(!s.order.empty()){
   std::vector<std::vector<json::Scalar>> keys(docs.size());
   for(size_t i=0;i<docs.size();i++){sql::Row row{docs[i],&params};row.aliases=aliases;row.aggregates=vals(i);for(const auto&o:s.order)keys[i].push_back(sql::eval(order_expr(s,o.e),row));}
   std::stable_sort(idx.begin(),idx.end(),[&](size_t a,size_t b){for(size_t k=0;k<s.order.size();k++){const auto&x=keys[a][k],&y=keys[b][k];if(scalar_less(x,y))return !s.order[k].desc;if(scalar_less(y,x))return s.order[k].desc;}return false;});
  }
  for(size_t i:idx)rows.push_back(project(s,docs[i],params,aliases,vals(i)));
  if(s.distinct)distinct_rows(rows);
  size_t from=std::min(rows.size(),size_t(offset)),to=limit<0?rows.size():std::min(rows.size(),from+size_t(limit));
  return rows_json(std::vector<std::string>(std::make_move_iterator(rows.begin()+std::ptrdiff_t(from)),std::make_move_iterator(rows.begin()+std::ptrdiff_t(to))));
 }
 // SUM and AVG add with Neumaier's compensated summation (as SQLite does): the result does
 // not depend on rounding errors that plain addition accumulates over many values.
 struct KSum {double s=0,c=0;void add(double x){double t=s+x;c+=std::fabs(s)>=std::fabs(x)?(s-t)+x:(x-t)+s;s=t;}double value()const{return s+c;}};
 struct Acc {long long count=0;KSum sum;bool any=false;json::Scalar best;std::unordered_set<std::string> seen;};
 static void collect_aggregates(const sql::Expr&e,std::vector<const sql::Expr*>&out){
  if(e.kind==sql::Expr::Func&&sql::is_aggregate(e.op)&&!e.over){out.push_back(&e);return;}
  for(const auto&a:e.args)collect_aggregates(a,out);
  if(e.over){for(const auto&p:e.over->partition)collect_aggregates(p,out);for(const auto&o:e.over->order)collect_aggregates(o.first,out);}  // RANK() OVER (ORDER BY COUNT(*))
 }
 static void collect_columns(const sql::Expr&e,std::vector<const sql::Expr*>&out){
  if(e.kind==sql::Expr::Col){out.push_back(&e);return;}
  for(const auto&a:e.args)collect_columns(a,out);
  if(e.over){for(const auto&p:e.over->partition)collect_columns(p,out);for(const auto&o:e.over->order)collect_columns(o.first,out);}
 }
 // Column values for one cache slot, for every column expression of a statement.
 struct SlotColumns:sql::ColumnSource {
  std::vector<std::pair<const sql::Expr*,const ColumnStore::Col*>> map;uint32_t slot=0;
  const json::Scalar*column(const sql::Expr&e)const override{for(const auto&[x,c]:map)if(x==&e)return &c->v[slot];return nullptr;}
 };
 // Rows for an aggregate: each call of emit gets the row (doc or cached columns) and,
 // for cached columns, the slot to come back to.
 using RowSink=std::function<void(const sql::Row&,std::string_view doc,uint32_t slot)>;
 std::string aggregate_select(const sql::Select&s,const Params&params,long long limit,long long offset,const std::function<void(const RowSink&)>&rows,SlotColumns*cols=nullptr,const std::vector<std::string>*aliases=nullptr){
  std::vector<const sql::Expr*> aggs;
  for(const auto&it:s.items){if(it.star)throw std::runtime_error("INPUT SQL: SELECT * cannot be combined with aggregates or GROUP BY");collect_aggregates(it.e,aggs);}
  if(s.having)collect_aggregates(*s.having,aggs);
  for(const auto&o:s.order)collect_aggregates(order_expr(s,o.e),aggs);
  for(const auto*a:aggs)if(a->args.size()!=1+size_t(a->filter))throw std::runtime_error("INPUT SQL: "+a->op+" takes one argument");
  struct Group {std::string doc;uint32_t slot=0;bool has_slot=false;std::vector<Acc> acc;};
  std::vector<Group> groups;std::unordered_map<std::string,size_t> where_is;
  bool single=s.group.empty();std::string gk;
  // Fast path: GROUP BY columns and aggregate arguments all in the column cache, no
  // DISTINCT. Values are read from the cached arrays directly, each aggregate's kind is
  // decided once, and a group whose key is one string is found by that string in place.
  enum Kind{K_COUNT_STAR,K_COUNT,K_SUM,K_AVG,K_MIN,K_MAX};
  auto col_of=[&](const sql::Expr&e)->const ColumnStore::Col*{if(!cols||e.kind!=sql::Expr::Col)return nullptr;for(const auto&[x,c]:cols->map)if(x==&e)return c;return nullptr;};
  bool fast=cols!=nullptr;std::vector<Kind> kinds;std::vector<const ColumnStore::Col*> argc,gcols;
  for(const auto*a:aggs){
   if(a->distinct||a->filter){fast=false;break;}
   Kind k=a->op=="COUNT"?(a->args[0].kind==sql::Expr::Star?K_COUNT_STAR:K_COUNT):a->op=="SUM"?K_SUM:a->op=="AVG"?K_AVG:a->op=="MIN"?K_MIN:a->op=="MAX"?K_MAX:K_COUNT;
   if(a->op!="COUNT"&&a->op!="SUM"&&a->op!="AVG"&&a->op!="MIN"&&a->op!="MAX"){fast=false;break;}
   const ColumnStore::Col*c=nullptr;if(k!=K_COUNT_STAR&&!(c=col_of(a->args[0]))){fast=false;break;}
   kinds.push_back(k);argc.push_back(c);
  }
  if(fast)for(const auto&g:s.group){auto*c=col_of(g);if(!c){fast=false;break;}gcols.push_back(c);}
  if(fast){
   std::unordered_map<std::string_view,size_t> by_text;  // views into the cache (stable during the query)
   rows([&](const sql::Row&,std::string_view doc,uint32_t slot){
    size_t gi;
    if(single){if(groups.empty())groups.push_back({std::string(doc),slot,true,std::vector<Acc>(aggs.size())});gi=0;}
    else if(gcols.size()==1&&gcols[0]->v[slot].tag==json::STRING){
     std::string_view key(gcols[0]->v[slot].str);auto it=by_text.find(key);
     if(it==by_text.end()){it=by_text.emplace(key,groups.size()).first;groups.push_back({std::string(doc),slot,true,std::vector<Acc>(aggs.size())});}
     gi=it->second;
    }else{
     gk.clear();gk.push_back('');for(const auto*c:gcols){const auto&v=c->v[slot];if(v.tag==json::MISSING)json::encode_into(gk,null_scalar());else json::encode_into(gk,v);}  // a missing value groups with NULL, as in SQL
     auto it=where_is.find(gk);
     if(it==where_is.end()){it=where_is.emplace(gk,groups.size()).first;groups.push_back({std::string(doc),slot,true,std::vector<Acc>(aggs.size())});}
     gi=it->second;
    }
    auto&grp=groups[gi];
    for(size_t k=0;k<kinds.size();k++){
     auto&acc=grp.acc[k];
     if(kinds[k]==K_COUNT_STAR){acc.count++;continue;}
     const auto&v=argc[k]->v[slot];if(v.tag==json::MISSING||v.tag==json::NUL)continue;
     switch(kinds[k]){
      case K_COUNT:acc.count++;break;
      case K_SUM:case K_AVG:if(v.tag!=json::NUMBER)throw std::runtime_error("INPUT SQL: "+aggs[k]->op+" needs numbers");acc.sum.add(v.num);acc.count++;acc.any=true;break;
      case K_MIN:if(!acc.any||scalar_less(v,acc.best)){acc.best=v;acc.any=true;}break;
      case K_MAX:if(!acc.any||scalar_less(acc.best,v)){acc.best=v;acc.any=true;}break;
      default:break;
     }
    }
   });
  }else
  rows([&](const sql::Row&row,std::string_view doc,uint32_t slot){
   size_t gi;
   if(single){if(groups.empty())groups.push_back({std::string(doc),slot,cols!=nullptr,std::vector<Acc>(aggs.size())});gi=0;}
   else{
    gk.clear();for(const auto&g:s.group){json::encode_into(gk,sql::eval(g,row));}
    auto it=where_is.find(gk);
    if(it==where_is.end()){it=where_is.emplace(gk,groups.size()).first;groups.push_back({std::string(doc),slot,cols!=nullptr,std::vector<Acc>(aggs.size())});}
    gi=it->second;
   }
   auto&grp=groups[gi];
   for(size_t k=0;k<aggs.size();k++){
    const auto&a=*aggs[k];auto&acc=grp.acc[k];
    if(a.filter&&!sql::truthy(sql::eval(a.args.back(),row)))continue;
    if(a.op=="COUNT"&&a.args[0].kind==sql::Expr::Star){acc.count++;continue;}
    auto v=sql::eval(a.args[0],row);if(sql::is_null(v))continue;
    if(a.distinct&&!acc.seen.insert(json::encode(v)).second)continue;
    if(a.op=="COUNT"){acc.count++;continue;}
    if(a.op=="SUM"||a.op=="AVG"){if(v.tag!=json::NUMBER)throw std::runtime_error("INPUT SQL: "+a.op+" needs numbers");acc.sum.add(v.num);acc.count++;acc.any=true;continue;}
    if(!acc.any||(a.op=="MIN"?scalar_less(v,acc.best):scalar_less(acc.best,v))){acc.best=v;acc.any=true;}
   }
  });
  if(groups.empty()&&single)groups.push_back({"{}",0,false,std::vector<Acc>(aggs.size())});
  struct Out {std::string text;std::vector<json::Scalar> sort;};std::vector<Out> outs;
  // Window functions run over the groups HAVING keeps, once every aggregate is known.
  const bool windows=select_has_window(s);struct Kept {size_t g;Values values;};std::vector<Kept> kept;
  for(auto&g:groups){
   std::unordered_map<const sql::Expr*,json::Scalar> values;
   for(size_t k=0;k<aggs.size();k++){
    const auto&a=*aggs[k];const auto&acc=g.acc[k];json::Scalar v;v.tag=json::NUL;
    if(a.op=="COUNT")v=sql::number(double(acc.count));
    else if(a.op=="SUM"){if(acc.any)v=sql::number(acc.sum.value());}
    else if(a.op=="AVG"){if(acc.count)v=sql::number(acc.sum.value()/double(acc.count));}
    else if(acc.any)v=acc.best;
    values[&a]=v;
   }
   sql::Row row{g.doc.empty()?std::string_view("{}"):std::string_view(g.doc),&params,&values};
   row.aliases=aliases;
   if(g.has_slot){cols->slot=g.slot;row.columns=cols;}
   if(s.having&&!sql::truthy(sql::eval(*s.having,row)))continue;
   if(windows){kept.push_back({size_t(&g-groups.data()),std::move(values)});continue;}
   Out o;o.text="{";
   for(size_t i=0;i<s.items.size();i++){if(i)o.text+=',';o.text+=json::quote(s.items[i].name);o.text+=':';o.text+=sql::to_json(sql::eval(s.items[i].e,row));}
   o.text+="}";
   for(const auto&ord:s.order)o.sort.push_back(sql::eval(order_expr(s,ord.e),row));
   outs.push_back(std::move(o));
  }
  if(windows){
   auto row_of=[&](size_t i){const auto&g=groups[kept[i].g];sql::Row row{g.doc.empty()?std::string_view("{}"):std::string_view(g.doc),&params,&kept[i].values};row.aliases=aliases;if(g.has_slot){cols->slot=g.slot;row.columns=cols;}return row;};
   auto wv=window_rows(s,kept.size(),params,[&](size_t i,const sql::Expr&x){return sql::eval(x,row_of(i));});
   for(size_t i=0;i<kept.size();i++){
    for(auto&[x,v]:wv[i])kept[i].values[x]=std::move(v);
    auto row=row_of(i);Out o;o.text="{";
    for(size_t k=0;k<s.items.size();k++){if(k)o.text+=',';o.text+=json::quote(s.items[k].name);o.text+=':';o.text+=sql::to_json(sql::eval(s.items[k].e,row));}
    o.text+="}";
    for(const auto&ord:s.order)o.sort.push_back(sql::eval(order_expr(s,ord.e),row));
    outs.push_back(std::move(o));
   }
  }
  if(!s.order.empty())std::stable_sort(outs.begin(),outs.end(),[&](const Out&a,const Out&b){for(size_t k=0;k<s.order.size();k++){if(scalar_less(a.sort[k],b.sort[k]))return !s.order[k].desc;if(scalar_less(b.sort[k],a.sort[k]))return s.order[k].desc;}return false;});
  std::vector<std::string> out;for(auto&o:outs)out.push_back(std::move(o.text));
  if(s.distinct)distinct_rows(out);
  size_t from=std::min(out.size(),size_t(offset)),to=limit<0?out.size():std::min(out.size(),from+size_t(limit));
  return rows_json(std::vector<std::string>(out.begin()+std::ptrdiff_t(from),out.begin()+std::ptrdiff_t(to)));
 }
 // Aggregates over the scan column cache, when the WHERE cannot use an index. Returns
 // nothing when the plan does not apply (the caller uses the regular path).
 std::optional<std::string> column_aggregate(const sql::Select&s,const Params&params,long long limit,long long offset){
  if(tl_nested()||sql::subquery_hook())return std::nullopt;  // correlated subqueries read whole rows
  if(auto*sn=tl_snap();sn&&sn->store==this&&sn->registered)return std::nullopt;  // the cache holds current values, not a snapshot's
  std::vector<const sql::Expr*> cexprs;
  for(const auto&it:s.items){if(it.star)return std::nullopt;collect_columns(it.e,cexprs);}
  if(s.where)collect_columns(*s.where,cexprs);
  for(const auto&g:s.group)collect_columns(g,cexprs);
  if(s.having)collect_columns(*s.having,cexprs);
  for(const auto&o:s.order)collect_columns(o.e,cexprs);
  if(cexprs.empty()&&!s.where)return std::nullopt;  // plain COUNT(*): the key count is faster
  query::Spec q=base_spec(s.table,s.where,params);bool exact=!q.residual;
  std::vector<std::vector<std::string>> paths;
  for(const auto*e:cexprs)if(std::find(paths.begin(),paths.end(),e->path)==paths.end())paths.push_back(e->path);
  for(const auto&c:q.where){if(c.is_key)return std::nullopt;if(std::find(paths.begin(),paths.end(),c.path)==paths.end())paths.push_back(c.path);}
  prebuild(s.table,false,&paths,&q.where);
  {
   char tok;GateHold gate(*this,gate_token(&tok));WriterLock w(*this);prepare_locked(s.table);require_table(s.table);
   if(index_narrows(s.table,q.where))return std::nullopt;
   if(!ensure_columns_locked(s.table,paths))return std::nullopt;
  }
  std::shared_lock<ReadWriteMutex>r(readers_);healthy();
  auto it=colstores_.find(s.table);if(it==colstores_.end())return std::nullopt;
  const ColumnStore&cs=*it->second;for(const auto&p:paths)if(!cs.find(p))return std::nullopt;
  SlotColumns sc;for(const auto*e:cexprs)sc.map.push_back({e,cs.find(e->path)});
  std::vector<std::pair<const query::Cond*,const ColumnStore::Col*>> conds;for(const auto&c:q.where)conds.push_back({&c,cs.find(c.path)});
  // A single COUNT(*) is answered from the conditions alone.
  bool count_only=s.group.empty()&&!s.having&&s.items.size()==1&&s.items[0].e.kind==sql::Expr::Func&&s.items[0].e.op=="COUNT"&&s.items[0].e.args.size()==1&&s.items[0].e.args[0].kind==sql::Expr::Star&&!s.items[0].e.over;
  sql::Row base{"{}",&params};base.columns=&sc;
  auto scan=[&](auto&&emit){
   for(uint32_t slot=0;slot<cs.ids.size();slot++){
    check_deadline();
    if(cs.ids[slot].empty())continue;
    bool ok=true;for(const auto&[c,col]:conds)if(!query::match(*c,col->v[slot])){ok=false;break;}
    if(!ok)continue;
    sc.slot=slot;
    if(!exact&&!sql::truthy(sql::eval(*s.where,base)))continue;
    emit(slot);
   }
  };
  if(count_only){
   long long n=0;scan([&](uint32_t){n++;});
   if(offset>0||limit==0)return std::string("{\"rows\":[]}");
   return "{\"rows\":[{"+json::quote(s.items[0].name)+":"+std::to_string(n)+"}]}";
  }
  return aggregate_select(s,params,limit,offset,[&](const RowSink&sink){scan([&](uint32_t slot){sink(base,std::string_view(),slot);});},&sc);
 }
 // Aggregates over documents (the WHERE can use an index, or the column cache is full).
 std::string doc_aggregate(const sql::Select&s,const Params&params,long long limit,long long offset){
  bool count_only=s.group.empty()&&!s.having&&s.items.size()==1&&s.items[0].e.kind==sql::Expr::Func&&s.items[0].e.op=="COUNT"&&s.items[0].e.args.size()==1&&s.items[0].e.args[0].kind==sql::Expr::Star&&!s.items[0].e.over;
  if(count_only&&!overlay_touches(s.table)){
   query::Spec q=base_spec(s.table,s.where,params);q.count=true;auto res=query_core(q);
   if(offset>0||limit==0)return "{\"rows\":[]}";
   return "{\"rows\":[{"+json::quote(s.items[0].name)+":"+std::to_string(res.count)+"}]}";
  }
  return aggregate_select(s,params,limit,offset,[&](const RowSink&sink){
   scan_rows(s.table,s.where,params,[&](std::string_view,uint64_t,std::string_view doc){sql::Row row{doc,&params};sink(row,doc,0);});
  });
 }
 static json::Json json_of(const json::Scalar&v){return json::parse(sql::to_json(v));}
 const std::vector<sql::ColumnDef>*schema_of(const std::string&t)const{auto it=columns_.find(t);return it==columns_.end()||it->second.empty()?nullptr:&it->second;}
 static const sql::ColumnDef*column_def(const std::vector<sql::ColumnDef>*schema,const std::string&name){if(schema)for(const auto&c:*schema)if(c.name==name)return &c;return nullptr;}
 // Declared types and NOT NULL. Undeclared columns accept any JSON value.
 static void check_value(const std::string&table,const sql::ColumnDef&c,const json::Scalar&v){
  if(sql::is_null(v)){if(c.not_null)throw std::runtime_error("INPUT SQL: "+table+"."+c.name+" cannot be NULL");return;}
  bool ok=true;
  if(c.type=="integer")ok=v.tag==json::NUMBER&&v.num==std::floor(v.num);
  else if(c.type=="number")ok=v.tag==json::NUMBER;
  else if(c.type=="text")ok=v.tag==json::STRING;
  else if(c.type=="boolean")ok=v.tag==json::TRUE_||v.tag==json::FALSE_;
  if(!ok)throw std::runtime_error("INPUT SQL: "+table+"."+c.name+" expects "+(c.type=="integer"?"an integer":c.type=="number"?"a number":c.type=="text"?"text":"a boolean")+", got "+sql::to_json(v));
  // VARCHAR(n) / CHAR(n): longer text fails, as in PostgreSQL (CHAR is not padded).
  if(c.length&&v.tag==json::STRING&&text_length(v.str)>c.length)throw std::runtime_error("INPUT SQL: "+table+"."+c.name+" holds at most "+std::to_string(c.length)+" characters, got "+std::to_string(text_length(v.str)));
 }
 static size_t text_length(const std::string&s){size_t n=0;for(unsigned char ch:s)if((ch&0xC0)!=0x80)n++;return n;}  // characters (UTF-8)
 static std::string apply_sets(std::string_view doc,const std::vector<std::pair<std::string,sql::Expr>>&sets,const Params&params,std::string_view excluded={},const std::vector<sql::ColumnDef>*schema=nullptr,const std::string&table=std::string()){
  sql::Row row{doc,&params};row.excluded=excluded;
  auto d=json::parse(doc);
  std::vector<std::pair<std::vector<std::string>,json::Json>> values;  // all computed from the old row
  for(const auto&[col,e]:sets){
   if(col=="id")throw std::runtime_error("INPUT SQL: id cannot be changed");
   auto v=sql::eval(e,row);if(const auto*c=column_def(schema,col))check_value(table,*c,v);
   values.emplace_back(json::split_path(col),json_of(v));
  }
  for(auto&[path,v]:values)json::set_path(d,path,std::move(v));
  return json::dump(d);
 }
 std::string run_sql(const sql::Insert&s,const Params&params){
  prebuild_write(s.table);char tok;GateHold gate(*this,gate_token(&tok));WriterLock w(*this);prepare_locked(s.table,true);require_table(s.table);
  if(s.cols.empty())throw std::runtime_error("INPUT SQL: INSERT needs a column list, e.g. INSERT INTO t (a, b) VALUES (?, ?)");
  long long seq=0;if(auto v=cur(seq_key(s.table)))seq=atoll(v->c_str());
  seq=std::max(seq,seq_floor(s.table));  // ids reserved by concurrent transactions in flight
  auto*cses=active_session();const bool reserve=cses&&cses->concurrent;
  long long seq0=seq;int id_col=-1;
  bool nested=false;std::vector<std::string> quoted;std::unordered_set<std::string> seen_cols;
  for(size_t i=0;i<s.cols.size();i++){
   require(seen_cols.insert(s.cols[i]).second,"INPUT SQL: column "+s.cols[i]+" appears twice");
   if(s.cols[i]=="id")id_col=int(i);
   if(s.cols[i].find('.')!=std::string::npos)nested=true;
   quoted.push_back(json::quote(s.cols[i]));
  }
  // Declared columns: types and NOT NULL are checked; omitted columns get their DEFAULT.
  const auto*schema=schema_of(s.table);std::vector<const sql::ColumnDef*> defs(s.cols.size(),nullptr),fill;
  if(schema){
   for(size_t i=0;i<s.cols.size();i++)defs[i]=column_def(schema,s.cols[i]);
   for(const auto&c:*schema){
    if(c.name=="id"||seen_cols.count(c.name))continue;
    if(c.has_default){fill.push_back(&c);if(c.name.find('.')!=std::string::npos)nested=true;}
    else if(c.not_null)throw std::runtime_error("INPUT SQL: "+s.table+"."+c.name+" is NOT NULL and has no DEFAULT; give it a value");
   }
  }
  std::vector<json::Scalar> vals;
  std::vector<Op> ops;std::unordered_set<std::string> keys;json::Scalar last;last.tag=json::NUL;size_t changes=0;
  for(const auto&row:s.rows){
   require(row.size()==s.cols.size(),"INPUT SQL: INSERT has "+std::to_string(s.cols.size())+" columns but "+std::to_string(row.size())+" values");
   sql::Row ctx{"{}",&params};json::Scalar id;
   if(id_col>=0){
    id=sql::eval(row[size_t(id_col)],ctx);
    bool integer=id.tag==json::NUMBER&&id.num==std::floor(id.num)&&std::fabs(id.num)<9e15;
    require(integer||(id.tag==json::STRING&&!id.str.empty()),"INPUT SQL: id must be an integer or non-empty text");
    if(integer&&id.num>double(seq))seq=(long long)id.num;
   }else id=sql::number(double(++seq));
   if(schema){
    vals.clear();for(size_t i=0;i<s.cols.size();i++)vals.push_back(int(i)==id_col?id:sql::eval(row[i],ctx));
    for(size_t i=0;i<s.cols.size();i++)if(defs[i])check_value(s.table,*defs[i],vals[i]);
   }
   auto value_of=[&](size_t i)->json::Scalar{
    if(schema)return vals[i];
    const auto&x=row[i];if(x.kind==sql::Expr::Param&&x.param<params.size())return params[x.param];
    return sql::eval(x,ctx);
   };
   std::string text;
   if(nested){
    json::Json doc;doc.kind=json::Json::Object;json::set_path(doc,{"id"},json_of(id));
    for(size_t i=0;i<s.cols.size();i++)if(int(i)!=id_col)json::set_path(doc,json::split_path(s.cols[i]),json_of(value_of(i)));
    for(const auto*c:fill)json::set_path(doc,json::split_path(c->name),json_of(c->def));
    text=json::dump(doc);
   }else{
    text="{\"id\":"+sql::to_json(id);
    for(size_t i=0;i<s.cols.size();i++)if(int(i)!=id_col){
     text+=',';text+=quoted[i];text+=':';
     const auto&x=row[i];
     if(!schema&&x.kind==sql::Expr::Param&&x.param<params.size())text+=sql::to_json(params[x.param]);  // no copy
     else text+=sql::to_json(value_of(i));
    }
    for(const auto*c:fill){text+=',';text+=json::quote(c->name);text+=':';text+=sql::to_json(c->def);}
    text+='}';
   }
   std::string key=row_key(s.table,id);
   if(id_col>=0)require(keys.insert(key).second,"CONFLICT duplicate id "+sql::to_json(id)+" in one INSERT");  // automatic ids are distinct
   // An automatic id is above every integer id ever stored, so it cannot exist yet.
   auto existing=id_col<0?std::optional<std::string>():cur(key);
   if(existing){
    if(s.on_conflict_nothing||s.ignore){if(auto*sn=tl_snap();sn&&sn->reads)note_reads(*sn->reads,s.table,nullptr,{{key,key+'\0'}},false);continue;}  // read, not written
    if(!s.on_conflict_set.empty()){ops.push_back({key,apply_sets(*existing,s.on_conflict_set,params,text,schema,s.table),ANY,false});changes++;last=id;continue;}
    if(!s.replace)throw std::runtime_error("CONFLICT duplicate id "+sql::to_json(id)+" in table "+s.table);
   }
   ops.push_back({key,std::move(text),ANY,false});changes++;last=id;
  }
  if(reserve&&seq!=seq0)seq_reserve(s.table,seq);
  if(seq!=seq0)ops.push_back({seq_key(s.table),std::to_string(seq),ANY,false});
  enforce_rules(ops);
  finish_write(ops);
  return "{\"changes\":"+std::to_string(changes)+",\"lastInsertId\":"+sql::to_json(last)+"}";
 }
 // Rows matching WHERE as (key, version, document), under the writer lock.
 // Rows matching WHERE as (key, version, document), as this statement sees them.
 std::vector<std::tuple<std::string,uint64_t,std::string>> matching_locked(const std::string&table,const std::optional<sql::Expr>&where,const Params&params){
  std::vector<std::tuple<std::string,uint64_t,std::string>> rows;std::string prefix=table+'\0';
  scan_rows(table,where,params,[&](std::string_view user,uint64_t version,std::string_view doc){rows.emplace_back(prefix+std::string(user),version,std::string(doc));});
  require(rows.size()<=65536,"LIMIT a statement can change at most 65,536 rows");
  return rows;
 }
 std::string run_sql(const sql::Update&s0,const Params&params){
  bool sub=s0.where&&sql::has_subquery(*s0.where);for(const auto&st:s0.sets)sub|=sql::has_subquery(st.second);
  if(sub){
   sql::Update c=s0;std::set<std::string> outer{s0.table},tables;bool left=false;
   if(c.where)resolve(*c.where,params,&outer,&left);for(auto&st:c.sets)resolve(st.second,params,&outer,&left);
   if(!left)return run_update(c,params);
   if(c.where)subquery_tables(*c.where,tables);for(const auto&st:c.sets)subquery_tables(st.second,tables);
   return with_correlated(outer,tables,params,[&]{return run_update(c,params);});
  }
  return run_update(s0,params);
 }
 std::string run_update(const sql::Update&s,const Params&params){
  prebuild_write(s.table);char tok;GateHold gate(*this,gate_token(&tok));WriterLock w(*this);prepare_locked(s.table,true);require_table(s.table);
  for(const auto&set:s.sets)require(set.first!="id","INPUT SQL: id cannot be changed");
  FtsScope ft;{std::vector<const sql::Expr*> roots{s.where?&*s.where:nullptr};for(const auto&st:s.sets)roots.push_back(&st.second);fulltext_prepare(ft,s.table,roots,params);}
  std::vector<Op> ops;
  for(auto&[key,version,doc]:matching_locked(s.table,s.where,params))ops.push_back({key,apply_sets(doc,s.sets,params,{},schema_of(s.table),s.table),version,false});
  size_t n=ops.size();
  enforce_rules(ops);
  finish_write(ops);
  return "{\"changes\":"+std::to_string(n)+"}";
 }
 std::string run_sql(const sql::Delete&s0,const Params&params){
  if(s0.where&&sql::has_subquery(*s0.where)){
   sql::Delete c=s0;std::set<std::string> outer{s0.table},tables;bool left=false;
   resolve(*c.where,params,&outer,&left);
   if(!left)return run_delete(c,params);
   subquery_tables(*c.where,tables);
   return with_correlated(outer,tables,params,[&]{return run_delete(c,params);});
  }
  return run_delete(s0,params);
 }
 std::string run_delete(const sql::Delete&s,const Params&params){
  prebuild_write(s.table);char tok;GateHold gate(*this,gate_token(&tok));WriterLock w(*this);prepare_locked(s.table,true);require_table(s.table);
  FtsScope ft;fulltext_prepare(ft,s.table,{s.where?&*s.where:nullptr},params);
  std::vector<Op> ops;
  for(auto&[key,version,doc]:matching_locked(s.table,s.where,params)){(void)doc;ops.push_back({key,"",version,true});}
  size_t n=ops.size();
  enforce_rules(ops);
  finish_write(ops);
  return "{\"changes\":"+std::to_string(n)+"}";
 }
 std::string run_sql(const sql::CreateTable&s,const Params&){
  no_transaction("CREATE TABLE");char tok;GateHold gate(*this,&tok);WriterLock w(*this);prepare_locked(s.table);
  if(table_exists(s.table)){if(s.if_not_exists)return "{\"changes\":0}";throw std::runtime_error("INPUT SQL: table "+s.table+" already exists");}
  sql::Constraints rules=s.rules;
  {std::unique_lock<ReadWriteMutex>r(readers_);catalog_epoch_++;tables_.push_back(s.table);columns_[s.table]=s.columns;}
  try{
   prepare_rules_locked(s.table,rules,sql::Constraints{},s.unique);
   if(!rules.empty()){std::unique_lock<ReadWriteMutex>r(readers_);catalog_epoch_++;rules_[s.table]=compile_rules(s.table,rules);}
   write_catalog_locked(nullptr,nullptr);
  }catch(...){std::unique_lock<ReadWriteMutex>r(readers_);catalog_epoch_++;tables_.pop_back();columns_.erase(s.table);rules_.erase(s.table);throw;}
  for(const auto&c:s.unique)create_index_locked(s.table,c,true,"");
  rule_indexes_locked(s.table,rules);
  for(const auto&f:s.fulltext)create_fulltext_sql(f,s.table);
  return "{\"changes\":0}";
 }
 std::string run_sql(const sql::DropTable&s,const Params&){
  no_transaction("DROP TABLE");char tok;GateHold gate(*this,&tok);WriterLock w(*this);prepare_locked(s.table);
  if(!table_exists(s.table)){if(s.if_exists)return "{\"changes\":0}";throw std::runtime_error("INPUT SQL: no such table: "+s.table);}
  for(const auto&[ct,r]:rules_)if(ct!=s.table)for(const auto&f:r.c.fks)if(f.table==s.table)
   throw std::runtime_error("INPUT SQL: table "+s.table+" is referenced by foreign key "+f.name+" of "+ct+"; drop that table or constraint first");
  std::string prefix=s.table+'\0',end=json::prefix_end(prefix);std::vector<std::string> keys;
  for(auto p=ordered_.seek(prefix);ordered_.valid(p);p=ordered_.next(p)){auto k=ordered_.get(p);if(k>=end)break;keys.emplace_back(k);}
  if(current_value(seq_key(s.table)))keys.push_back(seq_key(s.table));
  for(size_t i=0;i<keys.size();i+=50000){
   std::vector<Op> ops;for(size_t j=i;j<std::min(keys.size(),i+50000);j++)ops.push_back({keys[j],"",ANY,true});
   auto out=commit_group_locked({&ops},true);if(out[0].error)std::rethrow_exception(out[0].error);
  }
  {std::unique_lock<ReadWriteMutex>r(readers_);catalog_epoch_++;
   colstores_.erase(s.table);columns_.erase(s.table);rules_.erase(s.table);
   tables_.erase(std::find(tables_.begin(),tables_.end(),s.table));
   sec_.erase(std::remove_if(sec_.begin(),sec_.end(),[&](const auto&ix){return ix->collection==s.table;}),sec_.end());}
  write_catalog_locked(nullptr,nullptr);
  return "{\"changes\":"+std::to_string(keys.size())+"}";
 }
 std::string run_sql(const sql::CreateIndex&s,const Params&){
  no_transaction("CREATE INDEX");
  char tok;GateHold gate(*this,gate_token(&tok));WriterLock w(*this);prepare_locked(s.table);require_table(s.table);
  if(s.fulltext){create_fulltext_sql(s,s.table);return "{\"changes\":0}";}
  if(!s.more.empty()){
   std::vector<std::string> cols{s.column};std::string text=s.column;for(const auto&c:s.more){cols.push_back(c);text+=", "+c;}
   if(!create_composite_locked(s.table,cols,s.unique,s.name)&&!s.if_not_exists)throw std::runtime_error("INPUT SQL: an index on "+s.table+" ("+text+") already exists");
  }else
  if(!create_index_locked(s.table,s.column,s.unique,s.name)&&!s.if_not_exists)throw std::runtime_error("INPUT SQL: an index on "+s.table+"."+s.column+" already exists");
  return "{\"changes\":0}";
 }
 void create_fulltext_sql(const sql::CreateIndex&s,const std::string&table){
  std::vector<std::string> cols{s.column};std::string text=s.column;for(const auto&c:s.more){cols.push_back(c);text+=", "+c;}
  if(!create_fulltext_locked(table,cols,s.name,s.min_length)&&!s.if_not_exists)throw std::runtime_error("INPUT SQL: a FULLTEXT index on "+table+" ("+text+") already exists");
 }
 std::string run_sql(const sql::DropIndex&s,const Params&){
  no_transaction("DROP INDEX");char tok;GateHold gate(*this,&tok);std::lock_guard<std::mutex>w(writer_);
  if(!drop_index_locked(s.table,s.name)&&!s.if_exists)throw std::runtime_error("INPUT SQL: no such index: "+s.name);
  return "{\"changes\":0}";
 }
 // Rewrites every row of a table that change() modifies, in commits of up to 50,000 rows
 // (a very large table is therefore not changed atomically). Writer lock held.
 size_t rewrite_rows(const std::string&table,const std::function<bool(json::Json&)>&change){
  std::string prefix=table+'\0',end=json::prefix_end(prefix);std::vector<std::string> keys;
  for(auto p=ordered_.seek(prefix);ordered_.valid(p);p=ordered_.next(p)){auto k=ordered_.get(p);if(k>=end)break;keys.emplace_back(k);}
  size_t changed=0;std::vector<Op> ops;
  auto flush=[&]{if(ops.empty())return;auto out=commit_group_locked({&ops},true);if(out[0].error)std::rethrow_exception(out[0].error);ops.clear();};
  for(const auto&k:keys){
   auto v=current_value(k);if(!v)continue;
   auto d=json::parse(*v);if(!change(d))continue;
   ops.push_back({k,json::dump(d),ANY,false});changed++;
   if(ops.size()>=50000)flush();
  }
  flush();return changed;
 }
 // v converted to column c's type (ALTER COLUMN ... TYPE, MODIFY): text holding a number becomes
 // the number, numbers and booleans become text, text 'true'/'false' (or t, yes, on, 1, f, no,
 // off, 0, as in PostgreSQL) a boolean. False when it does not fit (or is longer than c.length).
 static bool convert_value(const sql::ColumnDef&c,json::Scalar&v){
  if(sql::is_null(v)||c.type=="any")return true;
  std::string t;
  if(v.tag==json::STRING){size_t a=v.str.find_first_not_of(" \t\r\n"),b=v.str.find_last_not_of(" \t\r\n");if(a!=std::string::npos)t=v.str.substr(a,b-a+1);}
  if(c.type=="integer"||c.type=="number"){
   if(v.tag==json::STRING){
    size_t i=0,digits=0;auto run=[&]{size_t k=0;while(i<t.size()&&t[i]>='0'&&t[i]<='9'){i++;k++;}return k;};
    if(i<t.size()&&(t[i]=='+'||t[i]=='-'))i++;
    digits+=run();
    if(c.type=="number"){
     if(i<t.size()&&t[i]=='.'){i++;digits+=run();}
     if(digits&&i<t.size()&&(t[i]=='e'||t[i]=='E')){i++;if(i<t.size()&&(t[i]=='+'||t[i]=='-'))i++;if(!run())return false;}
    }
    if(!digits||i!=t.size())return false;
    double x=strtod(t.c_str(),nullptr);
    if(!std::isfinite(x)||(c.type=="integer"&&std::fabs(x)>=9007199254740992.0))return false;  // beyond 2^53 it would not be exact
    v=sql::number(x==0?0.0:x);return true;
   }
   return v.tag==json::NUMBER&&(c.type=="number"||v.num==std::floor(v.num));
  }
  if(c.type=="boolean"){
   if(v.tag==json::TRUE_||v.tag==json::FALSE_)return true;
   if(v.tag!=json::STRING)return false;
   for(auto&ch:t)if(ch>='A'&&ch<='Z')ch=char(ch+32);
   static const char*yes[]={"true","t","yes","y","on","1"},*no[]={"false","f","no","n","off","0"};
   for(auto w:yes)if(t==w){v=json::Scalar{};v.tag=json::TRUE_;return true;}
   for(auto w:no)if(t==w){v=json::Scalar{};v.tag=json::FALSE_;return true;}
   return false;
  }
  // text
  if(v.tag==json::NUMBER){
   std::string s;
   if(std::fabs(v.num)<9007199254740992.0&&v.num==std::floor(v.num))s=std::to_string((long long)v.num);
   else{char b[40];for(int p=15;p<=17;p++){snprintf(b,sizeof b,"%.*g",p,v.num);if(strtod(b,nullptr)==v.num)break;}s=b;}  // the shortest text that reads back the same
   v=json::Scalar{};v.tag=json::STRING;v.str=std::move(s);
  }else if(v.tag==json::TRUE_||v.tag==json::FALSE_){bool b=v.tag==json::TRUE_;v=json::Scalar{};v.tag=json::STRING;v.str=b?"true":"false";}
  else if(v.tag!=json::STRING)return false;  // objects and arrays
  return !c.length||text_length(v.str)<=c.length;
 }
 // ALTER [COLUMN] c SET/DROP DEFAULT, SET/DROP NOT NULL, TYPE, and MySQL's MODIFY, on the declared
 // column c (updated here; the caller publishes the schema). SET NOT NULL, TYPE and MODIFY check
 // every row first and fail without changing anything when a value is NULL or does not convert;
 // then the rows whose value changes are rewritten in commits of up to 50,000 rows. Writer lock held.
 size_t alter_column_locked(const std::string&table,sql::ColumnDef&c,const sql::AlterTable&s){
  using A=sql::AlterTable;
  sql::ColumnDef n=c;
  switch(s.action){
   case A::SetDefault:n.has_default=s.column.has_default;n.def=s.column.def;break;
   case A::DropDefault:n.has_default=false;n.def=json::Scalar{};break;
   case A::SetNotNull:n.not_null=true;break;
   case A::DropNotNull:n.not_null=false;break;
   case A::SetType:n.type=s.column.type;n.length=s.column.length;break;
   default:n=s.column;n.unique=false;n.rules=sql::Constraints{};break;  // MODIFY replaces the whole definition
  }
  const bool retype=s.action==A::SetType||s.action==A::Modify,scan=retype||s.action==A::SetNotNull;
  const std::string what=table+"."+c.name,type=n.type+(n.length?"("+std::to_string(n.length)+")":std::string());
  bool unique=false,plain_index=false;for(const auto&ix:sec_)if(ix->collection==table&&ix->field==c.name&&!ix->fulltext){if(ix->unique)unique=true;else plain_index=true;}
  // MODIFY c ... UNIQUE adds a unique index, after the rows are checked for repeats below.
  const bool add_unique=s.action==A::Modify&&s.column.unique&&!unique;
  if(add_unique&&plain_index)throw std::runtime_error("INPUT SQL: "+what+" has an index that is not UNIQUE; drop it first");
  if(n.has_default){
   if(s.action==A::SetType){json::Scalar d=n.def;if(!convert_value(n,d))throw std::runtime_error("INPUT SQL: the DEFAULT "+sql::to_json(n.def)+" of "+what+" does not convert to "+type+"; drop or change the default first");n.def=d;}
   else{auto any_null=n;any_null.not_null=false;check_value(table,any_null,n.def);}
  }
  if(n.not_null)if(const auto*r=rules_for(table))for(const auto&f:r->c.fks)if(f.on_delete==sql::ForeignKey::SetNull||f.on_update==sql::ForeignKey::SetNull)
   for(const auto&x:f.cols)if(x==c.name)throw std::runtime_error("INPUT SQL: foreign key "+f.name+" sets "+c.name+" to NULL, so it cannot be NOT NULL");
  // A foreign key compares values strictly (5 is not '5'), so its columns keep their type.
  if(n.type!=c.type)for(const auto&[ct,r]:rules_)for(const auto&f:r.c.fks){
   bool hit=false;if(ct==table)for(const auto&x:f.cols)hit|=x==c.name;if(f.table==table)for(const auto&x:f.ref)hit|=x==c.name;
   if(hit)throw std::runtime_error("INPUT SQL: column "+c.name+" is used by foreign key "+f.name+(ct==table?std::string():" (of "+ct+")")+"; drop the constraint to change its type");
  }
  auto path=json::split_path(c.name);
  auto convert=[&](json::Json&d){
   const auto*f=json::find_dom(d,path);if(!f)return false;
   auto v=f->scalar(),x=v;if(!convert_value(n,x)||(x.tag==v.tag&&x.num==v.num&&x.str==v.str))return false;
   json::set_path(d,path,json_of(x));return true;
  };
  if(scan){
   std::string err;std::unordered_set<std::string> seen;
   scan_rows(table,std::nullopt,Params{},[&](std::string_view,uint64_t,std::string_view doc){
    if(!err.empty())return;
    auto v=json::field(doc,path);
    auto row=[&]{return "row id "+sql::to_json(json::field(doc,{"id"}));};
    if(sql::is_null(v)){if(n.not_null)err="CONFLICT SQL: "+what+" is NULL in "+row()+"; give every row a value before making it NOT NULL";return;}
    if(!retype)return;
    auto x=v;
    if(!convert_value(n,x)){err="CONFLICT SQL: "+what+" of "+row()+" is "+sql::to_json(v)+", which does not fit "+type+"; change or delete that row first";return;}
    if((unique||add_unique)&&!seen.insert(json::encode(x)).second)err="CONFLICT SQL: "+what+" would repeat "+sql::to_json(x)+", but it is UNIQUE";
   });
   if(!err.empty())throw std::runtime_error(err);
   // CHECK and multi-column UNIQUE constraints must still hold for the converted values.
   if(retype)if(const auto*r=rules_for(table)){
    sql::Constraints sub=r->c;sub.fks.clear();
    if(!sub.empty())check_existing_rows_locked(table,compile_rules(table,sub),[&](std::string_view doc){auto d=json::parse(doc);convert(d);return json::dump(d);});
   }
  }
  size_t changed=retype?rewrite_rows(table,convert):0;
  if(add_unique)create_index_locked(table,c.name,true,"");
  c=std::move(n);return changed;
 }
 std::string run_sql(const sql::AlterTable&s,const Params&){
  no_transaction("ALTER TABLE");char tok;GateHold gate(*this,&tok);WriterLock w(*this);prepare_locked(s.table);
  if(!table_exists(s.table)){if(s.if_exists)return "{\"changes\":0}";throw std::runtime_error("INPUT SQL: no such table: "+s.table);}
  auto cols=columns_[s.table];size_t changed=0;
  auto find=[&](const std::string&n){return std::find_if(cols.begin(),cols.end(),[&](const sql::ColumnDef&c){return c.name==n;});};
  auto publish=[&]{{std::unique_lock<ReadWriteMutex>r(readers_);catalog_epoch_++;columns_[s.table]=cols;}write_catalog_locked(nullptr,nullptr);};
  sql::Constraints have;if(const auto*r=rules_for(s.table))have=r->c;
  std::vector<std::string> ucols;for(const auto&ix:sec_)if(ix->collection==s.table&&ix->unique)ucols.push_back(ix->field);
  auto merged=[&](const sql::Constraints&add){sql::Constraints m=have;for(const auto&x:add.checks)m.checks.push_back(x);for(const auto&x:add.fks)m.fks.push_back(x);for(const auto&x:add.unique)m.unique.push_back(x);return m;};
  // A column a constraint uses (dropping or renaming it).
  auto uses=[&](const std::string&col)->std::string{
   std::function<bool(const sql::Expr&)> refers=[&](const sql::Expr&e){if(e.kind==sql::Expr::Col&&!e.path.empty()&&(e.path[0]==col||json::split_path(col)==e.path))return true;for(const auto&a:e.args)if(refers(a))return true;return false;};
   if(const auto*r=rules_for(s.table)){
    for(size_t k=0;k<r->checks.size();k++)if(refers(r->checks[k]))return r->c.checks[k].name;
    for(const auto&f:r->c.fks)for(const auto&c:f.cols)if(c==col)return f.name;
    for(const auto&u:r->c.unique)for(const auto&c:u.cols)if(c==col)return u.name;
   }
   for(const auto&[ct,r]:rules_)for(const auto&f:r.c.fks)if(f.table==s.table)for(const auto&c:f.ref)if(c==col)return f.name+" (of "+ct+")";
   return "";
  };
  if(s.action==sql::AlterTable::AddColumn){
   const auto&c=s.column;
   if(find(c.name)!=cols.end()){if(s.if_not_exists)return "{\"changes\":0}";throw std::runtime_error("INPUT SQL: column "+c.name+" already exists in "+s.table);}
   auto path=json::split_path(c.name);
   bool any_row=false;std::string prefix=s.table+'\0';{auto p=ordered_.seek(prefix);any_row=ordered_.valid(p)&&ordered_.get(p).compare(0,prefix.size(),prefix)==0;}
   if(c.not_null&&!c.has_default&&any_row)throw std::runtime_error("INPUT SQL: a NOT NULL column needs a DEFAULT when the table has rows");
   sql::Constraints add=s.rules;
   if(!add.empty()){
    auto u=ucols;if(c.unique)u.push_back(c.name);
    {std::unique_lock<ReadWriteMutex>r(readers_);catalog_epoch_++;columns_[s.table].push_back(c);}  // NOT NULL checks of SET NULL actions
    try{prepare_rules_locked(s.table,add,have,u);}catch(...){std::unique_lock<ReadWriteMutex>r(readers_);catalog_epoch_++;columns_[s.table].pop_back();throw;}
    {std::unique_lock<ReadWriteMutex>r(readers_);catalog_epoch_++;columns_[s.table].pop_back();}
    if(any_row)check_existing_rows_locked(s.table,compile_rules(s.table,add),[&](std::string_view doc){auto d=json::parse(doc);if(!json::find_dom(d,path))json::set_path(d,path,json_of(c.has_default?c.def:json::Scalar{}));return json::dump(d);});
   }
   if(c.has_default)changed=rewrite_rows(s.table,[&](json::Json&d){if(json::find_dom(d,path))return false;json::set_path(d,path,json_of(c.def));return true;});
   cols.push_back(c);publish();
   if(c.unique)create_index_locked(s.table,c.name,true,"");
   if(!add.empty()){publish_rules_locked(s.table,merged(add));rule_indexes_locked(s.table,add);}
  }else if(s.action==sql::AlterTable::AddConstraint&&!s.fulltext.empty()){
   for(const auto&f:s.fulltext)create_fulltext_sql(f,s.table);
  }else if(s.action==sql::AlterTable::AddConstraint){
   sql::Constraints add=s.rules;auto u=ucols;for(const auto&c:s.unique)u.push_back(c);
   prepare_rules_locked(s.table,add,have,u);
   for(const auto&c:s.unique)if(!create_index_locked(s.table,c,true,""))throw std::runtime_error("INPUT SQL: "+s.table+"."+c+" is already UNIQUE");
   if(!add.empty()){
    check_existing_rows_locked(s.table,compile_rules(s.table,add),nullptr);
    publish_rules_locked(s.table,merged(add));rule_indexes_locked(s.table,add);
   }
  }else if(s.action==sql::AlterTable::DropConstraint){
   sql::Constraints m=have;bool gone=false;
   auto drop=[&](auto&v){auto e=std::remove_if(v.begin(),v.end(),[&](const auto&x){return x.name==s.from;});gone|=e!=v.end();v.erase(e,v.end());};
   drop(m.checks);drop(m.fks);drop(m.unique);
   if(gone)publish_rules_locked(s.table,std::move(m));
   else if(!drop_index_locked(s.table,s.from)&&!s.if_exists)throw std::runtime_error("INPUT SQL: no constraint or index "+s.from+" on "+s.table);
  }else if(s.action==sql::AlterTable::RenameTable){
   const auto&to=s.to;
   if(to==s.table)return "{\"changes\":0}";
   if(table_exists(to))throw std::runtime_error("INPUT SQL: table "+to+" already exists");
   // Indexes are made again under the new name; rows move in commits of up to 50,000 rows
   // (rerun the statement if it was interrupted).
   std::vector<IndexDef> idx;for(const auto&ix:sec_)if(ix->collection==s.table)idx.push_back(index_def(*ix));
   for(const auto&d:idx)drop_index_locked(s.table,d.name);
   std::string prefix=s.table+'\0',end=json::prefix_end(prefix);std::vector<std::string> keys;
   for(auto p=ordered_.seek(prefix);ordered_.valid(p);p=ordered_.next(p)){auto k=ordered_.get(p);if(k>=end)break;keys.emplace_back(k);}
   std::vector<Op> ops;auto flush=[&]{if(ops.empty())return;auto out=commit_group_locked({&ops},true);if(out[0].error)std::rethrow_exception(out[0].error);ops.clear();};
   for(const auto&k:keys){auto v=current_value(k);if(!v)continue;ops.push_back({to+k.substr(s.table.size()),std::move(*v),ANY,false});ops.push_back({k,"",ANY,true});changed++;if(ops.size()>=50000)flush();}
   if(auto v=current_value(seq_key(s.table))){ops.push_back({seq_key(to),*v,ANY,false});ops.push_back({seq_key(s.table),"",ANY,true});}
   flush();
   {std::unique_lock<ReadWriteMutex>r(readers_);catalog_epoch_++;
    *std::find(tables_.begin(),tables_.end(),s.table)=to;
    if(auto it=columns_.find(s.table);it!=columns_.end()){auto c=std::move(it->second);columns_.erase(it);columns_[to]=std::move(c);}
    colstores_.erase(s.table);
    if(auto it=rules_.find(s.table);it!=rules_.end()){
     auto c=std::move(it->second.c);rules_.erase(it);
     for(auto&k:c.checks)k.text=sql::rename_identifier(k.text,s.table,to,true);
     for(auto&f:c.fks)if(f.table==s.table)f.table=to;
     rules_[to]=compile_rules(to,std::move(c));
    }
    for(auto&[ct,r]:rules_)for(auto&f:r.c.fks)if(f.table==s.table)f.table=to;
   }
   write_catalog_locked(nullptr,nullptr);
   for(const auto&d:idx)recreate_index_locked(s.table,to,d,d.cols);
  }else if(s.action==sql::AlterTable::DropColumn){
   if(auto n=uses(s.from);!n.empty())throw std::runtime_error("INPUT SQL: column "+s.from+" is used by constraint "+n+"; drop the constraint first");
   auto it=find(s.from);bool declared=it!=cols.end();
   auto path=json::split_path(s.from);
   // A FULLTEXT index on the column and others is made again without it (as in MySQL).
   std::vector<std::string> doomed;std::vector<IndexDef> shrunk;
   for(const auto&ix:sec_)if(ix->collection==s.table&&((ix->field==s.from&&!ix->fulltext)||std::count(ix->cols.begin(),ix->cols.end(),s.from))){doomed.push_back(ix->name);if(ix->fulltext&&ix->cols.size()>1)shrunk.push_back(index_def(*ix));}
   for(const auto&n:doomed)drop_index_locked(s.table,n);
   changed=rewrite_rows(s.table,[&](json::Json&d){return json::erase_path(d,path);});
   for(const auto&d:shrunk){auto c=d.cols;c.erase(std::find(c.begin(),c.end(),s.from));recreate_index_locked(s.table,s.table,d,c);}
   if(declared){cols.erase(it);publish();}
   else if(!changed&&!s.if_exists&&doomed.empty())throw std::runtime_error("INPUT SQL: no column "+s.from+" in "+s.table);
  }else if(s.action>=sql::AlterTable::SetDefault){
   auto it=find(s.from);
   if(it==cols.end())throw std::runtime_error("INPUT SQL: no declared column "+s.from+" in "+s.table+" (ALTER COLUMN and MODIFY change columns of CREATE TABLE or ADD COLUMN)");
   changed=alter_column_locked(s.table,*it,s);publish();
  }else{
   if(find(s.to)!=cols.end())throw std::runtime_error("INPUT SQL: column "+s.to+" already exists in "+s.table);
   bool constrained=!uses(s.from).empty();
   if(constrained&&(s.from.find('.')!=std::string::npos||s.to.find('.')!=std::string::npos))throw std::runtime_error("INPUT SQL: column "+s.from+" is used by constraint "+uses(s.from)+"; drop the constraint first");
   auto from=json::split_path(s.from),to=json::split_path(s.to);
   std::vector<std::pair<std::string,bool>> moved;for(const auto&ix:sec_)if(ix->collection==s.table&&ix->field==s.from&&!ix->fulltext)moved.push_back({ix->name,ix->unique});
   for(const auto&m:moved)drop_index_locked(s.table,m.first);
   // Composite indexes with the column: made again with the new name in its place.
   std::vector<IndexDef> remade;
   for(const auto&ix:sec_)if(ix->collection==s.table&&std::count(ix->cols.begin(),ix->cols.end(),s.from))remade.push_back(index_def(*ix));
   for(const auto&m:remade)drop_index_locked(s.table,m.name);
   changed=rewrite_rows(s.table,[&](json::Json&d){
    if(from.size()==1&&to.size()==1&&d.kind==json::Json::Object){  // in place, so SELECT * keeps the column order
     auto at=[&](const std::string&n){return std::find_if(d.fields.begin(),d.fields.end(),[&](const auto&f){return f.first==n;});};
     if(at(from[0])==d.fields.end())return false;
     if(auto t=at(to[0]);t!=d.fields.end())d.fields.erase(t);
     at(from[0])->first=to[0];return true;
    }
    const auto*v=json::find_dom(d,from);if(!v)return false;json::Json copy=*v;json::erase_path(d,from);json::set_path(d,to,std::move(copy));return true;});
   bool declared=false;if(auto it=find(s.from);it!=cols.end()){it->name=s.to;publish();declared=true;}
   if(!declared&&!changed&&moved.empty()&&remade.empty()&&!constrained)throw std::runtime_error("INPUT SQL: no column "+s.from+" in "+s.table);
   // An index keeps its name unless it had the automatic one (table_column_idx).
   for(const auto&m:moved)create_index_locked(s.table,s.to,m.second,m.first==s.table+"_"+s.from+"_idx"?std::string():m.first);
   for(const auto&d:remade){auto c=d.cols;for(auto&x:c)if(x==s.from)x=s.to;recreate_index_locked(s.table,s.table,d,c);}
   if(constrained){
    {std::unique_lock<ReadWriteMutex>r(readers_);catalog_epoch_++;
     for(auto&[ct,r]:rules_){
      auto c=r.c;
      if(ct==s.table){
       for(auto&k:c.checks)k.text=sql::rename_identifier(k.text,s.from,s.to,false);
       for(auto&f:c.fks)for(auto&x:f.cols)if(x==s.from)x=s.to;
       for(auto&u:c.unique)for(auto&x:u.cols)if(x==s.from)x=s.to;
      }
      for(auto&f:c.fks)if(f.table==s.table)for(auto&x:f.ref)if(x==s.from)x=s.to;
      r=compile_rules(ct,std::move(c));
     }}
    write_catalog_locked(nullptr,nullptr);
   }
  }
  return "{\"changes\":"+std::to_string(changed)+"}";
 }
 // ---------- Catalog statements ----------
 // SHOW TABLES: every SQL table with its row count.
 std::string run_sql(const sql::ShowTables&,const Params&){
  prebuild(std::string());{WriterLock w(*this);prepare_locked(std::string());}
  std::shared_lock<ReadWriteMutex>r(readers_);healthy();
  std::vector<std::string> names=tables_;std::sort(names.begin(),names.end());
  std::vector<std::string> rows;
  for(const auto&t:names){
   std::string prefix=t+'\0',end=json::prefix_end(prefix);size_t n=0;
   for(auto p=ordered_.seek(prefix);ordered_.valid(p)&&ordered_.get(p)<end;p=ordered_.next(p))n++;
   size_t cols=0;if(auto it=columns_.find(t);it!=columns_.end())cols=it->second.size();
   size_t idx=0;for(const auto&ix:sec_)if(ix->collection==t)idx++;
   rows.push_back("{\"table\":"+json::quote(t)+",\"rows\":"+std::to_string(n)+",\"columns\":"+std::to_string(cols)+",\"indexes\":"+std::to_string(idx)+"}");
  }
  return rows_json(rows);
 }
 // DESCRIBE t: the declared columns (an undeclared table lists only id).
 std::string run_sql(const sql::Describe&s,const Params&){
  prebuild(s.table);{WriterLock w(*this);prepare_locked(s.table);}
  std::shared_lock<ReadWriteMutex>r(readers_);healthy();require_table(s.table);
  std::vector<std::string> rows;bool has_id=false;
  auto row=[&](const sql::ColumnDef&c,bool key){
   std::string out="{\"column\":"+json::quote(c.name)+",\"type\":"+json::quote(c.type)+",\"notNull\":"+((c.not_null||key)?"true":"false")+",\"key\":"+(key?"\"PRIMARY\"":"null");
   bool uniq=false;for(const auto&ix:sec_)if(ix->collection==s.table&&ix->field==c.name&&ix->unique)uniq=true;
   if(uniq&&!key)out=out.substr(0,out.size()-4)+"\"UNIQUE\"";
   out+=",\"default\":"+(c.has_default?sql::to_json(c.def):std::string("null"))+"}";
   rows.push_back(out);
  };
  if(auto it=columns_.find(s.table);it!=columns_.end())for(const auto&c:it->second){if(c.name=="id")has_id=true;row(c,c.name=="id");}
  if(!has_id){sql::ColumnDef id;id.name="id";id.type="any";rows.insert(rows.begin(),std::string());row(id,true);rows.front()=rows.back();rows.pop_back();}
  return rows_json(rows);
 }
 // SHOW INDEX FROM t.
 std::string run_sql(const sql::ShowIndexes&s,const Params&){
  prebuild(s.table);{WriterLock w(*this);prepare_locked(s.table);}
  std::shared_lock<ReadWriteMutex>r(readers_);healthy();require_table(s.table);
  std::vector<std::string> rows;
  rows.push_back("{\"name\":\"PRIMARY\",\"column\":\"id\",\"columns\":[\"id\"],\"unique\":true,\"entries\":null,\"type\":\"BTREE\"}");
  for(const auto&ix:sec_)if(ix->collection==s.table)
   rows.push_back("{\"name\":"+json::quote(ix->name)+",\"column\":"+json::quote(ix->field)+",\"columns\":"+names_json(index_columns(*ix))+",\"unique\":"+(ix->unique?"true":"false")+",\"entries\":"+std::to_string(ix->set.size())+",\"type\":"+(ix->fulltext?"\"FULLTEXT\"":"\"BTREE\"")+"}");
  return rows_json(rows);
 }
 std::string run_sql(const sql::Begin&b,const Params&){
  auto*ses=tl_session();
  if(!ses)throw std::runtime_error("UNSUPPORTED SQL: transactions need a connection (Database, RemoteDB, a server client or a C library handle)");
  if(ses->active)throw std::runtime_error("INPUT SQL: a transaction is already open");
  if(b.concurrent){
   // Optimistic: no write gate until COMMIT; reads see the state as of now (a registered
   // snapshot); COMMIT fails with CONFLICT if another commit changed a row written here
   // or, serializable (the default), a row read here (store_mvcc.inc).
   std::shared_lock<ReadWriteMutex>r(readers_);healthy();
   ses->snap=Snap{};ses->snap.store=this;ses->snap.txn=true;ses->snap.end=meta_.end;ses->snap.epoch=catalog_epoch_;register_snapshot(ses->snap);
   ses->reads=ReadSet{};if(!b.snapshot)ses->snap.reads=&ses->reads;
   ses->concurrent=true;
  }else{gate_enter(ses);ses->concurrent=false;}
  ses->active=true;ses->overlay.clear();ses->unique.clear();ses->saves.clear();ses->bytes=0;
  return "{\"changes\":0}";
 }
 std::string run_sql(const sql::Commit&,const Params&){
  auto*ses=active_session();if(!ses)throw std::runtime_error("INPUT SQL: no transaction is open");
  size_t n=0;
  try{
   std::optional<GateHold> gate;if(ses->concurrent)gate.emplace(*this,static_cast<const void*>(ses));  // the gate only now
   struct NoSnap{Snap*p;NoSnap():p(tl_snap()){tl_snap()=nullptr;}~NoSnap(){tl_snap()=p;}} current_state;  // checks read the rows as they are now
   std::lock_guard<std::mutex>w(writer_);
   if(ses->concurrent)validate_concurrent_locked(*ses);
   std::vector<Op> ops;ops.reserve(ses->overlay.size());
   for(auto&[k,v]:ses->overlay){
    if(v)ops.push_back({k,std::move(*v),ANY,false});
    else if(current_value(k))ops.push_back({k,"",ANY,true});  // rows created and deleted in the transaction leave no trace
   }
   if(ses->concurrent){
    // Sequences: the larger of the committed value and this transaction's (both moved on).
    for(auto&o:ops)if(!o.erase&&o.key.size()>5&&!o.key.compare(0,5,std::string("\x01seq\0",5))){auto c=current_value(o.key);if(c&&atoll(c->c_str())>atoll(o.value.c_str()))o.value=*c;}
    // Constraints again, against the rows as they are now (another transaction may have
    // removed a parent or added a child meanwhile); cascades reach rows committed since.
    SessionScope none(nullptr);enforce_rules(ops);
   }
   for(const auto&o:ops)if(o.key.empty()||uint8_t(o.key[0])!=1)n++;  // rows (not the engine's own records)
   if(!ops.empty()){auto out=commit_group_locked({&ops},true);if(out[0].error)std::rethrow_exception(out[0].error);}
   for(auto&x:ses->notes)x.txid=meta_.txid;emit_locked(ses->notes);  // NOTIFYs: after the commit, before another
  }catch(...){end_session(*ses);throw;}
  end_session(*ses);
  return "{\"changes\":"+std::to_string(n)+"}";
 }
 // BEGIN CONCURRENT at COMMIT (gate and writer lock held): every row this transaction writes
 // must be as it was when it began and, serializable, every row it read. One that writes
 // nothing reads one committed state: it always commits.
 void validate_concurrent_locked(SqlSession&ses){
  std::shared_lock<ReadWriteMutex>r(readers_);healthy();
  auto again=[](const std::string&why){return std::runtime_error("CONFLICT "+why+"; the transaction was rolled back: run it again");};
  if(ses.snap.epoch!=catalog_epoch_)throw again("the schema changed during this transaction");
  std::lock_guard<std::mutex>u(undo_m_);
  for(const auto&[k,v]:ses.overlay){
   if(k.size()>5&&!k.compare(0,5,std::string("\x01seq\0",5)))continue;  // merged instead
   if(undo_at(k,ses.snap.end)){
    auto t=collection_of(k);
    throw again("another transaction changed "+(t.empty()?std::string("a key"):"a row of "+std::string(t))+" that this one writes (id "+(t.empty()?std::string(k):id_text(std::string_view(k).substr(t.size()+1)))+")");
   }
  }
  if(ses.snap.reads&&!ses.overlay.empty())validate_reads_locked(*ses.snap.reads,ses.snap.end,again);
 }
 std::string run_sql(const sql::Rollback&,const Params&){auto*ses=tl_session();if(ses&&ses->active)end_session(*ses);return "{\"changes\":0}";}
 // SAVEPOINT name: a point to return to inside the transaction (outside one, it begins one
 // that RELEASE of this savepoint commits, as in SQLite). ROLLBACK TO keeps the savepoint.
 std::string run_sql(const sql::Savepoint&s,const Params&p){
  auto*ses=tl_session();bool began=false;
  if(!ses||!ses->active){run_sql(sql::Begin{},p);ses=tl_session();began=true;}
  require(ses->saves.size()<1000,"LIMIT at most 1,000 savepoints");
  // Each savepoint keeps a copy of the transaction's changes: they count against its memory.
  size_t bytes=0;for(const auto&[k,v]:ses->overlay)bytes+=k.size()+(v?v->size():0)+64;
  require(bytes*(ses->saves.size()+2)<=MAX_TRANSACTION_BYTES,"LIMIT the transaction and its savepoints would use more than "+std::to_string(MAX_TRANSACTION_BYTES>>20)+" MB; commit sooner or use fewer savepoints");
  ses->saves.push_back({s.name,ses->overlay,ses->unique,began,ses->notes.size()});
  return "{\"changes\":0}";
 }
 size_t savepoint_index(const std::string&name){
  auto*ses=active_session();if(!ses)throw std::runtime_error("INPUT SQL: no transaction is open");
  for(size_t i=ses->saves.size();i-->0;)if(ses->saves[i].name==name)return i;
  throw std::runtime_error("INPUT SQL: no such savepoint: "+name);
 }
 std::string run_sql(const sql::Release&s,const Params&p){
  size_t i=savepoint_index(s.name);auto*ses=active_session();bool began=ses->saves[i].began;
  ses->saves.resize(i);
  if(began)return run_sql(sql::Commit{},p);
  return "{\"changes\":0}";
 }
 std::string run_sql(const sql::RollbackTo&s,const Params&){
  size_t i=savepoint_index(s.name);auto*ses=active_session();auto&sp=ses->saves[i];
  ses->overlay=sp.overlay;ses->unique=sp.unique;
  if(ses->notes.size()>sp.notes){ses->notes.resize(sp.notes);ses->noted.clear();for(const auto&x:ses->notes)ses->noted.emplace(x.channel,x.payload);}
  ses->saves.resize(i+1);
  return "{\"changes\":0}";
 }
 // ---------- Joins ----------
 // Nested loops in FROM order. A join whose ON has "this.col = <earlier tables>" looks the
 // matching rows up (by id, or an index on col); otherwise the joined table is read once
 // and every pair is tested. WHERE terms on the first table alone narrow its scan.
 static bool refs_only(const sql::Expr&e,const std::vector<std::string>&allowed){
  if(e.kind==sql::Expr::Col){if(e.path.size()<2)return false;return std::find(allowed.begin(),allowed.end(),e.path[0])!=allowed.end();}
  for(const auto&a:e.args)if(!refs_only(a,allowed))return false;
  return true;
 }
 static void and_terms(const sql::Expr&e,std::vector<const sql::Expr*>&out){if(e.kind==sql::Expr::And){and_terms(e.args[0],out);and_terms(e.args[1],out);}else out.push_back(&e);}
 std::string join_select(const sql::Select&s,const Params&params){
  for(const auto&f:s.from)prepare_query(f.table);
  ReadHold rl(*this);healthy();
  for(const auto&f:s.from)require_table(f.table);
  long long limit=s.limit?const_int(*s.limit,params,"LIMIT"):-1,offset=s.offset?const_int(*s.offset,params,"OFFSET"):0;
  std::vector<std::string> aliases;for(const auto&f:s.from)aliases.push_back(f.alias);
  std::optional<sql::Expr> first_where;
  // RIGHT/FULL joins: rows of the right table that no combination matched come out with
  // NULLs on the left. The first table is then not filtered early by WHERE (that would
  // turn matched rows into unmatched ones).
  bool outer_right=false;for(const auto&f:s.from)outer_right|=f.kind==sql::FromItem::Right||f.kind==sql::FromItem::Full;
  std::vector<std::vector<bool>> hit(s.from.size());
  if(s.where&&!outer_right){
   std::vector<const sql::Expr*> terms;and_terms(*s.where,terms);
   for(const auto*t:terms)if(refs_only(*t,{aliases[0]})){
    sql::Expr c=*t;sql::strip_qualifier(c,aliases[0],aliases[0]);
    if(!first_where)first_where=std::move(c);else{sql::Expr a;a.kind=sql::Expr::And;a.args.push_back(std::move(*first_where));a.args.push_back(std::move(c));first_where=std::move(a);}
   }
  }
  struct Lookup {bool ok=false;std::vector<std::string> path;const sql::Expr*other=nullptr;};
  std::vector<Lookup> lookups(s.from.size());
  for(size_t j=1;j<s.from.size();j++){
   const auto&f=s.from[j];if(!f.on)continue;
   if(f.kind==sql::FromItem::Right||f.kind==sql::FromItem::Full)continue;  // every row is needed
   std::vector<std::string> earlier(aliases.begin(),aliases.begin()+std::ptrdiff_t(j));
   std::vector<const sql::Expr*> terms;and_terms(*f.on,terms);
   for(const auto*t:terms){
    if(t->kind!=sql::Expr::Bin||t->op!="=")continue;
    for(int side=0;side<2&&!lookups[j].ok;side++){
     const auto&mine=t->args[side],&other=t->args[1-side];
     if(mine.kind==sql::Expr::Col&&mine.path.size()>1&&mine.path[0]==f.alias&&refs_only(other,earlier))
      lookups[j]={true,std::vector<std::string>(mine.path.begin()+1,mine.path.end()),&other};
    }
    if(lookups[j].ok)break;
   }
  }
  std::vector<std::optional<std::string>> part(s.from.size());
  auto combined=[&](size_t upto){
   std::string d="{";
   for(size_t i=0;i<s.from.size();i++){if(i)d+=',';d+=json::quote(aliases[i]);d+=':';d+=(i<=upto&&part[i])?*part[i]:std::string("null");}
   return d+"}";
  };
  std::vector<std::vector<std::string>> table_rows(s.from.size());std::vector<bool> loaded(s.from.size(),false);
  std::vector<std::string> out;
  std::function<void(size_t)> step=[&](size_t j){
   check_deadline();  // every combination counts: a join of large tables stops at the time limit
   if(j==s.from.size()){
    auto d=combined(j);
    if(s.where){sql::Row row{d,&params};row.aliases=&aliases;if(!sql::truthy(sql::eval(*s.where,row)))return;}
    out.push_back(std::move(d));
    require(out.size()<=1000000,"LIMIT a join can produce at most 1,000,000 rows before LIMIT");
    return;
   }
   const auto&f=s.from[j];
   std::vector<std::string> found;const std::vector<std::string>*rows=&found;
   if(lookups[j].ok){
    auto d=combined(j-1);sql::Row row{d,&params};row.aliases=&aliases;
    auto v=sql::eval(*lookups[j].other,row);
    if(!sql::is_null(v)){
     sql::Expr col;col.kind=sql::Expr::Col;col.path=lookups[j].path;
     sql::Expr lit;lit.kind=sql::Expr::Lit;lit.lit=v;
     sql::Expr eq;eq.kind=sql::Expr::Bin;eq.op="=";eq.args.push_back(std::move(col));eq.args.push_back(std::move(lit));
     std::optional<sql::Expr> w(std::move(eq));
     scan_rows(f.table,w,params,[&](std::string_view,uint64_t,std::string_view doc){found.emplace_back(doc);});
    }
   }else{
    if(!loaded[j]){scan_rows(f.table,std::nullopt,params,[&](std::string_view,uint64_t,std::string_view doc){table_rows[j].emplace_back(doc);});loaded[j]=true;}
    rows=&table_rows[j];
   }
   bool matched=false;bool track=f.kind==sql::FromItem::Right||f.kind==sql::FromItem::Full;
   if(track&&hit[j].size()<rows->size())hit[j].resize(rows->size(),false);
   for(size_t r=0;r<rows->size();r++){
    const auto&doc=(*rows)[r];
    part[j]=doc;
    if(f.on){auto d=combined(j);sql::Row row{d,&params};row.aliases=&aliases;if(!sql::truthy(sql::eval(*f.on,row)))continue;}
    matched=true;if(track)hit[j][r]=true;step(j+1);
   }
   part[j].reset();
   if(!matched&&(f.kind==sql::FromItem::Left||f.kind==sql::FromItem::Full))step(j+1);
  };
  std::vector<std::string> first;
  scan_rows(s.from[0].table,first_where,params,[&](std::string_view,uint64_t,std::string_view doc){first.emplace_back(doc);});
  for(auto&d:first){part[0]=std::move(d);step(1);}
  // Unmatched rows of RIGHT/FULL tables: NULL for every table before them.
  for(size_t j=1;j<s.from.size();j++){
   const auto&f=s.from[j];if(f.kind!=sql::FromItem::Right&&f.kind!=sql::FromItem::Full)continue;
   if(!loaded[j]){scan_rows(f.table,std::nullopt,params,[&](std::string_view,uint64_t,std::string_view doc){table_rows[j].emplace_back(doc);});loaded[j]=true;}
   hit[j].resize(table_rows[j].size(),false);
   for(size_t r=0;r<table_rows[j].size();r++){
    if(hit[j][r])continue;
    for(size_t i=0;i<j;i++)part[i].reset();
    part[j]=table_rows[j][r];
    // Later tables still join to this row; earlier RIGHT/FULL tables do not see it again.
    std::function<void(size_t)> rest=[&](size_t k){
     if(k==s.from.size()){auto d=combined(k);if(s.where){sql::Row row{d,&params};row.aliases=&aliases;if(!sql::truthy(sql::eval(*s.where,row)))return;}out.push_back(std::move(d));return;}
     step(k);
    };
    rest(j+1);
   }
   part[j].reset();
  }
  if(is_aggregate(s))return aggregate_select(s,params,limit,offset,[&](const RowSink&sink){for(const auto&d:out){sql::Row row{d,&params};row.aliases=&aliases;sink(row,d,0);}},nullptr,&aliases);
  if(limit==0)return "{\"rows\":[]}";
  return finish_rows(s,params,limit,offset,std::move(out),&aliases);
 }
public:

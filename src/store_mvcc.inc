// Included inside class Store (kestro.hpp): snapshot reads (MVCC).
//
// A SQL read runs against one committed state while holding readers_ shared. A long one
// would hold up every commit (a commit installs under readers_ exclusively), so a reader
// that has held the lock for a while and sees a commit waiting lets it through (a yield)
// and goes on reading the state it started with:
//   - The log is append-only: a row whose current value lies at or past the snapshot's end
//     changed after the snapshot began. Scans skip such rows where they find them.
//   - While snapshots are registered, each commit records, for every key it changes, the
//     value reference it replaces (undo_). The snapshot's version of a changed row is the
//     reference replaced by the first change at or past the snapshot's end. A scan adds
//     those rows where they belong in its order, unless it had passed them before the
//     change (it read them then, unchanged).
//   - A statement registers only at its first yield: short statements cost nothing, and a
//     commit costs one atomic load while no snapshot is registered.
//   - Memory: one record per row changed while a registered snapshot lasts: the row's key
//     (kept once) and the reference its change replaced, about 100 bytes. Records no
//     registered snapshot needs go at the next commit, and all of them, with their
//     allocations, the moment the last snapshot ends. No limit: statements and
//     transactions are bounded by their timeouts. stats().snapshotMemory.
//   - Only the outermost scan of a statement yields (nested scans run inside its callbacks);
//     every scan of the statement reads the same snapshot. Set positions are found again by
//     value after a yield. A schema change, a dropped index or a compaction during a yield
//     ends the statement with a retryable CONFLICT.
private:
 struct UndoRec {uint64_t at;bool existed;Ref prev;};
 // Guarded by undo_m_ (taken inside readers_; snap_m_ is never taken while holding it).
 mutable std::mutex undo_m_;
 std::unordered_map<std::string,std::vector<UndoRec>> undo_;  // key -> its changes, oldest first
 std::deque<std::pair<uint64_t,const std::string*>> undo_log_;uint64_t undo_base_=0;  // (at, key in undo_) in commit order; one per record; absolute positions
 size_t undo_bytes_=0;
 std::atomic<int> snapshots_{0},install_waiting_{0};
 std::mutex snap_m_;std::multiset<uint64_t> snap_ends_;
 uint64_t catalog_epoch_=0;  // readers_: bumped by changes other than commits that a yielding reader must not miss
 std::atomic<uint64_t> yields_{0};
 // What a serializable BEGIN CONCURRENT transaction read, checked at its COMMIT: no commit
 // after its snapshot may have changed a row in it (or added one: phantoms). Each scan adds
 // the ranges it walked: row key ranges ("table\0id": reads by id), index entry ranges
 // (encoded values + key: reads by an indexed value, unique ones included), or the whole
 // table (scans no key or index narrows). A scan that stopped at its LIMIT covers only the
 // part up to where it stopped. Bounded: past READ_SET_BYTES the ranges are merged, and if
 // that is not enough every table they cover is taken as read whole (coarser: more
 // conflicts, never fewer).
 struct ReadSet {
  struct Index {const SecIndex*ix;std::string table;query::Ranges r;};
  std::vector<std::string> tables;query::Ranges keys;std::vector<Index> idx;size_t bytes=0;
  bool empty()const{return tables.empty()&&keys.empty()&&idx.empty();}
 };
 static constexpr size_t READ_SET_BYTES=size_t(1)<<20;
 // txn: the snapshot of a BEGIN CONCURRENT transaction (registered from BEGIN to its end;
 // lock is set while one of its statements holds readers_; reads: its read set, when serializable).
 struct Snap {Store*store=nullptr;std::shared_lock<ReadWriteMutex>*lock=nullptr;uint64_t end=0,epoch=0;bool registered=false,txn=false;int depth=0;std::chrono::steady_clock::time_point since{};ReadSet*reads=nullptr;};
 static Snap*&tl_snap(){static thread_local Snap*s=nullptr;return s;}
 // Scans of a statement under way (in its Snap: no thread-local access per scan).
 struct ScanDepth {Snap*s;explicit ScanDepth(Snap*sn):s(sn){if(s)s->depth++;}~ScanDepth(){if(s)s->depth--;}};
 static size_t undo_cost(const std::string&key){return key.size()+sizeof(UndoRec)+96;}  // record, map node, log entry
 // undo_m_ held: every record goes and the memory is returned.
 void undo_clear_locked(){
  undo_base_+=undo_log_.size();
  decltype(undo_)().swap(undo_);decltype(undo_log_)().swap(undo_log_);undo_bytes_=0;
 }
 // Commit side (readers_ exclusive, before the install): what ops replace, while snapshots exist.
 void record_undo(const std::vector<Op>&ops,uint64_t at){
  if(!snapshots_.load(std::memory_order_acquire)){std::lock_guard<std::mutex>l(undo_m_);if(!undo_log_.empty())undo_clear_locked();return;}
  uint64_t min;{std::lock_guard<std::mutex>l(snap_m_);min=snap_ends_.empty()?UINT64_MAX:*snap_ends_.begin();}
  std::lock_guard<std::mutex>l(undo_m_);
  trim_undo_locked(min);
  for(const auto&o:ops){
   Ref r;auto h=index_hash(key_,o.key);bool existed=lookup(shard_of(h),o.key,h,r);
   auto it=undo_.try_emplace(o.key).first;
   it->second.push_back({at,existed,existed?r:Ref{}});undo_log_.emplace_back(at,&it->first);undo_bytes_+=undo_cost(o.key);
  }
 }
 // undo_m_ held: the records no registered snapshot needs (each needs those at or past its
 // end). Each log entry is one record, the oldest of its key.
 void trim_undo_locked(uint64_t min){
  while(!undo_log_.empty()&&undo_log_.front().first<min){
   auto it=undo_.find(*undo_log_.front().second);
   undo_bytes_-=std::min(undo_bytes_,undo_cost(it->first));
   it->second.erase(it->second.begin());
   undo_log_.pop_front();undo_base_++;
   if(it->second.empty())undo_.erase(it);  // no later log entry names it
  }
 }
 // The key's version in a snapshot ending at end, when it changed since (undo_m_ held).
 const UndoRec*undo_at(const std::string&key,uint64_t end)const{
  auto it=undo_.find(key);if(it==undo_.end())return nullptr;
  for(const auto&r:it->second)if(r.at>=end)return &r;
  return nullptr;
 }
 // A commit is waiting and this statement has held readers_ for a while.
 // The clock is read only once a commit waits: from then (or from the last yield) the
 // statement keeps readers_ for at most half a millisecond more.
 bool want_yield(Snap&s)const{
  if(!install_waiting_.load(std::memory_order_relaxed))return false;
  auto now=std::chrono::steady_clock::now();
  if(s.since==std::chrono::steady_clock::time_point{}){s.since=now;return false;}
  return now-s.since>=std::chrono::microseconds(500);
 }
 void yield_snapshot(Snap&s){
  register_snapshot(s);
  s.lock->unlock();
  // A rwlock may prefer readers: give the commit time to take the lock before asking again.
  auto t0=std::chrono::steady_clock::now();
  while(install_waiting_.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()-t0<std::chrono::milliseconds(20))std::this_thread::yield();
  s.lock->lock();
  yields_.fetch_add(1,std::memory_order_relaxed);s.since=std::chrono::steady_clock::now();
  healthy();
  if(catalog_epoch_!=s.epoch)throw std::runtime_error("CONFLICT the schema changed or the file was compacted while this query ran; run it again");
 }
 void register_snapshot(Snap&s){
  if(s.registered)return;
  {std::lock_guard<std::mutex>l(snap_m_);snap_ends_.insert(s.end);}snapshots_.fetch_add(1,std::memory_order_release);s.registered=true;
 }
 // Automatic ids handed to BEGIN CONCURRENT transactions not yet committed, per table: no
 // other INSERT takes them (a rolled-back transaction leaves a gap, as sequences do).
 std::mutex seq_m_;std::unordered_map<std::string,long long> seq_reserved_;
 long long seq_floor(const std::string&table){std::lock_guard<std::mutex>l(seq_m_);auto it=seq_reserved_.find(table);return it==seq_reserved_.end()?0:it->second;}
 void seq_reserve(const std::string&table,long long upto){std::lock_guard<std::mutex>l(seq_m_);auto&v=seq_reserved_[table];if(upto>v)v=upto;}
 void end_snapshot(Snap&s){
  if(!s.registered)return;
  std::lock_guard<std::mutex>l(snap_m_);
  auto it=snap_ends_.find(s.end);if(it!=snap_ends_.end())snap_ends_.erase(it);
  snapshots_.fetch_sub(1,std::memory_order_release);s.registered=false;
  // The last one: its records go at once (a snapshot registering meanwhile waits on snap_m_,
  // so none of its records can be among them).
  if(snap_ends_.empty()){std::lock_guard<std::mutex>u(undo_m_);undo_clear_locked();}
 }
 // The rows of one scan that changed after its statement's snapshot, as of the snapshot:
 // (entry, reference), in the scan's order, not yet passed.
 struct SnapScan {
  Store&st;Snap*sn;const std::string&prefix;const SecIndex*src;const query::Ranges&ranges;bool desc,shard_mode;
  struct Pend {std::string entry;Ref ref;};
  std::vector<Pend> pend;size_t next=0;std::unordered_set<std::string> handled;uint64_t seen=0;
  std::string last;bool have_last=false;size_t shard_now=0;
  bool in_ranges(std::string_view e)const{for(const auto&rg:ranges)if(e>=rg.first&&(rg.second.empty()||e<rg.second))return true;return false;}
  // Takes in undo records added since the last call (readers_ shared).
  void absorb(){
   if(!sn||!sn->registered)return;
   std::lock_guard<std::mutex>l(st.undo_m_);
   uint64_t end=st.undo_base_+st.undo_log_.size();
   // Undo references point into the file as it was: after a compaction they are not read.
   if(sn->epoch!=st.catalog_epoch_)throw std::runtime_error("CONFLICT the schema changed or the file was compacted during this snapshot; run it again");
   size_t before=pend.size();
   for(uint64_t i=std::max(seen,st.undo_base_);i<end;i++){
    const auto&rec=st.undo_log_[size_t(i-st.undo_base_)];
    if(rec.first<sn->end)continue;
    const std::string&key=*rec.second;
    if(key.size()<=prefix.size()||key.compare(0,prefix.size(),prefix)!=0)continue;
    if(!handled.insert(key).second)continue;
    const UndoRec*u=st.undo_at(key,sn->end);if(!u||!u->existed)continue;
    std::string e;
    if(src){std::string buf;auto v=st.view_ref(u->prev,buf);e=index_entry(*src,v,key);}else e=key;
    bool passed=shard_mode?size_t(st.shard_of(index_hash(st.key_,key)))<shard_now:have_last&&(desc?e>=last:e<=last);
    if(passed||!in_ranges(e))continue;
    pend.push_back({std::move(e),u->prev});
   }
   seen=end;
   if(pend.size()!=before)std::sort(pend.begin()+std::ptrdiff_t(next),pend.end(),[&](const Pend&a,const Pend&b){return desc?a.entry>b.entry:a.entry<b.entry;});
  }
 };
 static size_t range_cost(const query::Ranges&r){size_t n=0;for(const auto&x:r)n+=x.first.size()+x.second.size()+48;return n;}
 // Sorted, without empty or overlapping ranges ("" as hi: no bound).
 static void merge_ranges(query::Ranges&r){
  std::sort(r.begin(),r.end());query::Ranges out;
  for(auto&x:r){
   if(!x.second.empty()&&x.first>=x.second)continue;
   if(!out.empty()&&(out.back().second.empty()||x.first<=out.back().second)){if(x.second.empty())out.back().second.clear();else if(!out.back().second.empty())out.back().second=std::max(out.back().second,x.second);}
   else out.push_back(std::move(x));
  }
  r=std::move(out);
 }
 static bool in_merged(const query::Ranges&r,std::string_view e){
  auto it=std::upper_bound(r.begin(),r.end(),e,[](std::string_view v,const std::pair<std::string,std::string>&x){return v<x.first;});
  if(it==r.begin())return false;--it;return it->second.empty()||e<it->second;
 }
 // A row key's id as SQL shows it (the encoded number or text; anything else as it is).
 static std::string id_text(std::string_view e){
  json::Scalar v;
  if(e.size()==9&&uint8_t(e[0])==json::NUMBER){uint64_t b=0;for(size_t i=1;i<9;i++)b=b<<8|uint8_t(e[i]);b=(b>>63)?b^(uint64_t(1)<<63):~b;double d;memcpy(&d,&b,8);v=sql::number(d);}
  else if(e.size()>=3&&uint8_t(e[0])==json::STRING&&e.substr(e.size()-2)==std::string_view("\0\1",2)){v.tag=json::STRING;for(size_t i=1;i+2<e.size();i++){v.str+=e[i];if(e[i]==0)i++;}}
  else return std::string(e);
  return sql::to_json(v);
 }
 // A scan of table under a serializable transaction: ranges of ix (row keys without one);
 // whole: every row.
 static void note_reads(ReadSet&rs,const std::string&table,const SecIndex*ix,query::Ranges ranges,bool whole){
  if(std::find(rs.tables.begin(),rs.tables.end(),table)!=rs.tables.end())return;
  if(!whole&&ix)for(const auto&x:ranges)if(x.first.empty()&&x.second.empty())whole=true;  // the whole index
  if(whole){rs.tables.push_back(table);return;}
  size_t add=range_cost(ranges);
  if(rs.bytes+add>READ_SET_BYTES){
   // Merged first (the same rows read again add nothing); then table by table.
   merge_ranges(rs.keys);rs.bytes=range_cost(rs.keys);
   for(auto&x:rs.idx){merge_ranges(x.r);rs.bytes+=range_cost(x.r);}
   if(rs.bytes+add>READ_SET_BYTES/2){
    auto whole_table=[&](const std::string&t){if(std::find(rs.tables.begin(),rs.tables.end(),t)==rs.tables.end())rs.tables.push_back(t);};
    for(const auto&x:rs.keys)whole_table(std::string(collection_of(x.first)));
    for(const auto&x:rs.idx)whole_table(x.table);
    whole_table(table);
    rs.keys.clear();rs.keys.shrink_to_fit();rs.idx.clear();rs.idx.shrink_to_fit();rs.bytes=0;
    return;
   }
  }
  rs.bytes+=add;
  if(!ix){for(auto&x:ranges)rs.keys.push_back(std::move(x));return;}
  for(auto&x:rs.idx)if(x.ix==ix){for(auto&y:ranges)x.r.push_back(std::move(y));return;}
  rs.idx.push_back({ix,table,std::move(ranges)});
 }
 // The read set at COMMIT (writer lock, readers_ shared and undo_m_ held; same catalog epoch):
 // every row changed by a commit after the snapshot must lie outside it, as it was then and
 // as it is now.
 template<class Again> void validate_reads_locked(ReadSet&rs,uint64_t end,const Again&again){
  if(rs.empty())return;
  merge_ranges(rs.keys);for(auto&x:rs.idx)merge_ranges(x.r);
  auto it=std::lower_bound(undo_log_.begin(),undo_log_.end(),end,[](const std::pair<uint64_t,const std::string*>&r,uint64_t at){return r.first<at;});
  std::unordered_set<const std::string*> seen;std::string buf;
  for(;it!=undo_log_.end();++it){
   const std::string&key=*it->second;
   if(key.empty()||uint8_t(key[0])==1||!seen.insert(&key).second)continue;  // the engine's own records (sequences: merged)
   auto t=collection_of(key);if(t.empty())continue;
   bool hit=in_merged(rs.keys,key);
   for(const auto&x:rs.tables)hit|=x==t;
   for(const auto&x:rs.idx){
    if(hit)break;if(x.table!=t)continue;
    const UndoRec*u=undo_at(key,end);
    if(u&&u->existed&&in_merged(x.r,index_entry(*x.ix,view_ref(u->prev,buf),key)))hit=true;
    Ref r;auto h=index_hash(key_,key);
    if(!hit&&lookup(shard_of(h),key,h,r)&&in_merged(x.r,index_entry(*x.ix,view_ref(r,buf),key)))hit=true;
   }
   if(hit)throw again("another transaction changed a row of "+std::string(t)+" that this one read (id "+id_text(std::string_view(key).substr(t.size()+1))+")");
  }
 }
public:
 // Times a statement let a waiting commit through (stats, tests).
 uint64_t snapshot_yields()const{return yields_.load(std::memory_order_relaxed);}
 // Bytes the undo records of running snapshots take (stats).
 size_t snapshot_memory()const{std::lock_guard<std::mutex>l(undo_m_);return undo_bytes_;}
private:

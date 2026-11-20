// Included inside class Store (kestro.hpp): replication.
//
// A follower holds a byte-identical copy of the leader's log. It asks the leader for the
// groups after its own end (read_log), giving the hash of its last group so the leader can
// refuse a follower that no longer continues its log (after a compaction or a restore on
// either side: STALE, reseed). The follower commits each received group's operations
// through its normal commit path with the same expected versions (apply_replicated): the
// encoding is deterministic, so its groups, hashes and chain come out identical, and its
// key index, ordered keys, secondary indexes and column cache follow. A different chain
// stops replication (DIVERGED) instead of continuing silently.
public:
 struct LogChunk {uint64_t leader_end=0,leader_txid=0;std::array<unsigned char,16> uuid{};Bytes bytes;};
 // Leader: whole groups after from_end (whose last group hashes to chain), up to max_bytes
 // (at least one group), waiting up to wait_ms for a commit when there is none yet.
 // raw: the bytes may end inside a group (seeding a new follower, which copies bytes and
 // checks them as a whole): a group larger than max_bytes, such as the checkpoint group a
 // compaction writes, then arrives in pieces.
 LogChunk read_log(uint64_t from_end,const Hash&chain,size_t max_bytes,unsigned wait_ms,bool raw=false)const{
  Meta m;{std::shared_lock<ReadWriteMutex>r(readers_);healthy();m=meta_;}
  require(from_end>=START&&from_end<=m.end,"STALE replica: its log ends past the leader's (reseed it)");
  if(from_end>START){auto t=read_at(fd_.n,32,from_end-32);require(!memcmp(t.data(),chain.data(),32),"STALE replica: it no longer continues the leader's log (compacted or restored; reseed it)");}
  else require(chain==Hash{},"STALE replica: empty log with a chain");
  if(from_end==m.end&&wait_ms){
   repl_waiters_.fetch_add(1);
   {std::unique_lock<std::mutex>l(pub_m_);pub_cv_.wait_for(l,std::chrono::milliseconds(wait_ms),[&]{return published_end_.load(std::memory_order_acquire)>from_end;});}
   repl_waiters_.fetch_sub(1);
   std::shared_lock<ReadWriteMutex>r(readers_);healthy();m=meta_;
  }
  LogChunk out;out.leader_end=m.end;out.leader_txid=m.txid;out.uuid=m.uuid;
  uint64_t to=from_end;
  if(raw)to=std::min<uint64_t>(m.end,from_end+max_bytes);  // a byte range: group boundaries do not matter
  else while(to<m.end){
   auto head=read_at(fd_.n,64,to);require(is_group_magic(head.data()),"CORRUPT group magic");
   uint64_t next=to+96+get64(head.data()+8);require(next<=m.end,"CORRUPT frame length");
   if(to>from_end&&next-from_end>max_bytes)break;
   to=next;
  }
  out.bytes.resize(size_t(to-from_end));if(!out.bytes.empty())platform::read_exact(fd_.n,out.bytes.data(),out.bytes.size(),from_end);
  return out;
 }
 // Follower: commits whole groups received from the leader (they continue this log).
 // Returns the transaction ID reached.
 uint64_t apply_replicated(const unsigned char*p,size_t n){
  char tok;GateHold gate(*this,&tok);
  std::lock_guard<std::mutex>w(writer_);healthy();
  Hash chain=meta_.chain;uint64_t tx=meta_.txid;bool catalog=false;
  std::vector<std::vector<Op>> txs;std::vector<Hash> hashes;size_t pos=0;
  while(pos<n){
   require(n-pos>=96&&is_group_magic(p+pos),"CORRUPT replicated group");
   uint64_t len=get64(p+pos+8),seq=get64(p+pos+16);uint32_t flags=get32(p+pos+24),count=get32(p+pos+28);
   require(!flags,"STALE replica: the leader's log starts with a compacted checkpoint here (reseed it)");
   require(len<=MAX_GROUP&&len<=n-pos-96&&seq==tx+1&&!memcmp(p+pos+32,chain.data(),32),"CORRUPT replicated group sequence or chain");
   auto h=digest(p+pos,size_t(64+len));require(!memcmp(h.data(),p+pos+64+len,32),"CORRUPT replicated group hash");
   std::vector<Op> ops;ops.reserve(count);size_t q=pos+64;
   for(uint32_t i=0;i<count;i++){
    require(q+17<=pos+64+len,"CORRUPT replicated operation");
    bool set=p[q]!=0;uint64_t expect=get64(p+q+1);uint32_t kn=get32(p+q+9),vn=get32(p+q+13);q+=17;
    require(q+kn+(set?vn:0)<=pos+64+len,"CORRUPT replicated operation");
    Op o;o.key.assign(reinterpret_cast<const char*>(p+q),kn);q+=kn;
    if(set){o.value.assign(reinterpret_cast<const char*>(p+q),vn);q+=vn;}
    o.expected=expect;o.erase=!set;
    if(o.key==catalog_key())catalog=true;
    ops.push_back(std::move(o));
   }
   require(q==pos+64+len,"CORRUPT replicated operation bounds");
   txs.push_back(std::move(ops));hashes.push_back(h);chain=h;tx=seq;pos+=96+len;
  }
  if(txs.empty())return meta_.txid;
  std::vector<const std::vector<Op>*> refs;for(const auto&t:txs)refs.push_back(&t);
  auto out=commit_group_locked(refs,true);
  for(const auto&o:out)if(o.error)std::rethrow_exception(o.error);
  require(meta_.txid==tx&&meta_.chain==chain,"DIVERGED replica: its log differs from the leader's after applying (reseed it)");
  if(catalog)reload_catalog_locked();
  return tx;
 }
 // The commit point (follower handshake, tests).
 Hash chain()const{std::shared_lock<ReadWriteMutex>r(readers_);return meta_.chain;}
 std::array<unsigned char,16> uuid()const{return meta_.uuid;}
 // A new database file at path from raw log bytes [START, end) of a leader (seeding a
 // follower): fetch(from, chain) returns the next chunk; done when it reaches the leader's end.
 static void seed_file(const std::string&path,const std::function<LogChunk(uint64_t,const Hash&)>&fetch){
  auto temp=temp_name(path);bool created=false;
  try{
   FD out(platform::open_file(temp,true));created=true;
   uint64_t end=START;Hash chain{};uint64_t txid=0;std::array<unsigned char,16> uuid{};
   for(;;){
    auto c=fetch(end,chain);uuid=c.uuid;
    if(!c.bytes.empty()){
     write_at(out.n,c.bytes.data(),c.bytes.size(),end);end+=c.bytes.size();
     auto t=read_at(out.n,32,end-32);memcpy(chain.data(),t.data(),32);
    }
    if(end>=c.leader_end)break;
   }
   // The transaction ID at end: from the last group's header (walk the headers).
   for(uint64_t pos=START;pos<end;){auto h=read_at(out.n,64,pos);txid=get64(h.data()+16);pos+=96+get64(h.data()+8);}
   Meta m;m.generation=1;m.txid=txid;m.end=end;m.uuid=uuid;m.chain=chain;
   Bytes headers(START,0);auto h=meta_bytes(m);memcpy(headers.data()+4096,h.data(),4096);write_at(out.n,headers.data(),headers.size(),0);
   sync_file(out.n);verify_log_range(out.n,START,0,Hash{},m);
   publish(temp,path);created=false;
  }catch(...){if(created)platform::remove_noexcept(temp);throw;}
 }
 // A closed file cut back to transaction txid (a cluster node holding commits the cluster
 // never acknowledged, from a term it lost): the groups after it are dropped and both slots
 // rewritten. Returns false when txid is not a group boundary of this log (before a
 // compacted checkpoint): the node reseeds instead.
 static bool truncate_log(const std::string&path,uint64_t txid){
  FD fd(platform::open_file(path));bool rewrite=false;Meta m=recover_meta(fd.n,rewrite);
  if(txid>=m.txid)return txid==m.txid;
  uint64_t pos=START;Hash chain{};
  if(txid>0){
   bool found=false;
   while(pos<m.end){
    auto h=read_at(fd.n,64,pos);require(is_group_magic(h.data()),"CORRUPT group magic");
    uint64_t len=get64(h.data()+8),seq=get64(h.data()+16);require(len<=MAX_GROUP&&pos+96+len<=m.end,"CORRUPT frame length");
    pos+=96+len;
    if(seq==txid){auto t=read_at(fd.n,32,pos-32);memcpy(chain.data(),t.data(),32);found=true;break;}
    if(seq>txid)return false;
   }
   if(!found)return false;
  }
  Meta n=m;n.end=pos;n.txid=txid;n.chain=chain;
  for(int i=0;i<2;i++){n.generation++;auto b=meta_bytes(n);write_at(fd.n,b.data(),b.size(),(n.generation%2)*4096);}
  sync_file(fd.n);platform::truncate(fd.n,pos);sync_file(fd.n);
  for(auto ext:{".idx",".sets",".sort",".sorting"})platform::remove_noexcept(path+ext);  // caches of the longer log
  return true;
 }
private:
 mutable std::mutex pub_m_;mutable std::condition_variable pub_cv_;
 std::atomic<uint64_t> published_end_{0};mutable std::atomic<int> repl_waiters_{0};
 void publish_commit(uint64_t end){
  published_end_.store(end,std::memory_order_release);
  if(repl_waiters_.load(std::memory_order_acquire)){std::lock_guard<std::mutex>l(pub_m_);pub_cv_.notify_all();}
 }
 // A replicated change to the catalog: tables, columns and index definitions are read again.
 void reload_catalog_locked(){
  {std::unique_lock<ReadWriteMutex>r(readers_);catalog_epoch_++;cat_loaded_=false;sec_.clear();tables_.clear();columns_.clear();colstores_.clear();rules_.clear();}
  load_catalog_locked();
 }
public:

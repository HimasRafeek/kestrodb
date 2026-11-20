// Included inside class Store (kestro.hpp): online compaction.
//
// compact() rewrites the database file to hold only current values while reads and writes
// continue:
//  1. m0 = the commit point. Shard by shard (under that shard's shared lock), every live
//     value unchanged since m0 is written to a new file as one checkpoint group (seq
//     m0.txid). Values changed or deleted after m0 are left out: phase 2 brings them.
//  2. The groups committed after m0 are appended, re-chained after the checkpoint and with
//     their version expectations dropped (they held when committed; the rewritten history
//     starts at the checkpoint). Repeated while much is left.
//  3. Without the writer lock: a replay of the new file so far (which checks all of it and
//     gives the new key index) and a flush; then catching up again, replaying only the new
//     groups. With the writer lock: the last groups, a commit slot, a flush of that tail and
//     its replay; then, with every lock, the new file replaces the old one. (Replaying the
//     whole file under the writer lock held up every write and most reads for a second or
//     two on a 60 MB database: the soak's slowest operations.) Ordered keys, secondary indexes and the column cache
//     describe the same rows and are kept.
// Point reads that looked up a value just before the switch read again (file_gen_). No
// reader can be using the old file's mapped views at the switch (every lock is held, and
// point reads copy mapped values under their shard lock), so the switch unmaps them, closes
// the old file and deletes it; a copy that another process (a reader) still holds open is
// deleted at the next compaction or close. A crash leaves the database whole: the temporary <file>.compacting is
// removed on the next open, and a missing <file> next to a <file>.compacted-old-N is
// renamed back.
// Other processes reading the file (readFrom / read_from) see STALE and must reopen.
public:
 // before: the old file's committed bytes at the switch; after: the new file's.
 struct CompactResult {uint64_t before=0,after=0,keys=0;};
 CompactResult compact(){
  std::lock_guard<std::mutex>one(compact_m_);
  // The background verifier reads the old file by offset: let it finish first.
  Background::run_or_wait(verify_task_);
  CompactResult res;
  {std::lock_guard<std::mutex>w(writer_);healthy();materialize();res.before=meta_.end;}
  Meta m0;{std::shared_lock<ReadWriteMutex>r(readers_);healthy();m0=meta_;}
  if(m0.txid==0)return {m0.end,m0.end,0};
  const std::string temp=path_+".compacting";platform::remove_noexcept(temp);
  FD out(platform::open_file(temp,true));bool done=false;
  struct Cleanup{const std::string&p;bool&d;FD&f;~Cleanup(){if(!d){platform::close(f.n);f.n=platform::invalid_file();platform::remove_noexcept(p);}}}cleanup{temp,done,out};
  // Phase 1: the checkpoint body, then its header and hash.
  uint64_t len=0,count=0;Hash chain;
  {
   FrameWriter w(out.n,START+64);std::string buf;
   for(size_t s=0;s<shards_.size();s++){
    std::shared_lock<ReadWriteMutex>l(shards_[s].mutex);healthy();
    for_each_live(s,[&](std::string_view k,const Ref&r,uint64_t){
     if(r.version>m0.txid)return;  // changed after m0: comes with phase 2
     auto v=view_ref(r,buf);unsigned char op[17];op[0]=1;put64(op+1,r.version);put32(op+9,uint32_t(k.size()));put32(op+13,uint32_t(v.size()));
     w.add(op,17);w.add(k.data(),k.size());w.add(v.data(),v.size());len+=17+k.size()+v.size();count++;});
   }
   w.flush();
   require(count<=UINT32_MAX,"LIMIT checkpoint key count");
   unsigned char head[64]{};memcpy(head,MAGIC_GROUP,8);put64(head+8,len);put64(head+16,m0.txid);put32(head+24,1);put32(head+28,uint32_t(count));
   write_at(out.n,head,64,START);
   StreamHash sh;Bytes b(size_t(1)<<20);
   for(uint64_t off=START,end=START+64+len;off<end;){size_t n=size_t(std::min<uint64_t>(b.size(),end-off));platform::read_exact(out.n,b.data(),n,off);sh.update(b.data(),n);off+=n;}
   chain=sh.finish();write_at(out.n,chain.data(),32,START+64+len);
  }
  res.keys=count;
  uint64_t pos=START+96+len,tx=m0.txid,from=m0.end;
  // Phase 2: catch up without the writer lock while much is left.
  for(int round=0;round<16;round++){
   Meta m;{std::shared_lock<ReadWriteMutex>r(readers_);healthy();m=meta_;}
   if(m.end-from<(uint64_t(1)<<20))break;
   append_tail(out.n,from,m.end,pos,tx,chain);from=m.end;
  }
  // Phase 3a: the key index of the new file so far, checked in full, without the writer
  // lock; then the commits made meanwhile, replayed group by group as they are appended.
  Sharded rebuilt;
  {Meta part;part.end=pos;part.txid=tx;part.chain=chain;replay(out.n,part,rebuilt,key_);}
  sync_file(out.n);
  for(int round=0;round<16;round++){
   Meta m;{std::shared_lock<ReadWriteMutex>r(readers_);healthy();m=meta_;}
   if(m.end-from<(uint64_t(256)<<10))break;
   uint64_t p0=pos,t0=tx;Hash c0=chain;
   append_tail(out.n,from,m.end,pos,tx,chain);from=m.end;
   replay_tail(out.n,p0,t0,c0,pos,rebuilt,key_);
  }
  // Phase 3b: no index merge, no commit, then the last groups and the switch.
  {
   std::lock_guard<std::mutex>sc(sc_m_);
   char tok;GateHold gate(*this,&tok);
   std::lock_guard<std::mutex>w(writer_);healthy();
   uint64_t p0=pos,t0=tx;Hash c0=chain;
   append_tail(out.n,from,meta_.end,pos,tx,chain);
   require(tx==meta_.txid,"internal: compaction lost transactions");
   res.before=meta_.end;  // the old file at the switch: what the compaction replaced
   Meta nm=meta_;nm.generation=1;nm.end=pos;nm.chain=chain;
   {Bytes headers(START,0);auto h=meta_bytes(nm);memcpy(headers.data()+4096,h.data(),4096);write_at(out.n,headers.data(),headers.size(),0);}
   sync_file(out.n);  // the tail and the slot (the rest was flushed above)
   replay_tail(out.n,p0,t0,c0,pos,rebuilt,key_);   // every group before p0 was checked above
   require(total(rebuilt)==index_count_,"internal: compaction changed the key count");
   switch_file(out,nm,rebuilt);
   done=true;res.after=nm.end;
  }
  return res;
 }
private:
 std::mutex compact_m_;
 std::atomic<uint64_t> file_gen_{0};
 std::vector<std::unique_ptr<FD>> old_files_;std::vector<std::string> old_names_;
 // Appends the groups [from, to) of this file to out at pos, re-chained after chain and with
 // expected versions dropped. Groups are at most 64 MB.
 void append_tail(File out,uint64_t from,uint64_t to,uint64_t&pos,uint64_t&tx,Hash&chain){
  while(from<to){
   auto head=read_at(fd_.n,64,from);require(is_group_magic(head.data()),"CORRUPT group magic");
   auto len=get64(head.data()+8),seq=get64(head.data()+16);uint32_t count=get32(head.data()+28);
   require(get32(head.data()+24)==0&&seq==tx+1&&len<=MAX_GROUP,"CORRUPT group in compaction");
   auto g=read_at(fd_.n,size_t(64+len+32),from);
   auto d=digest(g.data(),size_t(64+len));require(!memcmp(d.data(),g.data()+64+len,32),"CORRUPT frame hash");
   size_t p=64;for(uint32_t i=0;i<count;i++){require(p+17<=64+len,"CORRUPT operation bounds");memset(g.data()+p+1,0xff,8);auto kn=get32(g.data()+p+9),vn=get32(g.data()+p+13);p+=17+kn+(g[p]?vn:0);}
   require(p==64+len,"CORRUPT operation bounds");
   memcpy(g.data()+32,chain.data(),32);
   auto h=digest(g.data(),size_t(64+len));memcpy(g.data()+64+len,h.data(),32);
   write_at(out,g.data(),g.size(),pos);
   pos+=g.size();from+=96+len;tx=seq;chain=h;
  }
 }
 // With the writer lock: the new file becomes the database.
 void switch_file(FD&out,const Meta&nm,Sharded&rebuilt){
  std::unique_lock<ReadWriteMutex>r(readers_);catalog_epoch_++;  // offsets change: yielding snapshot readers start again
  std::array<std::unique_lock<ReadWriteMutex>,64> locks;for(size_t i=0;i<shards_.size();i++)locks[i]=std::unique_lock<ReadWriteMutex>(shards_[i].mutex);
  const std::string old=path_+".compacted-old-"+std::to_string(file_gen_.load()+1),temp=path_+".compacting";
  // The marker tells the next open to look for old copies (left if the store does not
  // close cleanly); it is durable before the old file gets its new name.
  {std::ofstream marker(std::filesystem::u8path(path_+".compacted-old"),std::ios::binary);require(bool(marker),"IO cannot create "+path_+".compacted-old");}
  sync_parent(path_);
  // The old file keeps its handle (owner lock, mappings for readers mid-copy) until close.
  platform::replace_keeping(path_,temp,old);
  {FD fresh(platform::open_file(path_,false,true));  // owner lock on the new file
   old_files_.push_back(std::make_unique<FD>());old_names_.push_back(old);std::swap(old_files_.back()->n,fd_.n);std::swap(fd_.n,fresh.n);rfd_.store(fd_.n,std::memory_order_release);}
  if(platform::valid(slot_fd_.n)){platform::close(slot_fd_.n);slot_fd_.n=platform::invalid_file();}
  platform::close(out.n);out.n=platform::invalid_file();
  // Key index: the rebuilt one, in RAM (the paged index file described the old offsets).
  for(size_t s=0;s<64;s++){pg_drop_base(s);frozen_[s].clear();has_frozen_[s]=false;live_[s]=rebuilt[s].size();shards_[s].index=std::move(rebuilt[s]);pg_dirty_[s]=true;}
  if(paged_){paged_=false;for(auto&x:pgrec_)x=PgRec{};}
  if(platform::valid(sc_.n)){try{Bytes z(2*PG_SLOT,0);write_at(sc_.n,z.data(),z.size(),0);sync_file(sc_.n);}catch(...){}}
  index_count_=total(shards_);
  // Read views: those of the old file go (nothing refers to them any more); map the new file.
  for(auto&v:views_)v.store(nullptr,std::memory_order_release);
  maps_.clear();retired_.clear();last_view_=nullptr;last_base_=UINT64_MAX;
  // The old file itself: closed and deleted now; one another process holds open stays
  // listed for a later attempt.
  old_files_.clear();
  {std::vector<std::string> left;for(const auto&n:old_names_){platform::remove_noexcept(n);std::error_code ec;if(std::filesystem::exists(std::filesystem::u8path(n),ec))left.push_back(n);}
   old_names_=std::move(left);if(old_names_.empty())platform::remove_noexcept(path_+".compacted-old");}
  meta_=nm;reserved_=nm.end;snapshot_intact_=false;from_snapshot_=false;from_sidecar_=false;
  verify_state_.store(2,std::memory_order_release);  // the new file was replayed in full
  file_gen_.fetch_add(1,std::memory_order_acq_rel);
  map_all();
 }
public:

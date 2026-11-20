// Included inside class Store (kestro.hpp): the paged key index.
//
// While the key index fits in Options::indexMemory it lives in RAM, as before. Once it
// outgrows that budget it moves to a sidecar file next to the database (<database>.idx)
// that is memory-mapped: the OS keeps the parts in use in RAM and the rest on disk, and
// takes the memory back when other programs need it. RAM then holds only recent changes
// (each shard's delta). A lookup checks the delta, then the shard's on-disk hash table.
// When the deltas outgrow the budget again, the maintainer thread merges the largest one
// into its shard's on-disk table (rewriting that shard only), in the background, while
// reads and commits continue. The log stays the only source of truth: the sidecar is a
// cache bound to the log by offset, transaction ID and chain hash, checked by BLAKE3, and
// rebuilt from the log when it is missing, stale or damaged.
//
// File: two header slots (16 KB each; the valid one with the higher generation wins), then
// sections at 64 KB boundaries, reused once no header refers to them.
//   header: "KDBXIDX1", u64 generation, database uuid[16], read-checksum key (SipHash 16,
//           BLAKE3 32, index seed 8), 8 zero bytes; then per shard 168 bytes: base offset,
//           length, count; delta offset, length, count; end, txid, live keys (u64 each);
//           chain hash at end[32]; BLAKE3 of the base's tail[32]; BLAKE3 of the delta[32].
//           The slot's last 32 bytes: BLAKE3 of the rest.
//   A shard's state at log offset `end` is its base overlaid with its delta. Opening loads
//   the deltas and replays the log from each shard's end, checking that end, txid and
//   chain hash meet the log exactly.
//   base:   entries, then the slot array (u64: low 32 bits of the key hash << 32 | entry
//           offset + 1, 0 = empty; linear probing; at most 2/3 full); then one 16-byte
//           BLAKE3 per 64 KB block of entries and slots (a block is checked the first time
//           it is read); then a 32-byte trailer: u64 count, u64 entries length, u32 slots,
//           u32 0, "KDBXSEC1". The header holds the BLAKE3 of block hashes and trailer.
//   delta:  entries only; an entry with value offset 0 deletes the key (hides the base's).
//   entry:  u16 key length, key, u64 value offset, u32 value length, u64 version, first 8
//           bytes of the value's read checksum.
// Locking: sc_m_ (sidecar writes, merges) before writer_ before readers_ before shard locks.
// A merge freezes a shard's delta (frozen_, still searched) under writer_, readers_ and the
// shard lock, writes the merged table with no lock held, and publishes it under them again.
private:
 static constexpr uint64_t PG_SLOT=16384,PG_FIRST=65536,PG_ALIGN=65536,PG_BLOCK=65536;
 static constexpr size_t PG_TABLE=96,PG_REC=168,PG_ENTRY=30;  // entry: 2 + key + 28
 struct PgRec{uint64_t base_off=0,base_len=0,base_count=0,delta_off=0,delta_len=0,delta_count=0,end=START,txid=0,live=0;Hash chain{},base_hash{},delta_hash{};};
 struct PgBase{
  std::unique_ptr<Mapping> map;const unsigned char*p=nullptr;  // p null: no base
  uint64_t count=0,entries=0,data=0,blocks=0,end=START;uint32_t slots=0;
  const unsigned char*hashes=nullptr;bool trusted=false;  // trusted: written by this process
  std::unique_ptr<std::atomic<uint64_t>[]> checked;      // one bit per verified block
 };
 std::array<PgBase,64> pgb_;
 std::array<Index,64> frozen_;std::array<bool,64> has_frozen_{};
 std::array<PgRec,64> pgrec_{};          // what the durable header says (sc_m_)
 std::array<size_t,64> live_{};          // live keys per shard (with the shard's index)
 std::array<bool,64> pg_dirty_{};        // changed since its record (writer lock)
 std::atomic<bool> paged_{false};        // the sidecar is in use
 bool from_sidecar_=false,opening_=false,mt_merge_=false;
 FD sc_;uint64_t sc_gen_=0;
 mutable std::mutex sc_m_;
 mutable std::atomic<bool> pg_damaged_{false};
 std::atomic<uint64_t> pg_min_end_{START};
 std::string sc_path()const{return path_+".idx";}
 static uint32_t pg_slots(uint64_t n){uint32_t c=16;while(uint64_t(c)*2<n*3){require(c<(1u<<31),"LIMIT index shard");c*=2;}return c;}
 static uint64_t pg_round(uint64_t n){return (n+PG_ALIGN-1)/PG_ALIGN*PG_ALIGN;}
 // Starts the maintainer if needed and hands it work: merges (or a rebuild) or, after
 // commits go quiet, an index checkpoint.
 void wake(bool merge)const{
  auto self=const_cast<Store*>(this);std::lock_guard<std::mutex>l(self->mt_m_);if(self->mt_stop_)return;
  if(merge)self->mt_merge_=true;else self->mt_dirty_=true;
  if(!self->maintainer_.joinable()){try{self->maintainer_=std::thread([self]{self->maintain();});}catch(...){}}
  self->mt_cv_.notify_one();
 }
 [[noreturn]] void pg_fail()const{
  pg_damaged_=true;if(!opening_)wake(true);
  throw std::runtime_error("CORRUPT index file damaged; it is being rebuilt from the log, retry");
 }
 // Checks the blocks of shard s's base that cover [a, a+n), each the first time it is read.
 void pg_check(size_t s,uint64_t a,uint64_t n)const{
  const auto&b=pgb_[s];if(b.trusted||!n)return;
  for(uint64_t k=a/PG_BLOCK,last=(a+n-1)/PG_BLOCK;k<=last;k++){
   auto&w=b.checked[k/64];uint64_t bit=uint64_t(1)<<(k%64);if(w.load(std::memory_order_acquire)&bit)continue;
   uint64_t from=k*PG_BLOCK;auto h=blake3_digest(b.p+from,size_t(std::min<uint64_t>(PG_BLOCK,b.data-from)));
   if(memcmp(h.data(),b.hashes+16*k,16))pg_fail();
   w.fetch_or(bit,std::memory_order_acq_rel);
  }
 }
 void pg_entry(size_t s,const unsigned char*e,Ref&out)const{
  out=Ref{};out.offset=get64(e);out.length=get32(e+8);out.version=get64(e+12);memcpy(out.hash.data(),e+20,8);
  if(out.offset<START+64||out.length>16*1024*1024||out.offset+out.length>pgb_[s].end||!out.version)pg_fail();
 }
 // Finds key in shard s's on-disk table.
 bool pg_find(size_t s,std::string_view key,uint64_t kh,Ref&out)const{
  const auto&b=pgb_[s];if(!b.p)return false;
  uint32_t mask=b.slots-1,tag=uint32_t(kh);
  for(uint32_t i=tag&mask,n=0;n<b.slots;i=(i+1)&mask,n++){
   uint64_t at=b.entries+8*uint64_t(i);pg_check(s,at,8);uint64_t slot=get64(b.p+at);
   if(!slot)return false;if(uint32_t(slot>>32)!=tag)continue;
   if(!uint32_t(slot))pg_fail();uint64_t off=uint32_t(slot)-1;
   if(off+2>b.entries)pg_fail();pg_check(s,off,2);size_t kn=size_t(b.p[off])|(size_t(b.p[off+1])<<8);
   if(!kn||off+2+kn+28>b.entries)pg_fail();pg_check(s,off+2,kn+28);
   if(kn!=key.size()||memcmp(b.p+off+2,key.data(),kn))continue;
   pg_entry(s,b.p+off+2+kn,out);return true;
  }
  return false;
 }
 // f(key, ref) for every entry of shard s's on-disk table.
 template<class F> void pg_each_base(size_t s,F&&f)const{
  const auto&b=pgb_[s];if(!b.p)return;pg_check(s,0,b.entries);
  uint64_t pos=0;Ref r;
  for(uint64_t j=0;j<b.count;j++){
   if(pos+2>b.entries)pg_fail();size_t kn=size_t(b.p[pos])|(size_t(b.p[pos+1])<<8);if(!kn||pos+2+kn+28>b.entries)pg_fail();
   std::string_view k(reinterpret_cast<const char*>(b.p+pos+2),kn);pg_entry(s,b.p+pos+2+kn,r);pos+=2+kn+28;f(k,static_cast<const Ref&>(r));
  }
  if(pos!=b.entries)pg_fail();
 }
 // Below the delta: the frozen delta of a running merge, then the on-disk table.
 bool lower_find(size_t s,std::string_view key,uint64_t kh,Ref&out)const{
  if(has_frozen_[s]&&frozen_[s].find(key,kh,out))return out.offset!=0;
  return pg_find(s,key,kh,out);
 }
 bool layered(size_t s)const{return has_frozen_[s]||pgb_[s].p;}
 // Applies a set (ref) or a deletion (ref null) to shard s's delta, keeping the counts.
 // lower: whether the key is live below the delta (-1: look it up). Returns the version
 // the key had (0: absent; when lower is given, 1 stands for any version below).
 uint64_t pg_apply(size_t s,std::string_view key,uint64_t kh,const Ref*ref,int lower){
  auto&d=shards_[s].index;Ref cur,lo;bool in_d=d.find(key,kh,cur);
  bool low=lower<0?lower_find(s,key,kh,lo):lower!=0;if(lower>=0)lo.version=1;
  bool live=in_d?cur.offset!=0:low;uint64_t was=in_d?(cur.offset?cur.version:0):(low?lo.version:0);
  if(ref){if(!live){index_count_++;live_[s]++;}d.put(key,kh,*ref);}
  else{if(live){index_count_--;live_[s]--;}if(low)d.put(key,kh,Ref{});else if(in_d)d.erase(key,kh);}
  pg_dirty_[s]=true;return was;
 }
 void pg_drop_base(size_t s){auto&b=pgb_[s];b.p=nullptr;b.hashes=nullptr;b.map.reset();b.checked.reset();b.count=b.entries=b.data=b.blocks=0;b.slots=0;b.end=START;b.trusted=false;}
 // Installs a mapped base section for shard s after checking its trailer.
 void pg_set_base(size_t s,std::unique_ptr<Mapping> map,const PgRec&rec,bool trusted){
  const unsigned char*p=static_cast<const unsigned char*>(map->address);uint64_t len=rec.base_len;
  require(p&&len>=32&&!memcmp(p+len-8,"KDBXSEC1",8),"CORRUPT index file section");
  uint64_t count=get64(p+len-32),entries=get64(p+len-24);uint32_t slots=get32(p+len-16);
  require(slots>=16&&!(slots&(slots-1))&&count==rec.base_count&&count*3<=uint64_t(slots)*2+2&&entries<UINT32_MAX-1,"CORRUPT index file section");
  uint64_t data=entries+8*uint64_t(slots),blocks=(data+PG_BLOCK-1)/PG_BLOCK;
  require(len==data+16*blocks+32&&count*PG_ENTRY<=entries,"CORRUPT index file section");
  if(!trusted){auto h=blake3_digest(p+data,size_t(16*blocks+32));require(h==rec.base_hash,"CORRUPT index file section hash");}
  auto&b=pgb_[s];pg_drop_base(s);
  b.checked.reset(new std::atomic<uint64_t>[size_t(blocks/64+1)]());
  b.map=std::move(map);b.p=p;b.count=count;b.entries=entries;b.slots=slots;b.data=data;b.blocks=blocks;b.hashes=p+data;b.end=rec.end;b.trusted=trusted;
 }
 static void pg_put_entry(Bytes&o,std::string_view k,const Ref&r){
  size_t at=o.size();o.resize(at+PG_ENTRY+k.size());unsigned char*e=o.data()+at;
  e[0]=(unsigned char)(k.size()&255);e[1]=(unsigned char)(k.size()>>8);memcpy(e+2,k.data(),k.size());e+=2+k.size();
  put64(e,r.offset);put32(e+8,r.length);put64(e+12,r.version);memcpy(e+20,r.hash.data(),8);
 }
 // Streams a section to the sidecar, hashing each 64 KB block as it goes.
 struct PgWriter{
  File f;uint64_t at,done=0;Bytes buf,hashes;static constexpr size_t CAP=1<<20;
  PgWriter(File f,uint64_t at):f(f),at(at){buf.reserve(CAP);}
  uint64_t pos()const{return done+buf.size();}
  void add(const void*p,size_t n){auto*c=static_cast<const unsigned char*>(p);while(n){size_t k=std::min(n,CAP-buf.size());buf.insert(buf.end(),c,c+k);c+=k;n-=k;if(buf.size()==CAP)flush(false);}}
  void flush(bool last){
   size_t n=last?buf.size():buf.size()/PG_BLOCK*PG_BLOCK;
   for(size_t o=0;o<n;o+=PG_BLOCK){auto h=blake3_digest(buf.data()+o,std::min<size_t>(PG_BLOCK,n-o));hashes.insert(hashes.end(),h.begin(),h.begin()+16);}
   write_at(f,buf.data(),n,at+done);done+=n;buf.erase(buf.begin(),buf.begin()+n);
  }
 };
 using Extents=std::vector<std::pair<uint64_t,uint64_t>>;
 Extents pg_used()const{Extents u;for(const auto&r:pgrec_){if(r.base_len)u.push_back({r.base_off,pg_round(r.base_len)});if(r.delta_len)u.push_back({r.delta_off,pg_round(r.delta_len)});}return u;}
 // First fit among the extents no durable header refers to.
 static uint64_t pg_alloc(uint64_t n,Extents&used){
  n=pg_round(n);std::sort(used.begin(),used.end());uint64_t at=PG_FIRST;
  for(const auto&u:used){if(u.first>=at+n)break;at=std::max(at,pg_round(u.first+u.second));}
  used.push_back({at,n});return at;
 }
 // Writes shard s's merged table (its base overlaid with the frozen delta). sc_m_ held.
 void pg_write_base(size_t s,PgRec&rec){
  const auto&fz=frozen_[s];uint64_t ent=pgb_[s].entries;
  fz.for_each([&](std::string_view k,const Ref&r){if(r.offset)ent+=PG_ENTRY+k.size();});
  require(ent<UINT32_MAX-1,"LIMIT index shard size");
  uint32_t slots=pg_slots(pgb_[s].count+fz.size()),mask=slots-1;uint64_t data=ent+8*uint64_t(slots);
  auto used=pg_used();uint64_t at=pg_alloc(data+16*((data+PG_BLOCK-1)/PG_BLOCK)+32,used);
  std::vector<uint64_t> table(slots,0);PgWriter w(sc_.n,at);uint64_t count=0;Bytes e;
  auto add=[&](std::string_view k,const Ref&r,uint64_t h){
   uint32_t i=uint32_t(h)&mask;while(table[i])i=(i+1)&mask;table[i]=(uint64_t(uint32_t(h))<<32)|(w.pos()+1);
   e.clear();pg_put_entry(e,k,r);w.add(e.data(),e.size());count++;};
  pg_each_base(s,[&](std::string_view k,const Ref&r){auto h=index_hash(key_,k);Ref t;if(!fz.find(k,h,t))add(k,r,h);});
  fz.for_each([&](std::string_view k,const Ref&r){if(r.offset)add(k,r,index_hash(key_,k));});
  uint64_t entries=w.pos();
  unsigned char v[8];for(auto x:table){put64(v,x);w.add(v,8);}
  w.flush(true);
  Bytes tail=std::move(w.hashes);size_t t=tail.size();tail.resize(t+32);
  put64(tail.data()+t,count);put64(tail.data()+t+8,entries);put32(tail.data()+t+16,slots);put32(tail.data()+t+20,0);memcpy(tail.data()+t+24,"KDBXSEC1",8);
  write_at(sc_.n,tail.data(),tail.size(),at+w.done);
  rec.base_off=at;rec.base_len=w.done+tail.size();rec.base_count=count;rec.base_hash=blake3_digest(tail.data(),tail.size());
 }
 // Sections first, then the header in the other slot, each flushed. sc_m_ held. uuid: the
 // database's, copied by the caller under the writer lock (meta_ changes with every commit).
 void pg_write_header(const std::array<PgRec,64>&recs,const std::array<unsigned char,16>&uuid){
  Bytes h(PG_SLOT,0);unsigned char*p=h.data();uint64_t gen=sc_gen_+1;
  memcpy(p,"KDBXIDX1",8);put64(p+8,gen);memcpy(p+16,uuid.data(),16);
  put64(p+32,key_.sip[0]);put64(p+40,key_.sip[1]);memcpy(p+48,key_.blake,32);put64(p+80,key_.index_seed);
  uint64_t lo=UINT64_MAX;
  for(size_t s=0;s<64;s++){
   const auto&r=recs[s];unsigned char*q=p+PG_TABLE+PG_REC*s;
   put64(q,r.base_off);put64(q+8,r.base_len);put64(q+16,r.base_count);put64(q+24,r.delta_off);put64(q+32,r.delta_len);put64(q+40,r.delta_count);
   put64(q+48,r.end);put64(q+56,r.txid);put64(q+64,r.live);memcpy(q+72,r.chain.data(),32);memcpy(q+104,r.base_hash.data(),32);memcpy(q+136,r.delta_hash.data(),32);
   lo=std::min(lo,r.end);
  }
  auto x=blake3_digest(p,PG_SLOT-32);memcpy(p+PG_SLOT-32,x.data(),32);
  sync_file(sc_.n);write_at(sc_.n,p,PG_SLOT,(gen%2)*PG_SLOT);sync_file(sc_.n);
  sc_gen_=gen;pgrec_=recs;pg_min_end_=lo;
 }
 // Starts using the sidecar (first time over the budget): a fresh, empty file whose
 // records say "empty at the start of the log" until checkpoints and merges advance them.
 bool pg_ready(){
  if(paged_)return true;
  try{
   if(!platform::valid(sc_.n)){File f;try{f=platform::open_file(sc_path(),false,false);}catch(...){f=platform::open_file(sc_path(),true,false);sync_parent(sc_path());}sc_.n=f;}
   platform::truncate(sc_.n,0);sc_gen_=0;for(auto&r:pgrec_)r=PgRec{};pg_min_end_=START;
   std::lock_guard<std::mutex>w(writer_);for(auto&d:pg_dirty_)d=true;paged_=true;return true;
  }catch(...){return false;}
 }
 // Puts back a frozen delta after a failed merge (the newer delta wins).
 void pg_unfreeze(size_t s){
  std::lock_guard<std::mutex>w(writer_);std::unique_lock<ReadWriteMutex>r(readers_);std::unique_lock<ReadWriteMutex>l(shards_[s].mutex);
  auto&d=shards_[s].index;
  try{frozen_[s].for_each([&](std::string_view k,const Ref&v){auto h=index_hash(key_,k);Ref t;if(!d.find(k,h,t))d.put(k,h,v);});}catch(...){failed_=true;}
  Index dead=std::move(frozen_[s]);has_frozen_[s]=false;pg_dirty_[s]=true;
 }
 // Merges shard s's delta into its on-disk table. sc_m_ held.
 void pg_merge(size_t s){
  PgRec rec=pgrec_[s];std::array<unsigned char,16> uuid{};
  {std::lock_guard<std::mutex>w(writer_);if(failed_)return;uuid=meta_.uuid;
   std::unique_lock<ReadWriteMutex>r(readers_);std::unique_lock<ReadWriteMutex>l(shards_[s].mutex);
   frozen_[s]=std::move(shards_[s].index);shards_[s].index=Index{};has_frozen_[s]=true;
   rec.end=meta_.end;rec.txid=meta_.txid;rec.chain=meta_.chain;rec.live=live_[s];pg_dirty_[s]=false;}
  std::unique_ptr<Mapping> map;
  try{
   pg_write_base(s,rec);rec.delta_off=rec.delta_len=rec.delta_count=0;rec.delta_hash=Hash{};
   map=std::make_unique<Mapping>(sc_.n,rec.base_off,size_t(rec.base_len));require(map->address,"IO cannot map the index file");
   auto recs=pgrec_;recs[s]=rec;pg_write_header(recs,uuid);
  }catch(...){pg_unfreeze(s);throw;}
  Index dead;
  {std::lock_guard<std::mutex>w(writer_);std::unique_lock<ReadWriteMutex>r(readers_);std::unique_lock<ReadWriteMutex>l(shards_[s].mutex);
   pg_set_base(s,std::move(map),rec,true);dead=std::move(frozen_[s]);has_frozen_[s]=false;}
 }
 // Records every shard at the current end of the log, writing the deltas changed since
 // their last record. Skipped while the oldest record is less than min_lag bytes behind.
 // sc_m_ held.
 void pg_checkpoint(uint64_t min_lag=0){
  if(!paged_)return;
  auto recs=pgrec_;std::vector<std::pair<size_t,Bytes>> out;std::array<unsigned char,16> uuid{};
  {std::lock_guard<std::mutex>w(writer_);if(failed_||verify_state_==3)return;uuid=meta_.uuid;
   uint64_t lo=UINT64_MAX;bool any=false;for(size_t s=0;s<64;s++){lo=std::min(lo,recs[s].end);any|=pg_dirty_[s]||recs[s].end!=meta_.end;}
   if(!any||meta_.end-lo<min_lag)return;
   for(size_t s=0;s<64;s++){
    if(pg_dirty_[s]){Bytes b;shards_[s].index.for_each([&](std::string_view k,const Ref&r){pg_put_entry(b,k,r);});recs[s].delta_count=shards_[s].index.size();out.push_back({s,std::move(b)});pg_dirty_[s]=false;}
    recs[s].end=meta_.end;recs[s].txid=meta_.txid;recs[s].chain=meta_.chain;recs[s].live=live_[s];
   }
  }
  try{
   auto used=pg_used();
   for(auto&o:out){auto&r=recs[o.first];
    if(o.second.empty()){r.delta_off=r.delta_len=0;r.delta_hash=Hash{};continue;}
    r.delta_off=pg_alloc(o.second.size(),used);r.delta_len=o.second.size();r.delta_hash=blake3_digest(o.second.data(),o.second.size());
    write_at(sc_.n,o.second.data(),o.second.size(),r.delta_off);
   }
   pg_write_header(recs,uuid);
  }catch(...){std::lock_guard<std::mutex>w(writer_);for(auto&o:out)pg_dirty_[o.first]=true;throw;}
 }
 // Replaces the paged index with one rebuilt from the log (the sidecar was damaged).
 // sc_m_ held; no merge is running.
 void pg_rebuild(){
  std::lock_guard<std::mutex>w(writer_);if(!paged_)return;
  Sharded rec;replay(fd_.n,meta_,rec,key_);
  std::unique_lock<ReadWriteMutex>r(readers_);std::array<std::unique_lock<ReadWriteMutex>,64> locks;for(size_t i=0;i<64;i++)locks[i]=std::unique_lock<ReadWriteMutex>(shards_[i].mutex);
  for(size_t s=0;s<64;s++){live_[s]=rec[s].size();shards_[s].index=std::move(rec[s]);pg_drop_base(s);frozen_[s].clear();has_frozen_[s]=false;pg_dirty_[s]=true;}
  index_count_=total(shards_);paged_=false;for(auto&x:pgrec_)x=PgRec{};
  // The next open must not use this file.
  try{Bytes z(2*PG_SLOT,0);write_at(sc_.n,z.data(),z.size(),0);sync_file(sc_.n);}catch(...){}
 }
 // Maintainer: rebuild after damage, merge while over the memory budget, and keep crash
 // recovery short (a checkpoint once the oldest record is 64 MB of log behind).
 void pg_run(){
  std::lock_guard<std::mutex>sc(sc_m_);
  if(pg_damaged_.exchange(false)){try{pg_rebuild();}catch(...){}}
  if(!options_.indexMemory)return;
  bool merged=false;
  for(int round=0;round<4096;round++){
   size_t pick=SIZE_MAX;
   {std::lock_guard<std::mutex>w(writer_);if(failed_||lazy_||verify_state_==3)return;
    size_t mem=0,best=0;for(const auto&sh:shards_)mem+=sh.index.approx_memory();
    if(mem<=(merged?options_.indexMemory/2:options_.indexMemory))break;
    for(size_t s=0;s<64;s++){size_t n=shards_[s].index.size();if(n>=std::max<size_t>(64,size_t(pgb_[s].count/32))&&n>best){best=n;pick=s;}}
   }
   if(pick==SIZE_MAX||!pg_ready())break;
   pg_merge(pick);merged=true;
  }
  if(paged_)pg_checkpoint(uint64_t(64)<<20);
 }
 // Opens the paged index from a sidecar that matches the log. False (state reset) if
 // there is none or any check fails; the caller then replays the whole log.
 bool pg_open(){
  opening_=true;
  try{
   File f;try{f=platform::open_file(sc_path(),false,false);}catch(...){opening_=false;return false;}
   sc_.n=f;uint64_t size=file_size(f);
   if(size>=2*PG_SLOT){
    auto heads=read_at(f,size_t(2*PG_SLOT),0);const unsigned char*best=nullptr;uint64_t gen=0;
    for(int i=0;i<2;i++){const unsigned char*p=heads.data()+i*PG_SLOT;
     if(memcmp(p,"KDBXIDX1",8)||memcmp(p+16,meta_.uuid.data(),16))continue;
     auto h=blake3_digest(p,PG_SLOT-32);if(memcmp(h.data(),p+PG_SLOT-32,32))continue;
     if(!best||get64(p+8)>gen){best=p;gen=get64(p+8);}}
    if(best){
     DigestKey k{};k.sip[0]=get64(best+32);k.sip[1]=get64(best+40);memcpy(k.blake,best+48,32);k.index_seed=get64(best+80);key_=k;
     std::array<PgRec,64> recs;uint64_t lo=UINT64_MAX;
     for(size_t s=0;s<64;s++){const unsigned char*q=best+PG_TABLE+PG_REC*s;auto&r=recs[s];
      r.base_off=get64(q);r.base_len=get64(q+8);r.base_count=get64(q+16);r.delta_off=get64(q+24);r.delta_len=get64(q+32);r.delta_count=get64(q+40);
      r.end=get64(q+48);r.txid=get64(q+56);r.live=get64(q+64);memcpy(r.chain.data(),q+72,32);memcpy(r.base_hash.data(),q+104,32);memcpy(r.delta_hash.data(),q+136,32);
      require(r.end>=START&&r.end<=meta_.end&&r.txid<=meta_.txid&&r.live<(uint64_t(1)<<40),"index file record");
      require(!r.base_len||(r.base_off>=PG_FIRST&&r.base_off%PG_ALIGN==0&&r.base_len<=size&&r.base_off<=size-r.base_len),"index file record");
      require(!r.delta_len||(r.delta_off>=PG_FIRST&&r.delta_len<=size&&r.delta_off<=size-r.delta_len&&r.delta_len<=(uint64_t(1)<<32)),"index file record");
      lo=std::min(lo,r.end);}
     for(size_t s=0;s<64;s++){const auto&r=recs[s];
      if(r.base_len){auto map=std::make_unique<Mapping>(f,r.base_off,size_t(r.base_len));require(map->address,"index file map");pg_set_base(s,std::move(map),r,false);}
      if(r.delta_len){
       auto d=read_at(f,size_t(r.delta_len),r.delta_off);require(blake3_digest(d.data(),d.size())==r.delta_hash,"index file delta hash");
       auto&idx=shards_[s].index;size_t p=0;idx.reserve(size_t(r.delta_count));
       for(uint64_t j=0;j<r.delta_count;j++){
        require(d.size()-p>=PG_ENTRY,"index file delta");size_t kn=size_t(d[p])|(size_t(d[p+1])<<8);require(kn&&kn<=1024&&d.size()-p>=PG_ENTRY+kn,"index file delta");
        std::string_view key(reinterpret_cast<const char*>(d.data()+p+2),kn);const unsigned char*e=d.data()+p+2+kn;p+=PG_ENTRY+kn;
        Ref v;v.offset=get64(e);v.length=get32(e+8);v.version=get64(e+12);memcpy(v.hash.data(),e+20,8);
        require(!v.offset||(v.offset>=START+64&&v.length<=16*1024*1024&&v.offset+v.length<=r.end&&v.version>=1&&v.version<=r.txid),"index file delta entry");
        auto kh=index_hash(key_,key);require(shard_of(kh)==s&&idx.put(key,kh,v),"index file delta entry");
       }
       require(p==d.size(),"index file delta");
      }
      live_[s]=size_t(r.live);
     }
     size_t n=0;for(auto c:live_)n+=c;index_count_=n;
     pgrec_=recs;sc_gen_=gen;pg_min_end_=lo;
     pg_replay_suffix();
     paged_=true;from_sidecar_=true;opening_=false;return true;
    }
   }
  }catch(...){}
  for(size_t s=0;s<64;s++){pg_drop_base(s);shards_[s].index.clear();live_[s]=0;pg_dirty_[s]=false;}
  index_count_=0;for(auto&r:pgrec_)r=PgRec{};pg_damaged_=false;opening_=false;
  return false;
 }
 // Replays the log after each shard's record, with the same checks as a full replay.
 void pg_replay_suffix(){
  std::array<size_t,64> order;for(size_t i=0;i<64;i++)order[i]=i;
  std::sort(order.begin(),order.end(),[&](size_t a,size_t b){return pgrec_[a].end<pgrec_[b].end;});
  const auto&first_rec=pgrec_[order[0]];uint64_t pos=first_rec.end,tx=first_rec.txid;Hash prev=first_rec.chain;
  if(pos==START)require(tx==0&&prev==Hash{},"index file start");
  else{auto t=read_at(fd_.n,32,pos-32);require(!memcmp(t.data(),prev.data(),32),"index file does not match the log");}
  size_t k=0;
  auto boundary=[&]{while(k<64&&pgrec_[order[k]].end<=pos){const auto&r=pgrec_[order[k]];require(r.end==pos&&r.txid==tx&&r.chain==prev,"index file does not match the log");k++;}};
  boundary();
  while(pos<meta_.end){
   require(meta_.end-pos>=96,"CORRUPT short frame");auto head=read_at(fd_.n,64,pos);require(is_group_magic(head.data()),"CORRUPT group magic");
   auto len=get64(head.data()+8),seq=get64(head.data()+16);uint32_t flags=get32(head.data()+24),count=get32(head.data()+28);
   // A compacted log's checkpoint: the full replay reads it in bounded pieces.
   require(!flags,"index file before a checkpoint group");require(seq==tx+1,"CORRUPT transaction sequence");
   require(len<=MAX_GROUP&&len<=meta_.end-pos-96,"CORRUPT frame length");require(count<=1000000,"CORRUPT count");require(!memcmp(head.data()+32,prev.data(),32),"CORRUPT chain");
   auto whole=read_at(fd_.n,size_t(64+len+32),pos);auto h=digest(whole.data(),size_t(64+len));require(!memcmp(h.data(),whole.data()+64+len,32),"CORRUPT frame hash");
   const unsigned char*b=whole.data();size_t p=64;
   for(uint32_t i=0;i<count;i++){
    require(p+17<=64+len,"CORRUPT operation header");auto kind=b[p];auto expect=get64(b+p+1);auto kn=get32(b+p+9),vn=get32(b+p+13);p+=17;
    require(kind<=1&&kn&&kn<=1024&&vn<=16*1024*1024&&uint64_t(p)+kn+vn<=64+len&&(!kind?vn==0:true),"CORRUPT operation bounds");
    std::string_view key(reinterpret_cast<const char*>(b+p),kn);p+=kn;auto kh=index_hash(key_,key);size_t s=shard_of(kh);
    if(pos>=pgrec_[s].end){
     Ref v;if(kind)v=Ref{pos+p,seq,vn,value_digest(key_,b+p,vn)};
     uint64_t was=pg_apply(s,key,kh,kind?&v:nullptr,-1);require(expect==ANY||expect==was,"CORRUPT expected version");
    }
    p+=vn;
   }
   require(p==64+len,"CORRUPT trailing operation bytes");
   pos+=96+len;tx=seq;prev=h;boundary();
  }
  require(k==64&&tx==meta_.txid&&prev==meta_.chain,"CORRUPT final boundary");
 }
 // Background verification of every on-disk table block (a damaged one triggers a rebuild).
 void pg_verify_blocks(){
  for(size_t s=0;s<64&&!stop_verify_;s++){
   std::shared_lock<ReadWriteMutex>l(shards_[s].mutex);
   try{if(pgb_[s].p)pg_check(s,0,pgb_[s].data);}catch(...){if(!pg_damaged_)throw;return;}
  }
 }
 // ---------- Ordered keys and secondary indexes ----------
 SetMemory set_mem_;std::unique_ptr<PageFile> set_file_;
 // Prepares a set for paging under the orderedMemory budget.
 void attach_set(SortedSet&s){
  if(options_.orderedMemory&&!set_file_)set_file_=std::make_unique<PageFile>(path_+".sets");
  s.attach(options_.orderedMemory?set_file_.get():nullptr,&set_mem_);
 }
 size_t sort_budget()const{return options_.orderedMemory?std::max<size_t>(options_.orderedMemory/2,size_t(1)<<20):SIZE_MAX/4;}
 // Keeps the sets' RAM within orderedMemory: the least recently changed pages go to the
 // scratch file. Writer lock and readers_ exclusively held (no reader holds a page).
 void trim_sets_locked(){
  if(!options_.orderedMemory||!set_file_)return;
  if(set_mem_.bytes.load()>int64_t(options_.orderedMemory)){
   std::vector<SortedSet*> sets;if(ordered_built_)sets.push_back(&ordered_);for(auto&ix:sec_)if(ix->built)sets.push_back(&ix->set);
   struct C{uint64_t t;SortedSet*s;size_t c;};std::vector<C> cand;
   for(auto*s:sets)s->each_ram_page([&](uint64_t t,size_t c){cand.push_back({t,s,c});});
   std::sort(cand.begin(),cand.end(),[](const C&a,const C&b){return a.t<b.t;});
   int64_t target=int64_t(options_.orderedMemory/2);
   for(const auto&c:cand){if(set_mem_.bytes.load()<=target)break;c.s->spill_page(c.c,false);}
   set_file_->publish();
  }
  set_file_->release_retired();
 }
public:
 // Bytes of RAM held by ordered keys and secondary indexes, and of their scratch file.
 size_t ordered_memory()const{return size_t(std::max<int64_t>(0,set_mem_.bytes.load()));}
 uint64_t ordered_file_bytes()const{std::shared_lock<ReadWriteMutex>r(readers_);return set_file_?set_file_->bytes():0;}
 // Bytes of RAM held by the key index (the in-memory part; mapped pages are the OS's).
 size_t index_memory()const{
  size_t n=0;for(size_t s=0;s<64;s++){std::shared_lock<ReadWriteMutex>l(shards_[s].mutex);n+=shards_[s].index.approx_memory();if(has_frozen_[s])n+=frozen_[s].approx_memory();}
  return n;
 }
 // Bytes of the on-disk index file (0 when the whole index is in memory).
 uint64_t index_file_bytes()const{std::lock_guard<std::mutex>sc(sc_m_);if(!paged_||!platform::valid(sc_.n))return 0;try{return file_size(sc_.n);}catch(...){return 0;}}
 // Moves every change held in memory to the on-disk index now (also starts paging).
 void flush_index(){
  std::lock_guard<std::mutex>sc(sc_m_);
  {std::shared_lock<ReadWriteMutex>r(readers_);healthy();}
  {std::lock_guard<std::mutex>w(writer_);materialize();}
  if(!pg_ready())throw std::runtime_error("IO cannot create the index file");
  for(size_t s=0;s<64;s++){bool any;{std::lock_guard<std::mutex>w(writer_);any=!shards_[s].index.empty();}if(any)pg_merge(s);}
  pg_checkpoint();
 }
private:

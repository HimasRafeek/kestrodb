// Included inside class Store (kestro.hpp): online backups, continuous archiving and
// point-in-time restore.
//
// The log is append-only: bytes before the committed boundary never change. A backup
// therefore reads the commit slot under a short lock and copies the committed prefix
// while commits continue. A copy is checked by streaming its chained groups (SHA-256 and
// chain), in bounded memory.
//
// Archive: a directory with archive.kdb, itself a valid database holding every committed
// transaction archived so far, and timeline, one line per archive step: "unix_ms txid end".
// Each step appends only the groups committed since the last one. restore() cuts a backup
// or the archive at any transaction (or the last archived point at or before a time).
public:
 struct LogPoint {uint64_t txid=0,end=START;};
 // Checks the chained groups of fd from pos (with transaction tx and chain prev there) to
 // m: magic, sequence, chain and SHA-256 of every group, read in 1 MB pieces.
 static void verify_log_range(File fd,uint64_t pos,uint64_t tx,Hash prev,const Meta&m){
  bool first=pos==START;Bytes buf(size_t(1)<<20);
  while(pos<m.end){
   require(m.end-pos>=96,"CORRUPT short frame");auto head=read_at(fd,64,pos);const unsigned char*h=head.data();
   require(is_group_magic(h),"CORRUPT group magic");
   auto len=get64(h+8),seq=get64(h+16);uint32_t flags=get32(h+24),count=get32(h+28);
   require(flags<=1&&(!flags||first),"CORRUPT checkpoint location");require(flags?seq>=tx:seq==tx+1,"CORRUPT transaction sequence");
   require((flags||len<=MAX_GROUP)&&len<=m.end-pos-96,"CORRUPT frame length");require(flags||count<=1000000,"CORRUPT count");require(!memcmp(h+32,prev.data(),32),"CORRUPT chain");
   StreamHash sh;for(uint64_t off=0,n=64+len;off<n;){size_t k=size_t(std::min<uint64_t>(buf.size(),n-off));platform::read_exact(fd,buf.data(),k,pos+off);sh.update(buf.data(),k);off+=k;}
   auto d=sh.finish();auto stored=read_at(fd,32,pos+64+len);require(!memcmp(d.data(),stored.data(),32),flags?"CORRUPT checkpoint hash":"CORRUPT frame hash");
   pos+=96+len;tx=seq;prev=d;first=false;
  }
  require(pos==m.end&&tx==m.txid&&prev==m.chain,"CORRUPT final boundary");
 }
 // Online backup: a verified copy of the committed state at dest (a new file). Commits
 // continue while it is copied; the copy holds every transaction acknowledged before the
 // call. Returns the point copied.
 LogPoint backup(const std::string&dest)const{
  Meta m;{std::shared_lock<ReadWriteMutex>r(readers_);healthy();m=meta_;}
  write_copy(fd_.n,dest,m,m);
  return {m.txid,m.end};
 }
 // Appends to the archive in dir what was committed since the last step (the first step
 // copies everything). A source that no longer continues the archive (replaced by a
 // compacted copy or a restore) starts a new archive; the old one is kept, renamed.
 LogPoint archive(const std::string&dir){
  std::lock_guard<std::mutex>g(archive_m_);
  Meta m;{std::shared_lock<ReadWriteMutex>r(readers_);healthy();m=meta_;}
  std::filesystem::create_directories(std::filesystem::u8path(dir));
  const std::string path=dir+"/archive.kdb";
  if(std::filesystem::exists(std::filesystem::u8path(path))){
   bool ok=false;Meta am;
   {FD a(platform::open_file(path,false,false));
    try{am=read_meta(a.n);ok=am.uuid==m.uuid&&am.end<=m.end&&am.txid<=m.txid&&file_size(a.n)>=am.end;
     if(ok&&am.end>START){auto t=read_at(fd_.n,32,am.end-32);ok=!memcmp(t.data(),am.chain.data(),32);}
    }catch(...){ok=false;}
    if(ok){
     if(am.end==m.end)return {am.txid,am.end};
     copy_range(fd_.n,a.n,am.end,m.end);sync_file(a.n);
     verify_log_range(a.n,am.end,am.txid,am.chain,m);
     Meta n=m;n.generation=am.generation+1;auto h=meta_bytes(n);write_at(a.n,h.data(),h.size(),(n.generation%2)*4096);sync_file(a.n);
    }
   }
   if(ok){append_timeline(dir,m);return {m.txid,m.end};}
   // Keep the old archive under a new name and start again.
   auto stamp=std::to_string(now_ms());
   std::filesystem::rename(std::filesystem::u8path(path),std::filesystem::u8path(dir+"/archive-"+stamp+".kdb"));
   auto tl=std::filesystem::u8path(dir+"/timeline");
   if(std::filesystem::exists(tl))std::filesystem::rename(tl,std::filesystem::u8path(dir+"/timeline-"+stamp));
  }
  write_copy(fd_.n,path,m,m);
  append_timeline(dir,m);
  return {m.txid,m.end};
 }
 // A new database at dest from source (a database, backup or archive.kdb) cut after
 // transaction txid (0: everything source holds). at_ms >= 0 instead picks the last
 // archived point at or before that time (unix milliseconds), from source's timeline.
 static LogPoint restore(const std::string&source,const std::string&dest,uint64_t txid=0,int64_t at_ms=-1){
  FD in(platform::open_file(source,false,false));
  bool rw=false;Meta m=recover_meta(in.n,rw);
  if(at_ms>=0){
   auto tl=std::filesystem::u8path(source).parent_path()/"timeline";
   std::ifstream f(tl);require(bool(f),"INPUT no timeline next to "+source+" (time-based restore needs an archive)");
   long long ms;unsigned long long tx,end;bool found=false;
   while(f>>ms>>tx>>end)if(ms<=at_ms){txid=tx;found=true;}
   require(found,"INPUT no archived point at or before that time");
   if(!txid)txid=UINT64_MAX;  // the archive started empty: restore an empty database
  }
  Meta target=m;
  if(txid==UINT64_MAX){target.end=START;target.txid=0;target.chain=Hash{};}
  else if(txid&&txid!=m.txid){
   require(txid<m.txid,"INPUT "+source+" ends at transaction "+std::to_string(m.txid));
   uint64_t pos=START;bool hit=false;
   while(pos<m.end){
    auto head=read_at(in.n,64,pos);require(is_group_magic(head.data()),"CORRUPT group magic");
    auto len=get64(head.data()+8),seq=get64(head.data()+16);uint32_t flags=get32(head.data()+24);
    require(!(flags&&seq>txid),"INPUT transaction "+std::to_string(txid)+" is inside the compacted checkpoint at the start of "+source+"; restore needs a point after "+std::to_string(seq));
    pos+=96+len;
    if(seq==txid){target.end=pos;target.txid=seq;auto c=read_at(in.n,32,pos-32);memcpy(target.chain.data(),c.data(),32);hit=true;break;}
   }
   require(hit,"INPUT transaction "+std::to_string(txid)+" not found");
  }
  target.generation=1;
  write_copy(in.n,dest,target,target);
  return {target.txid,target.end};
 }
 // Archive points recorded so far (oldest first): unix ms, txid, end.
 static std::vector<std::array<uint64_t,3>> timeline(const std::string&dir){
  std::vector<std::array<uint64_t,3>> out;std::ifstream f(std::filesystem::u8path(dir)/"timeline");
  unsigned long long ms,tx,end;while(f>>ms>>tx>>end)out.push_back({ms,tx,end});return out;
 }
 static uint64_t now_ms(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());}
private:
 std::mutex archive_m_;
 static void copy_range(File in,File out,uint64_t from,uint64_t to){
  Bytes b(size_t(4)<<20);
  for(uint64_t off=from;off<to;){size_t n=size_t(std::min<uint64_t>(b.size(),to-off));platform::read_exact(in,b.data(),n,off);write_at(out,b.data(),n,off);off+=n;}
 }
 // Writes [START, m.end) of in plus a commit slot for m to dest through a temporary file,
 // flushed and verified before it is published.
 static void write_copy(File in,const std::string&dest,const Meta&m,const Meta&slot){
  auto temp=temp_name(dest);bool created=false;
  try{
   FD out(platform::open_file(temp,true));created=true;
   Bytes headers(START,0);auto h=meta_bytes(slot);memcpy(headers.data()+(slot.generation%2)*4096,h.data(),4096);write_at(out.n,headers.data(),headers.size(),0);
   copy_range(in,out.n,START,m.end);sync_file(out.n);
   verify_log_range(out.n,START,0,Hash{},m);
   publish(temp,dest);created=false;
  }catch(...){if(created)platform::remove_noexcept(temp);throw;}
 }
 static void append_timeline(const std::string&dir,const Meta&m){
  auto p=dir+"/timeline";File f;
  try{f=platform::open_file(p,false,false);}catch(...){f=platform::open_file(p,true,false);}
  FD fd(f);auto line=std::to_string(now_ms())+" "+std::to_string(m.txid)+" "+std::to_string(m.end)+"\n";
  write_at(fd.n,line.data(),line.size(),file_size(fd.n));sync_file(fd.n);
 }

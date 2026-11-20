#include "kestro.hpp"
#include "reader.hpp"
#include <iostream>
#include <set>
#include <map>
#include <unordered_map>
#include <thread>
#include <functional>
#include <chrono>
#ifdef _WIN32
#include <winioctl.h>
#endif
using namespace kestro;
static int passed=0;
static void expect_error(const std::function<void()>&fn,const char*contains=""){bool hit=false;try{fn();}catch(const std::exception&e){hit=true;require(std::string(e.what()).find(contains)!=std::string::npos,"unexpected error");}require(hit,"expected failure");}
static void test(const char*name,const std::function<void()>&fn){fn();std::cout<<"PASS "<<name<<std::endl;++passed;}
static std::string hex(const Hash&h){std::string out;for(auto c:h){out.push_back("0123456789abcdef"[c>>4]);out.push_back("0123456789abcdef"[c&15]);}return out;}
int main(){
 auto root=std::filesystem::temp_directory_path()/std::filesystem::u8path("kestro-portable-"+std::to_string(platform::process_id())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 try{require(std::filesystem::create_directory(root),"test directory exists");auto file=[&](const std::string&n){return (root/std::filesystem::u8path(n)).u8string();};
 test("SHA-256 known answers and reusable streaming contexts",[&]{
  const char*empty="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  require(hex(digest("",0))==empty,"SHA empty");require(hex(digest("abc",3))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA abc");require(hex(digest("",0))==empty,"SHA reset");
  StreamHash h;h.update("a",1);h.update("bc",2);require(h.finish()==digest("abc",3),"stream split");require(hex(h.finish())==empty,"stream reuse");std::string million(1000000,'a');require(hex(digest(million.data(),million.size()))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","SHA million");
 });
 test("33 official BLAKE3 vectors",[&]{
 #include "blake3_vectors.inc"
 });
 test("key index matches std::unordered_map under random inserts, overwrites and erases",[&]{
  // Second pass uses a hash with 3 significant bits: long probe runs that wrap around
  // the table, which is where backward-shift deletion would break.
  for(int pass=0;pass<2;pass++){
   KeyIndex<uint64_t> idx;std::unordered_map<std::string,uint64_t> ref;uint64_t rng=0x9e3779b97f4a7c15ULL+pass;
   auto hash=[&](const std::string&k){uint64_t h=index_hash(digest_key(),k);return pass?(h&7)*0x1000000000000001ULL:h;};
   for(int step=0;step<200000;step++){
    rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;std::string k="k"+std::to_string(rng%(pass?300:5000));auto h=hash(k);
    if(rng%3==0){require(idx.erase(k,h)==ref.erase(k),"erase result");}
    else{bool fresh=idx.insert_or_assign(k,h,rng);bool refFresh=ref.insert_or_assign(k,rng).second;require(fresh==refFresh,"insert result");}
    if(step%997==0){require(idx.size()==ref.size(),"size");for(auto&[rk,rv]:ref){auto*f=idx.find(rk,hash(rk));require(f&&*f==rv,"lookup");}
     size_t seen=0;idx.for_each([&](std::string_view k2,const uint64_t&v,uint64_t){auto it=ref.find(std::string(k2));require(it!=ref.end()&&it->second==v,"iteration");seen++;});require(seen==ref.size(),"iteration count");}
   }
  }
 });
 test("mapped and streaming replay agree, and both reject corruption",[&]{
  ignore_index_snapshot=true;struct Restore{~Restore(){ignore_index_snapshot=false;}}restore;
  auto p=file("replay-modes"),q=file("replay-modes-compact");
  {Store db(p,true);for(int i=0;i<300;i++)db.commit({{"k"+std::to_string(i%40),std::string(100+i,'a'+i%26),ANY,false}});db.commit({{"k3","",ANY,true},{"k4","x",ANY,false}});db.commit({{"k4","y",2*0+db.get("k4").version,false}});db.snapshot(q,true);}
  for(auto path:{p,q}){std::vector<std::string> seen[2];
   for(int mode=0;mode<2;mode++){force_streaming_replay=mode==1;Store db(path);db.verify();for(int i=0;i<40;i++){auto v=db.get("k"+std::to_string(i));seen[mode].push_back(v.found?std::to_string(v.version)+":"+v.value:"-");}}
   force_streaming_replay=false;require(seen[0]==seen[1]&&seen[0][3]=="-"&&seen[0][4].substr(seen[0][4].find(':')+1)=="y","replay modes differ");}
  for(int kind=0;kind<2;kind++)for(int mode=0;mode<2;mode++){
   auto c=file("replay-corrupt"+std::to_string(kind)+std::to_string(mode));{Store db(c,true);for(int i=0;i<50;i++)db.commit({{"k"+std::to_string(i),std::string(200,'v'),ANY,false}});}
   {FD fd(platform::open_file(c));unsigned char bad='X';write_at(fd.n,&bad,1,kind?START+3:START+5000);}
   force_streaming_replay=mode==1;expect_error([&]{Store db(c);},"CORRUPT");force_streaming_replay=false;
  }
 });
 test("index snapshot: fast reopen matches replay; stale, damaged or absent snapshots fall back",[&]{
  auto p=file("snapshot");
  {Store db(p,true);for(int i=0;i<500;i++)db.commit({{"k"+std::to_string(i%120),std::string(50+i,'a'+i%26),ANY,false}});db.commit({{"k7","",ANY,true}});}
  auto committed=[&]{Store db(p);return db.bytes();};
  auto dump=[&](Store&db){std::vector<std::string> v;for(int i=0;i<120;i++){auto x=db.get("k"+std::to_string(i));v.push_back(x.found?std::to_string(x.version)+":"+x.value:"-");}return v;};
  std::vector<std::string> expected;{ignore_index_snapshot=true;Store db(p);expected=dump(db);ignore_index_snapshot=false;require(std::string(db.opened_from())=="log-replay","replay open");}
  require(std::filesystem::file_size(std::filesystem::u8path(p))>committed(),"snapshot written on close");
  {Store db(p);require(std::string(db.opened_from())=="index-snapshot","snapshot open");require(dump(db)==expected&&db.count()==119&&db.txid()==501,"snapshot contents");db.wait_for_verification();require(std::string(db.verification())=="passed","background verification");db.verify();
   db.commit({{"k1","after",ANY,false}});require(db.get("k1").value=="after","commit after snapshot open");expected[1]=std::to_string(db.get("k1").version)+":after";}
  {Store db(p);require(std::string(db.opened_from())=="index-snapshot","snapshot rewritten after commit");require(dump(db)==expected,"contents after commit");db.verify();}
  // Damaged snapshot: one flipped byte anywhere in it means a replay instead.
  // Each replay draws a new hash seed, so the next snapshot's size can differ: offsets are
  // taken from the current file every time.
  auto end=committed();
  for(int64_t rel:{int64_t(3),int64_t(200),int64_t(-40),int64_t(-1)}){
   {Store db(p);}uint64_t size=std::filesystem::file_size(std::filesystem::u8path(p));uint64_t off=rel>=0?end+uint64_t(rel):size-uint64_t(-rel);
   {FD fd(platform::open_file(p));unsigned char c;platform::read_exact(fd.n,&c,1,off);c^=0x40;write_at(fd.n,&c,1,off);}
   // Damage to the header, table or trailer means a replay at open. Damage inside a section
   // is found when that section is first used (here at the latest by verify()), and the
   // index is then rebuilt from the log.
   {Store db(p);bool replayed=std::string(db.opened_from())=="log-replay";require(dump(db)==expected,"contents after damaged snapshot");db.verify();
    require(replayed||std::string(db.index_mode())=="memory","damaged section rebuilt from the log");}
  }
  // Older readers truncate the snapshot as an uncommitted tail; the file still opens by replay.
  {Store db(p);}{FD fd(platform::open_file(p));platform::truncate(fd.n,end);}
  {Store db(p);require(std::string(db.opened_from())=="log-replay"&&dump(db)==expected,"no snapshot");}
 });
 test("lazy index: reads, verify, backup and compaction from the mapped snapshot; first write switches safely under readers",[&]{
  auto p=file("lazy");std::string v(300,'z');
  {Store db(p,true);for(int i=0;i<2000;i+=100){std::vector<Op> ops;for(int j=i;j<i+100;j++)ops.push_back({"k"+std::to_string(j),v+std::to_string(j),ANY,false});db.commit(ops);}db.commit({{"k5","",ANY,true}});}
  {Store db(p);require(std::string(db.index_mode())=="mapped","lazy open");
   require(db.count()==1999&&!db.get("k5").found&&db.get("k1999").value==v+"1999"&&db.get("k7").version==1,"lazy reads");
   auto many=db.getMany({"k1","k5","nope","k1"});require(many[0].value==v+"1"&&!many[1].found&&!many[2].found&&many[3].version==many[0].version,"lazy getMany");
   db.verify();db.snapshot(file("lazy-backup"));db.snapshot(file("lazy-compact"),true);require(std::string(db.index_mode())=="mapped","still lazy after maintenance");}
  for(auto q:{file("lazy-backup"),file("lazy-compact")}){Store c(q);require(c.count()==1999&&c.get("k1234").value==v+"1234","copies from lazy store");c.verify();}
  {Store db(p);std::atomic<bool> stop{false},bad{false};std::vector<std::thread> readers;
   for(int t=0;t<8;t++)readers.emplace_back([&,t]{try{for(int n=0;!stop;n++){int i=(n*31+t)%2000;if(i==5)continue;auto x=db.get("k"+std::to_string(i));if(!x.found||x.value.compare(0,300,v))bad=true;}}catch(...){bad=true;}});
   std::this_thread::sleep_for(std::chrono::milliseconds(20));
   db.commit({{"k5","back",0,false}});require(std::string(db.index_mode())=="memory","materialized");
   std::this_thread::sleep_for(std::chrono::milliseconds(20));stop=true;for(auto&t:readers)t.join();
   require(!bad,"readers saw a bad value across the switch");require(db.get("k5").value=="back"&&db.count()==2000,"after switch");db.verify();}
 });
 test("damaged snapshot section: readers rebuild the index from the log, under concurrency",[&]{
  auto p=file("section-damage");std::string v(200,'s');uint64_t end;
  {Store db(p,true);std::vector<Op> ops;for(int i=0;i<3000;i++)ops.push_back({"k"+std::to_string(i),v+std::to_string(i),ANY,false});db.commit(ops);end=db.bytes();}
  // Section 10's offset and length are in the snapshot table (160-byte header, 48-byte entries).
  {FD fd(platform::open_file(p));auto entry=read_at(fd.n,16,end+160+48*10);uint64_t off=get64(entry.data()),len=get64(entry.data()+8);
   require(len>200,"section size");uint64_t at=end+off+len-5;unsigned char c;platform::read_exact(fd.n,&c,1,at);c^=0x55;write_at(fd.n,&c,1,at);}
  Store db(p);require(std::string(db.opened_from())=="index-snapshot"&&std::string(db.index_mode())=="mapped","lazy open of damaged snapshot");
  std::atomic<bool> bad{false};std::vector<std::thread> readers;
  for(int t=0;t<8;t++)readers.emplace_back([&,t]{try{for(int i=t;i<3000;i+=8){auto x=db.get("k"+std::to_string(i));if(!x.found||x.value!=v+std::to_string(i))bad=true;}}catch(...){bad=true;}});
  for(auto&t:readers)t.join();
  require(!bad,"reads across the rebuild");require(std::string(db.index_mode())=="memory"&&db.count()==3000,"rebuilt from the log");db.verify();
  db.commit({{"k1","after",ANY,false}});require(db.get("k1").value=="after","write after rebuild");
 });
 test("idle checkpoint: after a crash the next open still skips the replay, unless commits followed",[&]{
  auto p=file("idle");Store db(p,true);
  for(int i=0;i<100;i++)db.commit({{"k"+std::to_string(i),std::string(100,'x'),ANY,false}});
  // A crash, simulated by copying the file as it is on disk while the store stays open.
  auto crash_copy=[&](const std::string&q){FD in(platform::open_file(p));auto bytes=read_at(in.n,size_t(file_size(in.n)),0);FD out(platform::open_file(q,true));write_at(out.n,bytes.data(),bytes.size(),0);};
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  crash_copy(file("idle-copy1"));
  {Store c(file("idle-copy1"));require(std::string(c.opened_from())=="index-snapshot","checkpoint used after crash");require(c.count()==100&&c.get("k99").value==std::string(100,'x'),"checkpoint contents");c.wait_for_verification();require(std::string(c.verification())=="passed","checkpoint verified");}
  // A commit after the checkpoint: the copy must replay and include it.
  db.commit({{"k0","newer",ANY,false}});crash_copy(file("idle-copy2"));
  {Store c(file("idle-copy2"));require(std::string(c.opened_from())=="log-replay","stale checkpoint ignored");require(c.get("k0").value=="newer","commit after checkpoint kept");}
 });
 test("snapshot open still detects corrupted log data: in the background, or at open on request",[&]{
  for(int mode=0;mode<2;mode++){
   auto p=file("snapshot-corrupt"+std::to_string(mode));
   {Store db(p,true);for(int i=0;i<50;i++)db.commit({{"k"+std::to_string(i),std::string(200,'v'),ANY,false}});}
   {FD fd(platform::open_file(p));unsigned char bad='X';write_at(fd.n,&bad,1,START+5000);}
   if(mode==0){Store db(p);require(std::string(db.opened_from())=="index-snapshot","opened from snapshot");db.wait_for_verification();require(std::string(db.verification())=="failed","corruption found");expect_error([&]{db.get("k1");},"CORRUPT");expect_error([&]{db.commit({{"x","y",ANY,false}});},"CORRUPT");}
   else{Options o;o.verifyOnOpen=true;expect_error([&]{Store db(p,false,o);},"CORRUPT");}
  }
 });
 test("SipHash-2-4 reference vectors and keyed point-read digests",[&]{
  // Key 00..0f. SipHash paper appendix A (15-byte message), and the reference
  // implementation's empty-message vectors for 64- and 128-bit output.
  uint64_t k0=0x0706050403020100ULL,k1=0x0f0e0d0c0b0a0908ULL;unsigned char m[16];for(int i=0;i<16;i++)m[i]=(unsigned char)i;
  require(sip::hash64(k0,k1,m,15)==0xa129ca6149be45e5ULL,"SipHash paper vector");require(sip::hash64(k0,k1,m,0)==0x726fdb47dd0e0e31ULL,"SipHash empty");
  unsigned char o[16];sip::hash128(k0,k1,m,0,o);std::string h;for(auto c:o){h.push_back("0123456789abcdef"[c>>4]);h.push_back("0123456789abcdef"[c&15]);}
  require(h=="a3817f04ba25a8e66df67214c7550293","SipHash-128 empty");
  // The point-read check (first 8 bytes only) agrees with the full digest, unaligned too.
  {std::string buf(4200,'\0');for(size_t i=0;i<buf.size();i++)buf[i]=char(i*131+7);
   for(size_t n:{size_t(0),size_t(1),size_t(7),size_t(8),size_t(15),size_t(226),SIPHASH_LIMIT,SIPHASH_LIMIT+1,size_t(4000)})for(size_t off:{size_t(0),size_t(3)}){
    const char*q=buf.data()+off;auto d=value_digest(digest_key(),q,n);require(value_matches(digest_key(),q,n,d),"value_matches agrees");
    auto bad=d;bad[0]^=1;require(!value_matches(digest_key(),q,n,bad),"value_matches rejects");
    if(n<=SIPHASH_LIMIT){unsigned char full[16];sip::hash128(digest_key().sip[0],digest_key().sip[1],q,n,full);uint64_t lo=sip::hash128_low(digest_key().sip[0],digest_key().sip[1],q,n);for(int i=0;i<8;i++)require(full[i]==(unsigned char)(lo>>(8*i)),"hash128_low");}}}
  // Both digest paths (SipHash up to 2 KiB, keyed BLAKE3 above) detect one changed byte.
  for(size_t n:{size_t(0),size_t(1),size_t(226),SIPHASH_LIMIT,SIPHASH_LIMIT+1,size_t(100000)}){std::string v(n,'a');auto d=value_digest(digest_key(),v.data(),n);require(d==value_digest(digest_key(),v.data(),n),"digest deterministic");if(n){v[n/2]='b';require(d!=value_digest(digest_key(),v.data(),n),"digest detects change");}}
  auto p=file("digest");std::string small(300,'s'),large(9000,'L');{Store db(p,true);db.commit({{"small",small,0,false},{"large",large,0,false}});}
  for(auto mode:{ReadMode::Pread,ReadMode::Mapped}){Options o2;o2.reads=mode;Store db(p,false,o2);require(db.get("small").value==small&&db.get("large").value==large,"digest read");}
 });
 test("binary data, atomic conflicts, versions and reopen",[&]{auto p=file("basic");std::string bytes;for(int i=0;i<256;i++)bytes.push_back(char(i));
  {Store db(p,true);require(db.commit({{"a",bytes,0,false},{"b","two",0,false}})==1,"txid");expect_error([&]{db.commit({{"new","no",ANY,false},{"a","bad",0,false}});},"CONFLICT");require(!db.get("new").found&&db.get("a").value==bytes,"atomic conflict");db.commit({{"b","",1,true},{"empty","",0,false}});db.verify();}
  Store db(p);require(db.get("a").value==bytes&&db.get("a").version==1&&!db.get("b").found&&db.get("empty").found,"reopen");auto values=db.getMany({"empty","missing","a","a"});require(values.size()==4&&values[0].found&&!values[1].found&&values[2].value==bytes&&values[3].version==1,"getMany");
 });
 test("kernel ownership lock blocks identical and hardlink paths",[&]{auto p=file("lock"),alias=file("alias");{Store db(p,true);expect_error([&]{Store other(p);},"LOCKED");platform::hardlink(p,alias);expect_error([&]{Store other(alias);},"LOCKED");}Store reopened(alias);reopened.verify();});
 test("Unicode and spaces in source and snapshot paths",[&]{auto p=file(u8"日本語 தமிழ் 🌿 data.kdb"),q=file(u8"保存 نسخة 🌿.kdb");{Store db(p,true);db.commit({{u8"キー",u8"வணக்கம் 🌿",0,false}});db.snapshot(q);}Store db(q);require(db.get(u8"キー").value==u8"வணக்கம் 🌿","Unicode data");});
 test("long paths beyond 260 characters",[&]{auto longdir=root;for(int i=0;i<6;i++)longdir/=std::string(48,'a'+i);std::filesystem::create_directories(longdir);auto p=(longdir/"long.kdb").u8string();require(p.size()>300,"test path too short");{Store db(p,true);db.commit({{"key","long path",0,false}});}{Store db(p);require(db.get("key").value=="long path","long path reopen");}
  // Remove the >260-character tree through extended-length platform calls. MinGW's
  // std::filesystem::remove_all spins forever on this tree instead of failing.
  platform::remove(p);for(auto d=longdir;d!=root;d=d.parent_path())platform::remove_directory(d.u8string());});
 test("read policies and ordered synchronization round trip",[&]{int n=0;for(auto mode:{ReadMode::Pread,ReadMode::Mapped}){if(mode==ReadMode::Mapped&&!platform::mapped_supported)continue;for(bool ordered:{false,true}){Options opts;opts.reads=mode;opts.orderedSync=ordered;auto p=file("policy"+std::to_string(n++));{Store db(p,true,opts);db.commit({{"k","v",0,false}});require(db.get("k").value=="v","policy read");}Store db(p,false,opts);db.verify();}}if(!platform::mapped_supported){Options o;o.reads=ReadMode::Mapped;expect_error([&]{Store db(file("unsupported"),true,o);},"UNSUPPORTED");require(!std::filesystem::exists(root/"unsupported"),"unsupported mode created file");}});
 test("invalid input cannot partially publish",[&]{Store db(file("invalid"),true);expect_error([&]{db.commit({{"a","1",ANY,false},{"a","2",ANY,false}});});expect_error([&]{db.commit({{"","1",ANY,false}});});expect_error([&]{db.getMany(std::vector<std::string>(4097,"a"));});require(db.count()==0&&db.txid()==0,"invalid write published");});
 test("backups and compacted checkpoints preserve versions; no overwrite",[&]{Store db(file("snap-source"),true);for(int i=0;i<20;i++)db.commit({{"key",std::string(4096,'a'+i),ANY,false}});db.commit({{"removed","v",0,false}});db.commit({{"removed","",ANY,true}});for(bool compact:{false,true}){auto p=file(compact?"compact":"backup");db.snapshot(p,compact);Store copy(p);require(copy.txid()==db.txid()&&copy.get("key").version==20&&!copy.get("removed").found,"snapshot versions");if(compact)require(copy.bytes()<db.bytes()/5,"history not compacted");expect_error([&]{db.snapshot(p,compact);});copy.commit({{"key","changed",20,false}});copy.verify();}});
 test("uncommitted tails truncated after validating committed data",[&]{auto p=file("tail");uint64_t end;{Store db(p,true);db.commit({{"key","safe",0,false}});end=db.bytes();}{FD fd(platform::open_file(p));std::string junk(500,'?');write_at(fd.n,junk.data(),junk.size(),end);}Store db(p);require(db.bytes()==end&&db.get("key").value=="safe","tail recovery");require(std::filesystem::file_size(root/"tail")==end,"tail not removed");});
 // Both ways of opening that verify before returning: a replay, and a snapshot open
 // with verifyOnOpen. (A plain snapshot open verifies in the background; see above.)
 test("a damaged commit slot is recovered from the other slot and the chained groups",[&]{auto p=file("slot-damage");{Store db(p,true);db.commit({{"key","safe",0,false}});db.commit({{"key2","also",0,false}});}FD fd(platform::open_file(p));unsigned char bad='X';write_at(fd.n,&bad,1,4096*((2)%2)+3000);{Store db(p);require(db.get("key").value=="safe"&&db.get("key2").value=="also","recovered");db.verify();}auto m=read_meta(fd.n);require(m.txid==2,"slots rewritten");});
 test("power loss mid-commit (POSIX: group and slot in one flush): a newer slot without its group gives way to the older one",[&]{
  for(int kind=0;kind<3;kind++){auto p=file("slot-ahead"+std::to_string(kind));uint64_t end;
   {Store db(p,true);db.commit({{"k1","one",0,false}});end=db.bytes();}
   {FD fd(platform::open_file(p));auto m=read_meta(fd.n);require(m.end==end&&m.txid==1,"clean close");
    // The interrupted commit: its slot reached the disk; its group did not (kind 0: the file was
    // never extended), only in part (kind 1: a torn header), or with a bad checksum (kind 2).
    Meta n=m;n.generation++;n.txid=2;n.end=end+96+700;n.chain[0]^=1;auto h=meta_bytes(n);write_at(fd.n,h.data(),h.size(),(n.generation%2)*4096);
    platform::truncate(fd.n,end);
    if(kind>0){std::string g(96+700,char(0));memcpy(&g[0],MAGIC_GROUP,8);put64(reinterpret_cast<unsigned char*>(&g[8]),700);put64(reinterpret_cast<unsigned char*>(&g[16]),2);memcpy(&g[32],m.chain.data(),32);if(kind==1)g.resize(512);write_at(fd.n,g.data(),g.size(),end);}}
   {Store db(p);require(db.txid()==1&&db.bytes()==end&&db.get("k1").value=="one","fell back to the older slot");db.verify();db.commit({{"k2","two",0,false}});}
   {Store db(p);require(db.get("k1").value=="one"&&db.get("k2").value=="two"&&db.txid()==2,"commits after the fallback");db.verify();}
  }});
 test("MVCC: long reads see one snapshot while commits go through",[&]{
  // Transfers keep SUM(bal) fixed and move rows between groups (index positions); a delete
  // plus an insert of the same balance keeps COUNT and SUM. Every read must see both
  // totals exactly, whatever the commits it let through.
  auto p=file("mvcc");Store db(p,true);
  db.sql("CREATE TABLE acct (id INTEGER PRIMARY KEY, bal INTEGER, grp INTEGER, pad TEXT)");
  db.sql("CREATE INDEX acct_grp ON acct (grp)");
  const int N=20000;std::vector<long long> live;
  for(int b=0;b<N/500;b++){std::string q="INSERT INTO acct (bal, grp, pad) VALUES ";for(int i=0;i<500;i++){q+=(i?",":"");q+="(100, "+std::to_string((b*500+i)%50)+", 'pppppppppppppppppppppppppppppppppppppppp')";}db.sql(q);}
  for(int i=1;i<=N;i++)live.push_back(i);
  std::atomic<bool> stop{false};std::atomic<int> writes{0};std::string failure;std::mutex fm;
  std::thread writer([&]{
   try{
    Store::SqlSession s;uint64_t r=12345;auto rnd=[&]{r=r*6364136223846793005ULL+1442695040888963407ULL;return size_t(r>>33);};
    while(!stop){
     long long a=live[rnd()%live.size()],b=live[rnd()%live.size()];if(a==b)continue;
     if(writes%3==2){
      size_t k=rnd()%live.size();long long id=live[k];
      db.sql(&s,"BEGIN","[]");
      auto j=json::parse(db.sql(&s,"SELECT bal FROM acct WHERE id = ?","["+std::to_string(id)+"]"));
      std::string bal=j.get("rows")->items.at(0).get("bal")->raw;
      db.sql(&s,"DELETE FROM acct WHERE id = ?","["+std::to_string(id)+"]");
      auto ins=json::parse(db.sql(&s,"INSERT INTO acct (bal, grp, pad) VALUES (?, 7, 'moved')","["+bal+"]"));
      db.sql(&s,"COMMIT","[]");
      live[k]=std::stoll(ins.get("lastInsertId")->raw);
     }else{
      db.sql(&s,"BEGIN","[]");
      db.sql(&s,"UPDATE acct SET bal = bal - 7, grp = (grp + 13) % 50 WHERE id = ?","["+std::to_string(a)+"]");
      db.sql(&s,"UPDATE acct SET bal = bal + 7 WHERE id = ?","["+std::to_string(b)+"]");
      db.sql(&s,"COMMIT","[]");
     }
     writes++;
    }
   }catch(const std::exception&e){std::lock_guard<std::mutex>g(fm);failure=e.what();stop=true;}
  });
  auto totals=[&](const std::string&result,const char*what){
   auto j=json::parse(result);const auto&rows=j.get("rows")->items;long long n=0,sum=0;
   if(rows.size()==1&&rows[0].get("n")){n=std::stoll(rows[0].get("n")->raw);sum=std::stoll(rows[0].get("s")->raw);}
   else for(const auto&row:rows){n++;sum+=std::stoll(row.get("bal")->raw);}
   if(n!=N||sum!=100LL*N){stop=true;writer.join();throw std::runtime_error(std::string("MVCC snapshot broken: ")+what+" saw "+std::to_string(n)+" rows, sum "+std::to_string(sum));}
  };
  const char*queries[]={
   "SELECT COUNT(*) AS n, SUM(bal) AS s FROM acct WHERE id >= 0",                     // key order
   "SELECT COUNT(*) AS n, SUM(bal) AS s FROM acct WHERE grp >= 0",                    // secondary index
   "SELECT id, bal FROM acct WHERE id >= 0 ORDER BY id DESC",                         // key order, descending
   "SELECT id, bal FROM acct WHERE grp >= 0 ORDER BY grp DESC",                       // index, descending
   "SELECT id, bal, ROW_NUMBER() OVER (ORDER BY id) AS r FROM acct"};                // whole table by shard
  auto t0=std::chrono::steady_clock::now();int rounds=0;
  while(std::chrono::steady_clock::now()-t0<std::chrono::seconds(3)||rounds<10){
   for(const char*q:queries)totals(db.sql(q),q);
   rounds++;
   {std::lock_guard<std::mutex>g(fm);if(!failure.empty())break;}
  }
  stop=true;writer.join();
  require(failure.empty(),"writer failed: "+failure);
  require(writes>0,"no commits ran during the reads");
  std::cout<<"  mvcc: "<<rounds*5<<" reads, "<<writes<<" commits, "<<db.snapshot_yields()<<" yields"<<std::endl;
  require(db.snapshot_yields()>0,"no read let a commit through");
  db.verify();
 });
 test("BEGIN CONCURRENT: transfers from many threads, conflicts retried, totals exact",[&]{
  auto p=file("mvcc-concurrent");Store db(p,true);
  db.sql("CREATE TABLE acct (id INTEGER PRIMARY KEY, bal INTEGER)");
  const int N=200;{std::string q="INSERT INTO acct (bal) VALUES ";for(int i=0;i<N;i++)q+=std::string(i?",":"")+"(1000)";db.sql(q);}
  std::atomic<int> committed{0},conflicts{0};std::string failure;std::mutex fm;
  auto worker=[&](int seed){
   try{
    Store::SqlSession s;uint64_t r=uint64_t(seed)*7919+1;auto rnd=[&]{r=r*6364136223846793005ULL+1442695040888963407ULL;return int(r>>33);};
    for(int k=0;k<300;k++){
     int a=1+rnd()%N,b=1+rnd()%N;if(a==b)continue;int amt=1+rnd()%9;
     for(;;){
      db.sql(&s,"BEGIN CONCURRENT","[]");
      try{
       auto j=json::parse(db.sql(&s,"SELECT bal FROM acct WHERE id = ?","["+std::to_string(a)+"]"));
       long long bal=std::stoll(j.get("rows")->items.at(0).get("bal")->raw);
       db.sql(&s,"UPDATE acct SET bal = ? WHERE id = ?","["+std::to_string(bal-amt)+","+std::to_string(a)+"]");
       db.sql(&s,"UPDATE acct SET bal = bal + ? WHERE id = ?","["+std::to_string(amt)+","+std::to_string(b)+"]");
       db.sql(&s,"INSERT INTO acct (bal) VALUES (0)","[]");  // ids from many transactions at once
       db.sql(&s,"COMMIT","[]");committed++;break;
      }catch(const std::exception&e){
       if(std::string(e.what()).rfind("CONFLICT",0)!=0)throw;
       conflicts++;if(s.active)db.sql(&s,"ROLLBACK","[]");
      }
     }
    }
   }catch(const std::exception&e){std::lock_guard<std::mutex>g(fm);failure=e.what();}
  };
  std::vector<std::thread> ts;for(int i=0;i<4;i++)ts.emplace_back(worker,i+1);
  // A reader meanwhile: every snapshot balances.
  long long reads=0;
  while(committed<4*250&&reads<100000){
   auto j=json::parse(db.sql("SELECT SUM(bal) AS s FROM acct WHERE id >= 0"));
   require(std::stoll(j.get("rows")->items.at(0).get("s")->raw)==1000LL*N,"a snapshot did not balance");
   reads++;
   {std::lock_guard<std::mutex>g(fm);if(!failure.empty())break;}
  }
  for(auto&t:ts)t.join();
  require(failure.empty(),"worker failed: "+failure);
  auto j=json::parse(db.sql("SELECT SUM(bal) AS s, COUNT(*) AS n FROM acct"));
  require(std::stoll(j.get("rows")->items.at(0).get("s")->raw)==1000LL*N,"money appeared or vanished");
  require(std::stoll(j.get("rows")->items.at(0).get("n")->raw)==N+committed,"one new row per committed transaction, with distinct ids");
  std::cout<<"  concurrent: "<<committed<<" committed, "<<conflicts<<" conflicts retried, "<<reads<<" reads"<<std::endl;
  db.verify();
 });
 test("BEGIN CONCURRENT is serializable: write skew, phantoms, read-only, disjoint, coarse read sets",[&]{
  auto p=file("serializable");Store db(p,true);
  auto q=[&](Store::SqlSession&s,const std::string&sql){return db.sql(&s,sql,"[]");};
  auto num=[&](Store::SqlSession&s,const std::string&sql){auto j=json::parse(q(s,sql));return std::stoll(j.get("rows")->items.at(0).fields.at(0).second.raw);};
  auto lost=[&](Store::SqlSession&s){expect_error([&]{q(s,"COMMIT");},"that this one read");require(!s.active,"a failed COMMIT ends it");};
  Store::SqlSession a,b;
  db.sql("CREATE TABLE doctors (id INTEGER PRIMARY KEY, name TEXT, on_call BOOLEAN)");
  for(int pass=0;pass<3;pass++){
   // Two doctors on call; each goes off call if the other still is (write skew). Pass 1:
   // through an index on on_call; pass 2: ISOLATION LEVEL SNAPSHOT, which allows it.
   if(pass==1)db.sql("CREATE INDEX ON doctors (on_call)");
   db.sql("DELETE FROM doctors");db.sql("INSERT INTO doctors (id, name, on_call) VALUES (1, 'alice', true), (2, 'bob', true), (3, 'carol', false)");
   std::string begin=pass==2?"BEGIN CONCURRENT ISOLATION LEVEL SNAPSHOT":"BEGIN CONCURRENT";
   q(a,begin);q(b,begin);
   require(num(a,"SELECT COUNT(*) FROM doctors WHERE on_call = true")==2&&num(b,"SELECT COUNT(*) FROM doctors WHERE on_call = true")==2,"both on call");
   q(a,"UPDATE doctors SET on_call = false WHERE id = 1");q(b,"UPDATE doctors SET on_call = false WHERE id = 2");
   q(a,"COMMIT");
   if(pass<2){lost(b);require(num(a,"SELECT COUNT(*) FROM doctors WHERE on_call = true")==1,"one doctor stays on call");}
   else{q(b,"COMMIT");require(num(a,"SELECT COUNT(*) FROM doctors WHERE on_call = true")==0,"snapshot isolation allows write skew");}
  }
  // Phantoms: a row added to a range read (index range, key range) fails the reader; one
  // added outside it does not.
  db.sql("CREATE TABLE orders (id INTEGER PRIMARY KEY, customer INT, amount INT)");db.sql("CREATE INDEX ON orders (customer)");
  db.sql("CREATE TABLE totals (id INTEGER PRIMARY KEY, customer INT, total INT)");
  db.sql("INSERT INTO orders (customer, amount) VALUES (5, 10), (5, 20), (6, 7)");
  for(int c:{6,5}){
   q(a,"BEGIN CONCURRENT");
   require(num(a,"SELECT SUM(amount) FROM orders WHERE customer = 5")==30,"sum");
   q(a,"INSERT INTO totals (customer, total) VALUES (5, 30)");
   db.sql("INSERT INTO orders (customer, amount) VALUES ("+std::to_string(c)+", 1)");
   if(c==6)q(a,"COMMIT");else lost(a);
  }
  q(a,"BEGIN CONCURRENT");
  require(num(a,"SELECT COUNT(*) FROM orders WHERE id > 2")==3,"key range");
  q(a,"UPDATE totals SET total = 0 WHERE id = 1");
  db.sql("INSERT INTO orders (customer, amount) VALUES (9, 9)");
  lost(a);
  // Read-only transactions always commit, whatever changed meanwhile.
  q(a,"BEGIN CONCURRENT");
  require(num(a,"SELECT COUNT(*) FROM orders")==6,"count");num(a,"SELECT amount FROM orders WHERE id = 1");
  db.sql("UPDATE orders SET amount = 0");db.sql("DELETE FROM orders WHERE id = 1");
  require(num(a,"SELECT COUNT(*) FROM orders")==6,"its snapshot");q(a,"COMMIT");
  // Disjoint reads and writes commit side by side: rows by id, by a unique value, by an index
  // value, and a scan that stopped at its LIMIT (rows past it were not read).
  db.sql("CREATE TABLE acct (id INTEGER PRIMARY KEY, owner TEXT UNIQUE, grp INT, bal INT)");db.sql("CREATE INDEX ON acct (grp)");
  db.sql("INSERT INTO acct (owner, grp, bal) VALUES ('ann', 1, 100), ('bob', 2, 100), ('cid', 3, 100), ('dan', 4, 100)");
  q(a,"BEGIN CONCURRENT");q(b,"BEGIN CONCURRENT");
  num(a,"SELECT bal FROM acct WHERE id = 1");num(a,"SELECT bal FROM acct WHERE owner = 'bob'");num(a,"SELECT id FROM acct ORDER BY id LIMIT 1");
  num(b,"SELECT COUNT(*) FROM acct WHERE grp = 3");num(b,"SELECT bal FROM acct WHERE owner = 'dan'");
  q(a,"UPDATE acct SET bal = bal - 10 WHERE id = 1");q(a,"UPDATE acct SET bal = bal + 10 WHERE owner = 'bob'");
  q(b,"UPDATE acct SET bal = bal - 10 WHERE grp = 3");q(b,"UPDATE acct SET bal = bal + 10 WHERE owner = 'dan'");
  q(b,"INSERT INTO acct (owner, grp, bal) VALUES ('eve', 5, 0)");
  q(b,"COMMIT");q(a,"COMMIT");
  require(num(a,"SELECT SUM(bal) FROM acct")==400&&num(a,"SELECT bal FROM acct WHERE id = 2")==110&&num(a,"SELECT bal FROM acct WHERE id = 4")==110,"both committed");
  // INSERT ... ON CONFLICT DO NOTHING that kept an existing row read it.
  q(a,"BEGIN CONCURRENT");q(a,"INSERT INTO acct (id, owner, grp, bal) VALUES (2, 'x', 0, 0) ON CONFLICT DO NOTHING");q(a,"UPDATE acct SET bal = 1 WHERE id = 3");
  db.sql("DELETE FROM acct WHERE id = 2");
  lost(a);
  // Bounded read sets: the same rows read again add nothing; past the limit, the tables read
  // count as read whole (rows read later in other tables stay exact).
  {std::string v;for(int i=0;i<20000;i++)v+=std::string(i?",":"")+"("+std::to_string(i)+")";db.sql("CREATE TABLE big (id INTEGER PRIMARY KEY, v INT)");db.sql("INSERT INTO big (v) VALUES "+v);}
  auto reads=[&](int distinct){for(int i=0;i<20000;i++)db.sql(&a,"SELECT v FROM big WHERE id = ?","["+std::to_string(1+i%distinct)+"]");};
  q(a,"BEGIN CONCURRENT");reads(50);num(a,"SELECT bal FROM acct WHERE id = 1");q(a,"UPDATE acct SET bal = bal WHERE id = 1");
  db.sql("UPDATE big SET v = 0 WHERE id = 19999");db.sql("INSERT INTO big (v) VALUES (1)");db.sql("UPDATE acct SET bal = bal WHERE id = 3");
  q(a,"COMMIT");  // exact: none of those rows was read
  q(a,"BEGIN CONCURRENT");reads(20000);num(a,"SELECT bal FROM acct WHERE id = 1");q(a,"UPDATE acct SET bal = bal WHERE id = 1");
  db.sql("UPDATE acct SET bal = bal WHERE id = 3");
  q(a,"COMMIT");  // acct, read after the fallback, is still exact
  q(a,"BEGIN CONCURRENT");reads(20000);q(a,"UPDATE acct SET bal = bal WHERE id = 1");
  db.sql("INSERT INTO big (v) VALUES (1)");  // past every row read, but big now counts as read whole
  lost(a);
  // The message names the row's id.
  db.sql("CREATE TABLE named (id TEXT PRIMARY KEY, v INT)");db.sql("INSERT INTO named (id, v) VALUES ('k1', 1)");
  q(a,"BEGIN CONCURRENT");num(a,"SELECT v FROM named WHERE id = 'k1'");q(a,"UPDATE acct SET bal = bal WHERE id = 1");
  db.sql("UPDATE named SET v = 2 WHERE id = 'k1'");
  expect_error([&]{q(a,"COMMIT");},"changed a row of named that this one read (id \"k1\")");
  q(a,"BEGIN CONCURRENT");q(a,"UPDATE named SET v = 3 WHERE id = 'k1'");db.sql("UPDATE named SET v = 4 WHERE id = 'k1'");
  expect_error([&]{q(a,"COMMIT");},"changed a row of named that this one writes (id \"k1\")");
  // ISOLATION LEVEL: SERIALIZABLE (the default), SNAPSHOT, REPEATABLE READ, READ COMMITTED.
  expect_error([&]{q(a,"BEGIN CONCURRENT ISOLATION LEVEL CHAOS");},"INPUT");
  q(a,"START TRANSACTION ISOLATION LEVEL SERIALIZABLE");q(a,"COMMIT");
  q(a,"BEGIN CONCURRENT TRANSACTION ISOLATION LEVEL READ COMMITTED");q(a,"COMMIT");
  q(a,"BEGIN CONCURRENT ISOLATION LEVEL REPEATABLE READ");num(a,"SELECT COUNT(*) FROM big");q(a,"UPDATE acct SET bal = bal WHERE id = 1");
  db.sql("INSERT INTO big (v) VALUES (1)");q(a,"COMMIT");
  require(db.snapshot_memory()==0,"no snapshot left");
  db.verify();
 });
 test("repeated online compactions leave no old files or mappings behind",[&]{
  auto dir=root/"compact-many";std::filesystem::create_directories(dir);auto p=(dir/"c.kdb").u8string();
  Store db(p,true);
  for(int round=0;round<12;round++){
   for(int i=0;i<200;i++)db.commit({{"k"+std::to_string(i),std::string(500,char('a'+round%26)),ANY,false}});
   std::thread reader([&]{for(int i=0;i<2000;i++){auto v=db.get("k"+std::to_string(i%200));require(v.found,"read during compaction");}});
   db.compact();reader.join();
   size_t old=0;for(const auto&e:std::filesystem::directory_iterator(dir))if(e.path().filename().u8string().find("compacted-old")!=std::string::npos)old++;
   require(old==0,"an old file is left after compaction "+std::to_string(round+1));
  }
  require(db.get("k5").value==std::string(500,char('a'+11)),"values after compactions");db.verify();
 });
 test("truncate_log cuts a closed log back to a transaction (cluster rejoin)",[&]{auto p=file("cut");
  {Store db(p,true);for(int i=1;i<=5;i++)db.commit({{"k"+std::to_string(i),"v"+std::to_string(i),ANY,false}});db.commit({{"k1","",ANY,true}});}
  require(Store::truncate_log(p,6),"no change at the end");require(!Store::truncate_log(p,9),"past the end is not a boundary");
  require(Store::truncate_log(p,3),"cut");
  {Store db(p);require(db.txid()==3&&db.get("k1").value=="v1"&&db.get("k3").value=="v3"&&!db.get("k4").found,"cut state");db.verify();db.commit({{"k9","after",ANY,false}});}
  {Store db(p);require(db.txid()==4&&db.get("k9").value=="after"&&!db.get("k5").found,"commits after the cut");db.verify();}
  require(Store::truncate_log(p,0),"cut to empty");{Store db(p);require(db.txid()==0&&db.count()==0,"empty");db.verify();}
 });
 test("corrupt and truncated committed files fail without modifying bytes",[&]{for(int kind=0;kind<2;kind++)for(int opener=0;opener<2;opener++){auto p=file("corrupt"+std::to_string(kind)+std::to_string(opener));uint64_t end;{Store db(p,true);db.commit({{"key","safe",0,false}});end=db.bytes();}FD fd(platform::open_file(p));if(kind==0)platform::truncate(fd.n,end-1);else{unsigned char bad='X';write_at(fd.n,&bad,1,kind==1?START+64+17+3:4096+3000);}auto before=read_at(fd.n,size_t(file_size(fd.n)),0);Options o;o.verifyOnOpen=true;ignore_index_snapshot=opener==0;expect_error([&]{Store db(p,false,o);});ignore_index_snapshot=false;require(before==read_at(fd.n,size_t(file_size(fd.n)),0),"corruption recovery modified file");}});
 test("point reads detect live value corruption and pread truncation",[&]{for(int kind=0;kind<2;kind++){auto p=file("live"+std::to_string(kind));Options o;o.reads=ReadMode::Pread;Store db(p,true,o);db.commit({{"key","safe",0,false}});FD fd(platform::open_file(p));if(kind)platform::truncate(fd.n,START);else{unsigned char bad='X';write_at(fd.n,&bad,1,START+64+17+3);}expect_error([&]{db.get("key");});}});
 test("64-bit positioned I/O and sparse file truncation beyond 4 GiB",[&]{FD fd(platform::open_file(file("sparse"),true));
 #ifdef _WIN32
  OVERLAPPED op{};DWORD returned=0;BOOL ok=DeviceIoControl(fd.n,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&returned,&op);if(!ok){require(GetLastError()==ERROR_IO_PENDING,"sparse file control failed");require(GetOverlappedResult(fd.n,&op,&returned,TRUE),"sparse file control completion");}
 #endif
  uint64_t off=(uint64_t(1)<<32)+31;std::string value="offset-ok";write_at(fd.n,value.data(),value.size(),off);auto got=read_at(fd.n,value.size(),off);require(std::string(got.begin(),got.end())==value&&file_size(fd.n)==off+value.size(),"64-bit file offset");platform::truncate(fd.n,17);require(file_size(fd.n)==17,"truncate offset");});
 test("eight writers and eight readers preserve atomic multi-key versions",[&]{Store db(file("threads"),true);std::vector<Op> ops;std::vector<std::string> keys;for(int i=0;i<64;i++){keys.push_back(std::to_string(i));ops.push_back({keys.back(),"value",ANY,false});}db.commit(ops);std::atomic<bool> stop{false},bad{false};std::vector<std::thread> readers,writers;for(int t=0;t<8;t++)readers.emplace_back([&]{try{while(!stop){auto values=db.getMany(keys);for(auto&v:values)if(!v.found||v.value!="value"||v.version!=values[0].version)bad=true;}}catch(...){bad=true;}});for(int t=0;t<8;t++)writers.emplace_back([&]{try{for(int i=0;i<30;i++)db.commit(ops);}catch(...){bad=true;}});for(auto&t:writers)t.join();stop=true;for(auto&t:readers)t.join();require(!bad&&db.txid()==241,"thread consistency");db.verify();});
 test("reader in another handle sees every acknowledged commit, in order",[&]{
  auto p=file("reader");Store db(p,true);db.commit({{"a","1",0,false}});Reader r(p);std::string v;uint64_t ver=0;
  require(r.read("a",v,ver)&&v=="1"&&ver==1,"initial read");
  // Each acknowledged write is visible to the very next read; values grow past the
  // reserve so the reader also remaps; deletes disappear.
  std::string big(300000,'b');
  for(int i=0;i<200;i++){auto k="k"+std::to_string(i%17);auto x=db.commit({{k,i%5?big+std::to_string(i):std::to_string(i),ANY,false}});require(r.read(k,v,ver)&&ver==x&&v==(i%5?big+std::to_string(i):std::to_string(i)),"read after commit");if(i%7==0){auto d=db.commit({{k,"",ANY,true}});require(!r.read(k,v,ver)&&d==x+1,"read after delete");}}
  require(r.size()==db.count()&&r.txid()==db.txid(),"reader state");
  auto many=r.getMany({"a","nope","a"});require(many[0].found&&!many[1].found&&many[2].version==many[0].version,"reader getMany");
  // A concurrent reader thread never sees versions go backwards or a torn transaction.
  std::atomic<bool> stop{false};std::atomic<int> bad{0};
  std::thread watcher([&]{uint64_t last=0;while(!stop){auto m=r.getMany({"x","y"});if(m[0].found!=m[1].found||(m[0].found&&(m[0].version!=m[1].version||m[0].value!=m[1].value)))bad++;if(m[0].found&&m[0].version<last)bad++;if(m[0].found)last=m[0].version;}});
  for(int i=0;i<3000;i++)db.commit({{"x",std::to_string(i),ANY,false},{"y",std::to_string(i),ANY,false}});
  stop=true;watcher.join();require(bad==0,"reader saw a torn or older state");
  Reader late(p);require(late.size()==db.count()&&late.read("x",v,ver)&&v=="2999","new reader replays");
  // A damaged committed value fails its checksum in the reader.
  db.commit({{"victim",std::string(64,'v'),0,false}});require(r.read("victim",v,ver),"victim visible");FD fd(platform::open_file(p));unsigned char bad2='X';write_at(fd.n,&bad2,1,db.bytes()-32-10);expect_error([&]{r.read("victim",v,ver);},"CORRUPT");
 });
 test("sorted set matches std::set under random inserts, erases and range scans",[&]{
  SortedSet s;std::set<std::string> ref;uint64_t rng=12345;
  for(int step=0;step<60000;step++){
   rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;std::string k="k"+std::to_string(rng%3000);
   if(rng%3==0)require(s.erase(k)==(ref.erase(k)>0),"erase");else require(s.insert(k)==ref.insert(k).second,"insert");
   if(step%2999==0){
    require(s.size()==ref.size(),"size");auto it=ref.begin();for(auto p=s.seek("");s.valid(p);p=s.next(p),++it)require(it!=ref.end()&&s.get(p)==*it,"order");
    std::string from="k"+std::to_string(rng%3000);auto lo=ref.lower_bound(from);auto p=s.seek(from);require(lo==ref.end()?!s.valid(p):s.get(p)==*lo,"seek");
    auto rit=ref.rbegin();for(auto q=s.prev(s.end());s.valid(q);q=s.prev(q),++rit)require(s.get(q)==*rit,"reverse");
   }
  }
 });
 test("queries: where, ranges, prefix, ordering, cursors and counts agree with a brute-force scan",[&]{
  auto p=file("query");Store db(p,true);
  std::vector<std::string> cities={"Colombo","Kandy","Galle","Jaffna"};
  for(int b=0;b<10;b++){std::vector<Op> ops;for(int i=b*100;i<b*100+100;i++){std::string v="{\"age\":"+std::to_string(i%90)+",\"city\":\""+cities[i%4]+"\",\"vip\":"+(i%7==0?"true":"false")+(i%11==0?"":",\"tags\":{\"n\":"+std::to_string(i%5)+"}")+"}";ops.push_back({std::string("users")+'\0'+"u"+std::to_string(1000+i),v,ANY,false});}
   db.commit(ops);}
  db.commit({{std::string("other")+'\0'+"x","{\"age\":5}",ANY,false}});
  auto all=[&](const std::string&spec){std::vector<std::string> keys;std::string after;for(int guard=0;guard<1000;guard++){std::string s=spec;if(!after.empty())s.insert(s.size()-1,",\"after\":\""+after+"\"");auto r=db.query(s);for(auto&it:r.items)keys.push_back(it.key);if(r.next.empty())break;after=r.next;}return keys;};
  auto brute=[&](auto pred){std::vector<std::string> keys;for(int i=0;i<1000;i++)if(pred(i))keys.push_back("u"+std::to_string(1000+i));return keys;};
  for(int pass=0;pass<2;pass++){
   if(pass==1){db.create_index("users","age");db.create_index("users","city");db.create_index("users","tags.n");}
   {auto got=all(R"({"collection":"users","limit":37})");auto want=brute([](int){return true;});if(got!=want){std::cout<<"pass "<<pass<<" got "<<got.size()<<" want "<<want.size();for(size_t i=0;i<got.size()&&i<want.size();i++)if(got[i]!=want[i]){std::cout<<" first diff at "<<i<<": "<<got[i]<<" vs "<<want[i];break;}std::cout<<std::endl;}require(got==want,"all keys in order");}
   require(all(R"({"collection":"users","where":{"city":"Kandy"},"limit":13})")==brute([](int i){return i%4==1;}),"equality");
   auto ages=all(R"({"collection":"users","where":{"age":{"$gte":30,"$lt":40}},"limit":50})");std::sort(ages.begin(),ages.end());require(ages==brute([](int i){return i%90>=30&&i%90<40;}),"range");
   auto in=all(R"({"collection":"users","where":{"city":{"$in":["Galle","Jaffna"]},"vip":true},"limit":9})");std::sort(in.begin(),in.end());require(in==brute([](int i){return (i%4==2||i%4==3)&&i%7==0;}),"in + bool");
   auto nested=all(R"({"collection":"users","where":{"tags.n":{"$gt":2}},"limit":40})");std::sort(nested.begin(),nested.end());require(nested==brute([](int i){return i%11!=0&&i%5>2;}),"nested path");
   auto missing=all(R"({"collection":"users","where":{"tags":{"$exists":false}},"limit":40})");require(missing==brute([](int i){return i%11==0;}),"exists false");
   require(all(R"({"collection":"users","where":{"_key":{"$prefix":"u15"}},"limit":7})")==brute([](int i){return std::to_string(1000+i).rfind("15",0)==0;}),"key prefix");
   require(all(R"({"collection":"users","where":{"city":{"$ne":"Colombo"},"age":{"$lte":3}},"limit":5})")==brute([](int i){return i%4!=0&&i%90<=3;}),"ne + lte");
   // Ordered by a field: ascending by (age, key), descending, pages joined.
   auto byAge=all(R"({"collection":"users","orderBy":"age","limit":33})");auto exp=brute([](int){return true;});std::stable_sort(exp.begin(),exp.end(),[](const std::string&a,const std::string&b){int x=(std::stoi(a.substr(1))-1000)%90,y=(std::stoi(b.substr(1))-1000)%90;return x<y;});require(byAge==exp,"orderBy age");
   auto desc=all(R"({"collection":"users","orderBy":"-age","where":{"city":"Galle"},"limit":21})");require(desc.size()==250,"desc count");
   {auto r=db.query(R"({"collection":"users","orderBy":"-age","limit":1})");require(r.items.size()==1&&r.items[0].value.find("\"age\":89")!=std::string::npos,"desc first");}
   require(db.query(R"({"collection":"users","count":true})").count==1000&&db.query(R"({"collection":"users","where":{"city":"Kandy"},"count":true})").count==250,"count");
   require(db.query(R"({"collection":"other","count":true})").count==1,"collections separate");
   auto ko=db.query(R"({"collection":"users","keysOnly":true,"limit":2})");require(ko.items.size()==2&&ko.items[0].value.empty()&&ko.items[0].version>0,"keysOnly");
  }
  expect_error([&]{db.query(R"({"collection":"users","where":{"a":{"$bogus":1}}})");},"INPUT");
  expect_error([&]{db.query(R"({"collection":"users","limit":0})");},"INPUT");
  auto ix=db.indexes();require(ix.size()==3&&ix[0].entries==1000,"index list");
 });
 test("indexes follow writes, deletes and group commits; unique indexes reject duplicates",[&]{
  auto p=file("unique");
  {Store db(p,true);db.create_index("acct","email",true);
   auto k=[](const std::string&s){return std::string("acct")+'\0'+s;};
   db.commit({{k("a"),R"({"email":"a@x"})",ANY,false}});
   expect_error([&]{db.commit({{k("b"),R"({"email":"a@x"})",ANY,false}});},"CONFLICT unique");
   db.commit({{k("a"),R"({"email":"a2@x"})",ANY,false}});db.commit({{k("b"),R"({"email":"a@x"})",ANY,false}});  // freed value is reusable
   db.commit({{k("n1"),R"({"email":null})",ANY,false},{k("n2"),R"({})",ANY,false}});  // null and missing are not unique values
   // One group: the second transaction conflicts with the first; the third is independent.
   std::vector<Op> t1{{k("c"),R"({"email":"c@x"})",ANY,false}},t2{{k("d"),R"({"email":"c@x"})",ANY,false}},t3{{k("e"),R"({"email":"e@x"})",ANY,false}};
   auto out=db.commit_group({&t1,&t2,&t3});require(!out[0].error&&out[1].error&&!out[2].error,"group unique");
   // Swap two values inside one transaction is allowed.
   db.commit({{k("c"),R"({"email":"e@x"})",ANY,false},{k("e"),R"({"email":"c@x"})",ANY,false}});
   db.commit({{k("b"),"",ANY,true}});
   require(db.query(R"({"collection":"acct","where":{"email":"a@x"},"count":true})").count==0,"deleted leaves index");
   require(db.query(R"({"collection":"acct","where":{"email":"c@x"}})").items.at(0).key=="e","swap visible");
   expect_error([&]{db.create_index("acct","dup",true);db.commit({{k("x1"),R"({"dup":1})",ANY,false}});db.commit({{k("x2"),R"({"dup":1})",ANY,false}});},"CONFLICT");
   db.drop_index("acct","dup");db.commit({{k("x2"),R"({"dup":1})",ANY,false}});
   expect_error([&]{db.create_index("acct","dup",true);},"CONFLICT");
   expect_error([&]{db.commit({{std::string("\x01" "catalog",8),"{}",ANY,false}});},"reserved");
  }
  // Reopen (index snapshot and replay): definitions persist, indexes rebuild on first use.
  for(int mode=0;mode<2;mode++){ignore_index_snapshot=mode==1;Store db(p);ignore_index_snapshot=false;
   auto ix=db.indexes();require(ix.size()==1&&ix[0].unique&&ix[0].field=="email","catalog persisted");
   expect_error([&]{db.commit({{std::string("acct")+'\0'+"z",R"({"email":"a2@x"})",ANY,false}});},"CONFLICT unique");
   require(db.query(R"({"collection":"acct","where":{"email":"a2@x"}})").items.at(0).key=="a","index rebuilt");}
 });
 test("atomic increments: integers, doubles, missing values, conflicts and concurrency",[&]{
  auto p=file("incr");Store db(p,true);auto k=std::string("c")+'\0'+"n";
  Op inc{k,"5",ANY,false,true};
  std::vector<Op> first{inc};db.commit_group({&first});
  require(db.get(k).value=="5","missing counts as zero");
  std::vector<Op> two{{k,"-7",ANY,false,true}};auto o=db.commit_group({&two});require(o[0].values.at(0)=="-2"&&db.get(k).value=="-2","integer");
  std::vector<Op> half{{k,"0.5",ANY,false,true}};db.commit_group({&half});require(db.get(k).value=="-1.5","double");
  db.commit({{k,"\"text\"",ANY,false}});std::vector<Op> bad{{k,"1",ANY,false,true}};require(db.commit_group({&bad})[0].error!=nullptr,"non-number");
  db.commit({{k,"9223372036854775807",ANY,false}});require(db.commit_group({&bad})[0].error!=nullptr,"overflow");
  db.commit({{k,"0",ANY,false}});
  // Many threads, grouped commits: no increment is lost.
  std::vector<std::thread> th;for(int t=0;t<8;t++)th.emplace_back([&]{for(int i=0;i<200;i++){std::vector<Op> one{{k,"1",ANY,false,true}};auto res=db.commit_group({&one});require(!res[0].error,"incr");}});
  for(auto&t:th)t.join();require(db.get(k).value=="1600","concurrent increments");
  // Two increments of one key inside one group see each other.
  std::vector<Op> a{{k,"10",ANY,false,true}},b{{k,"100",ANY,false,true}};auto g=db.commit_group({&a,&b});require(g[0].values[0]=="1610"&&g[1].values[0]=="1710","group increments");
  Store*dbp=&db;(void)dbp;
 });
 test("queries see one committed state while writers commit",[&]{
  auto p=file("query-concurrent");Store db(p,true);db.create_index("t","g");
  auto k=[](int i){return std::string("t")+'\0'+"k"+std::to_string(i);};
  {std::vector<Op> ops;for(int i=0;i<200;i++)ops.push_back({k(i),"{\"g\":0}",ANY,false});db.commit(ops);}
  std::atomic<bool> stop{false};std::atomic<int> bad{0};
  std::thread w([&]{for(int round=1;round<300;round++){std::vector<Op> ops;for(int i=0;i<200;i++)ops.push_back({k(i),"{\"g\":"+std::to_string(round)+"}",ANY,false});db.commit(ops);}stop=true;});
  while(!stop){auto r=db.query(R"({"collection":"t","orderBy":"g","limit":10000})");if(r.items.size()!=200)bad++;else for(auto&it:r.items)if(it.value!=r.items[0].value)bad++;}
  w.join();require(bad==0,"query saw a partial transaction");
 });
 test("SQL: tables, auto ids, SELECT/UPDATE/DELETE, aggregates, upserts, constraints and reopen",[&]{
  auto p=file("sql");
  auto has=[](const std::string&s,const std::string&part){if(s.find(part)==std::string::npos){std::cout<<"  result: "<<s<<"\n  missing: "<<part<<std::endl;return false;}return true;};
  {Store db(p,true);
   db.sql("CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT UNIQUE NOT NULL, name VARCHAR(100), age INT, city TEXT)");
   expect_error([&]{db.sql("CREATE TABLE users (id INT)");},"already exists");
   db.sql("CREATE TABLE IF NOT EXISTS users (id INT)");
   expect_error([&]{db.sql("SELECT * FROM nope");},"no such table");
   std::vector<std::string> cities={"Colombo","Kandy","Galle"};
   for(int i=1;i<=30;i++){
    auto r=db.sql("INSERT INTO users (email, name, age, city) VALUES (?, ?, ?, ?)","[\"u"+std::to_string(i)+"@x.com\",\"User "+std::to_string(i)+"\","+std::to_string(20+i%10)+",\""+cities[i%3]+"\"]");
    require(has(r,"\"lastInsertId\":"+std::to_string(i))&&has(r,"\"changes\":1"),"auto id");
   }
   require(has(db.sql("SELECT * FROM users WHERE id = 7"),R"({"rows":[{"id":7,"email":"u7@x.com","name":"User 7","age":27,"city":"Kandy"}]})"),"select by id");
   expect_error([&]{db.sql("INSERT INTO users (email) VALUES ('u7@x.com')");},"CONFLICT unique");
   expect_error([&]{db.sql("INSERT INTO users (id, email) VALUES (7, 'new@x.com')");},"CONFLICT duplicate id");
   for(int pass=0;pass<2;pass++){
    if(pass==1){db.sql("CREATE INDEX idx_age ON users (age)");db.sql("CREATE INDEX ON users (city)");}
    require(has(db.sql("SELECT id FROM users WHERE age >= 28 AND city = 'Galle' ORDER BY id"),R"({"rows":[{"id":8},{"id":29}]})"),"where and");
    require(has(db.sql("SELECT id, age FROM users WHERE city IN ('Kandy') AND age BETWEEN 21 AND 23 ORDER BY age DESC, id"),R"({"rows":[{"id":13,"age":23},{"id":22,"age":22},{"id":1,"age":21}]})"),"in between order");
    require(has(db.sql("SELECT name FROM users WHERE name LIKE 'User 2%' ORDER BY id LIMIT 2 OFFSET 1"),R"({"rows":[{"name":"User 20"},{"name":"User 21"}]})"),"like limit offset");
    require(has(db.sql("SELECT COUNT(*) AS n FROM users WHERE age < 22 OR city = 'Kandy'"),R"({"rows":[{"n":14}]})"),"count or");
    require(has(db.sql("SELECT city, COUNT(*) AS n, AVG(age) AS avg_age, MAX(age) AS oldest FROM users GROUP BY city HAVING COUNT(*) >= 10 ORDER BY city"),R"({"rows":[{"city":"Colombo","n":10,"avg_age":24.5,"oldest":29},{"city":"Galle","n":10,"avg_age":24.5,"oldest":29},{"city":"Kandy","n":10,"avg_age":24.5,"oldest":29}]})"),"group by");
    require(has(db.sql("SELECT id FROM users ORDER BY age DESC, id DESC LIMIT 3"),R"({"rows":[{"id":29},{"id":19},{"id":9}]})"),"multi order");
    require(has(db.sql("SELECT DISTINCT city FROM users ORDER BY city DESC"),R"({"rows":[{"city":"Kandy"},{"city":"Galle"},{"city":"Colombo"}]})"),"distinct");
    require(has(db.sql("SELECT UPPER(city) AS c, age * 2 + 1 AS x FROM users WHERE id = ?","[3]"),R"({"rows":[{"c":"COLOMBO","x":47}]})"),"expressions");
    require(has(db.sql("SELECT COUNT(*) AS n FROM users WHERE city <> 'Galle' AND NOT age = 25"),R"({"rows":[{"n":18}]})"),"not");
    require(has(db.sql("SELECT id FROM users WHERE id > 27"),R"({"rows":[{"id":28},{"id":29},{"id":30}]})"),"id range");
   }
   require(has(db.sql("UPDATE users SET age = age + 10, name = 'Seven' WHERE id = 7"),R"({"changes":1})"),"update");
   require(has(db.sql("SELECT name, age FROM users WHERE id = 7"),R"({"rows":[{"name":"Seven","age":37}]})"),"updated");
   require(has(db.sql("SELECT id FROM users WHERE age = 37"),R"({"rows":[{"id":7}]})"),"index follows update");
   expect_error([&]{db.sql("UPDATE users SET email = 'u1@x.com' WHERE id = 2");},"CONFLICT unique");
   expect_error([&]{db.sql("UPDATE users SET id = 99 WHERE id = 2");},"id cannot be changed");
   require(has(db.sql("DELETE FROM users WHERE city = 'Galle' AND age > 26"),R"({"changes":3})"),"delete");
   require(has(db.sql("SELECT COUNT(*) AS n FROM users"),R"({"rows":[{"n":27}]})"),"count after delete");
   // Upserts in MySQL and PostgreSQL syntax, nested columns, JSON parameters, text ids.
   require(has(db.sql("INSERT INTO users (id, email, name) VALUES (7, 'u7@x.com', 'Again') ON CONFLICT (id) DO UPDATE SET name = excluded.name"),R"("changes":1)"),"upsert pg");
   require(has(db.sql("INSERT INTO users (id, email, name) VALUES (7, 'u7@x.com', 'Mysql') ON DUPLICATE KEY UPDATE name = VALUES(name)"),R"("changes":1)"),"upsert mysql");
   require(has(db.sql("SELECT name FROM users WHERE id = 7"),R"({"rows":[{"name":"Mysql"}]})"),"upserted");
   require(has(db.sql("INSERT IGNORE INTO users (id, email) VALUES (7, 'zz@x.com')"),R"({"changes":0)"),"insert ignore");
   db.sql("CREATE TABLE docs (id TEXT PRIMARY KEY)");
   db.sql("INSERT INTO docs (id, meta.owner, meta.tags, body) VALUES ('readme', 'kestro', ?, ?)",R"([["a","b"],{"text":"hi","n":1}])");
   require(has(db.sql("SELECT meta.owner AS owner, meta.tags, body.text FROM docs WHERE id = 'readme'"),R"({"rows":[{"owner":"kestro","meta.tags":["a","b"],"body.text":"hi"}]})"),"nested + json params");
   require(has(db.sql("SELECT id FROM docs WHERE meta.owner IS NOT NULL AND missing IS NULL"),R"({"rows":[{"id":"readme"}]})"),"is null");
   expect_error([&]{db.sql("SELEC * FROM users");},"INPUT SQL");
   expect_error([&]{db.sql("SELECT * FROM users WHERE age > ?");},"parameter 1");
   expect_error([&]{db.sql("BEGIN");},"UNSUPPORTED");
  }
  // Reopen: tables, indexes and the id sequence persist.
  for(int mode=0;mode<2;mode++){ignore_index_snapshot=mode==1;Store db(p);ignore_index_snapshot=false;
   if(mode==0){auto r=db.sql("INSERT INTO users (email) VALUES ('after@x.com')");require(has(r,"\"lastInsertId\":31"),"sequence persists");}
   require(has(db.sql("SELECT id FROM users WHERE age = 37"),R"({"rows":[{"id":7}]})"),"index after reopen");
   expect_error([&]{db.sql("INSERT INTO users (email) VALUES ('u1@x.com')");},"CONFLICT unique");
   require(has(db.sql("SELECT COUNT(*) AS n FROM users"),R"({"rows":[{"n":28}]})"),"rows after reopen");
  }
  {Store db(p);db.sql("DROP INDEX idx_age ON users");db.sql("DROP TABLE docs");expect_error([&]{db.sql("SELECT * FROM docs");},"no such table");
   require(db.indexes().size()==2,"indexes after drops");require(has(db.sql("SELECT COUNT(*) AS n FROM users WHERE age = 37"),R"({"rows":[{"n":1}]})"),"scan without index");}
 });
 test("scan column cache stays exact under random inserts, updates and deletes",[&]{
  auto p=file("colcache");Store db(p,true);
  db.sql("CREATE TABLE t (id INTEGER PRIMARY KEY, g TEXT, v INT)");
  std::map<long long,std::pair<std::string,long long>> model;uint64_t rng=777;long long next=1;
  auto rnd=[&](uint64_t n){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng%n;};
  auto expected=[&](long long minv){
   std::map<std::string,std::pair<long long,long long>> m;for(auto&[id,r]:model)if(r.second>=minv){m[r.first].first++;m[r.first].second+=r.second;}
   std::string s="{\"rows\":[";bool first=true;
   for(auto&[g,c]:m){if(!first)s+=',';first=false;s+="{\"g\":\""+g+"\",\"n\":"+std::to_string(c.first)+",\"s\":"+std::to_string(c.second)+"}";}
   return s+"]}";};
  auto count=[&](const std::string&g){long long n=0;for(auto&[id,r]:model)if(r.first==g)n++;return n;};
  for(int round=0;round<40;round++){
   for(int k=0;k<60;k++){
    auto op=rnd(10);
    if(op<5||model.empty()){std::string g="g"+std::to_string(rnd(7));long long v=(long long)rnd(100);db.sql("INSERT INTO t (g, v) VALUES (?, ?)","[\""+g+"\","+std::to_string(v)+"]");model[next++]={g,v};}
    else{auto it=model.begin();std::advance(it,(long)rnd(model.size()));
     if(op<8){long long v=(long long)rnd(100);std::string g="g"+std::to_string(rnd(7));db.sql("UPDATE t SET v = ?, g = ? WHERE id = ?","["+std::to_string(v)+",\""+g+"\","+std::to_string(it->first)+"]");it->second={g,v};}
     else{db.sql("DELETE FROM t WHERE id = ?","["+std::to_string(it->first)+"]");model.erase(it);}}
   }
   long long minv=(long long)rnd(60);
   require(db.sql("SELECT g, COUNT(*) AS n, SUM(v) AS s FROM t WHERE v >= ? GROUP BY g ORDER BY g","["+std::to_string(minv)+"]")==expected(minv),"grouped aggregate");
   require(db.sql("SELECT COUNT(*) AS n FROM t WHERE g = 'g3'")=="{\"rows\":[{\"n\":"+std::to_string(count("g3"))+"}]}","count");
  }
 });
 test("SQL transactions: read-your-writes, isolation, rollback, unique checks, one writer at a time",[&]{
  auto p=file("txn");Store db(p,true);
  auto has=[](const std::string&s,const std::string&part){if(s.find(part)==std::string::npos){std::cout<<"  result: "<<s<<"\n  missing: "<<part<<std::endl;return false;}return true;};
  db.sql("CREATE TABLE acct (id INTEGER PRIMARY KEY, owner TEXT UNIQUE, balance INT)");
  db.sql("INSERT INTO acct (owner, balance) VALUES ('a', 100), ('b', 50)");
  Store::SqlSession s1;
  db.sql(&s1,"BEGIN","[]");
  db.sql(&s1,"UPDATE acct SET balance = balance - 30 WHERE owner = 'a'","[]");
  db.sql(&s1,"UPDATE acct SET balance = balance + 30 WHERE owner = 'b'","[]");
  db.sql(&s1,"INSERT INTO acct (owner, balance) VALUES ('c', 7)","[]");
  require(has(db.sql(&s1,"SELECT owner, balance FROM acct ORDER BY owner","[]"),R"({"rows":[{"owner":"a","balance":70},{"owner":"b","balance":80},{"owner":"c","balance":7}]})"),"read your writes");
  require(has(db.sql(&s1,"SELECT SUM(balance) AS t, COUNT(*) AS n FROM acct","[]"),R"({"rows":[{"t":157,"n":3}]})"),"aggregate sees own writes");
  require(has(db.sql("SELECT owner, balance FROM acct ORDER BY owner"),R"({"rows":[{"owner":"a","balance":100},{"owner":"b","balance":50}]})"),"others see the committed state");
  expect_error([&]{db.sql(&s1,"INSERT INTO acct (owner) VALUES ('c')","[]");},"CONFLICT unique");  // checked now, against own insert
  expect_error([&]{db.commit({{std::string("kv")+'\0'+"x","1",ANY,false}});},"transaction is open");  // same thread: refused, not a deadlock
  // Another writer waits for COMMIT; readers never wait.
  std::atomic<bool> other_done{false};
  std::thread other([&]{db.sql("INSERT INTO acct (owner, balance) VALUES ('d', 1)");other_done=true;});
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  require(!other_done,"a second writer waits for the open transaction");
  require(has(db.sql("SELECT COUNT(*) AS n FROM acct"),R"({"rows":[{"n":2}]})"),"readers do not wait");
  require(has(db.sql(&s1,"COMMIT","[]"),R"("changes":3)"),"commit");
  other.join();require(other_done,"writer continued after commit");
  require(has(db.sql("SELECT owner, balance FROM acct ORDER BY owner"),R"({"rows":[{"owner":"a","balance":70},{"owner":"b","balance":80},{"owner":"c","balance":7},{"owner":"d","balance":1}]})"),"committed atomically");
  // Rollback leaves nothing; a failed COMMIT ends the transaction.
  db.sql(&s1,"BEGIN","[]");db.sql(&s1,"DELETE FROM acct","[]");
  require(has(db.sql(&s1,"SELECT COUNT(*) AS n FROM acct","[]"),R"({"rows":[{"n":0}]})"),"delete visible inside");
  db.sql(&s1,"ROLLBACK","[]");
  require(has(db.sql("SELECT COUNT(*) AS n FROM acct"),R"({"rows":[{"n":4}]})"),"rollback");
  expect_error([&]{db.sql(&s1,"COMMIT","[]");},"no transaction");
  db.sql(&s1,"BEGIN","[]");expect_error([&]{db.sql(&s1,"CREATE INDEX ON acct (balance)","[]");},"inside a transaction");
  db.sql(&s1,"INSERT INTO acct (owner, balance) VALUES ('tmp', 1)","[]");db.sql(&s1,"DELETE FROM acct WHERE owner = 'tmp'","[]");
  require(has(db.sql(&s1,"COMMIT","[]"),R"("changes":0)"),"created-and-deleted row leaves no trace");
  db.end_session(s1);
  // An abandoned transaction (connection closed) releases the gate.
  Store::SqlSession s2;db.sql(&s2,"BEGIN","[]");db.sql(&s2,"UPDATE acct SET balance = 0","[]");db.end_session(s2);
  db.sql("UPDATE acct SET balance = balance + 1 WHERE owner = 'a'");
  require(has(db.sql("SELECT balance FROM acct WHERE owner = 'a'"),R"({"rows":[{"balance":71}]})"),"abandoned transaction discarded");
 });
 test("SQL joins and subqueries agree with and without indexes",[&]{
  auto p=file("join");Store db(p,true);
  auto has=[](const std::string&s,const std::string&part){if(s.find(part)==std::string::npos){std::cout<<"  result: "<<s<<"\n  missing: "<<part<<std::endl;return false;}return true;};
  db.sql("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, city TEXT)");
  db.sql("CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INT, total INT)");
  db.sql("INSERT INTO users (name, city) VALUES ('Ann','Colombo'),('Bob','Kandy'),('Cid','Colombo')");
  db.sql("INSERT INTO orders (user_id, total) VALUES (1, 30), (1, 70), (2, 20), (9, 5)");
  for(int pass=0;pass<2;pass++){
   if(pass==1)db.sql("CREATE INDEX ON orders (user_id)");
   require(has(db.sql("SELECT u.name, o.total FROM users u JOIN orders o ON o.user_id = u.id ORDER BY o.total"),R"({"rows":[{"name":"Bob","total":20},{"name":"Ann","total":30},{"name":"Ann","total":70}]})"),"inner join");
   require(has(db.sql("SELECT u.name, COUNT(o.id) AS n, SUM(o.total) AS spent FROM users u LEFT JOIN orders o ON o.user_id = u.id GROUP BY u.name ORDER BY u.name"),R"({"rows":[{"name":"Ann","n":2,"spent":100},{"name":"Bob","n":1,"spent":20},{"name":"Cid","n":0,"spent":null}]})"),"left join aggregate");
   require(has(db.sql("SELECT u.name FROM users u LEFT JOIN orders o ON o.user_id = u.id WHERE o.id IS NULL"),R"({"rows":[{"name":"Cid"}]})"),"anti join");
   require(has(db.sql("SELECT o.* FROM orders o INNER JOIN users ON users.id = o.user_id WHERE users.city = ? AND o.total > ?","[\"Colombo\",50]"),R"({"rows":[{"id":2,"user_id":1,"total":70}]})"),"alias.star + where");
   require(has(db.sql("SELECT name FROM users WHERE id IN (SELECT user_id FROM orders WHERE total >= ?) ORDER BY name","[20]"),R"({"rows":[{"name":"Ann"},{"name":"Bob"}]})"),"IN subquery");
   require(has(db.sql("SELECT name FROM users WHERE NOT EXISTS (SELECT id FROM orders WHERE total > 100)"),R"("rows":[{"name":"Ann"},{"name":"Bob"},{"name":"Cid"}])"),"NOT EXISTS");
   require(has(db.sql("SELECT id, total FROM orders WHERE total = (SELECT MAX(total) FROM orders)"),R"({"rows":[{"id":2,"total":70}]})"),"scalar subquery");
   require(has(db.sql("SELECT a.name AS a, b.name AS b FROM users a, users b WHERE a.city = b.city AND a.id < b.id"),R"({"rows":[{"a":"Ann","b":"Cid"}]})"),"self join (comma)");
  }
  require(has(db.sql("UPDATE orders SET total = total + 1 WHERE user_id IN (SELECT id FROM users WHERE city = 'Kandy')"),R"({"changes":1})"),"update with subquery");
  require(has(db.sql("SELECT u.name, o.total FROM users u RIGHT JOIN orders o ON o.user_id = u.id WHERE u.id IS NULL"),R"({"rows":[{"name":null,"total":5}]})"),"right join unmatched");
  expect_error([&]{db.sql("SELECT * FROM users NATURAL JOIN orders");},"not supported");
  expect_error([&]{db.sql("SELECT name FROM users WHERE id = (SELECT user_id FROM orders)");},"more than one row");
 });
 test("SQL schemas: column types, NOT NULL, DEFAULT and ALTER TABLE, persisted",[&]{
  auto p=file("schema");
  auto has=[](const std::string&s,const std::string&part){if(s.find(part)==std::string::npos){std::cout<<"  result: "<<s<<"\n  missing: "<<part<<std::endl;return false;}return true;};
  {Store db(p,true);
   db.sql("CREATE TABLE items (id INTEGER PRIMARY KEY, sku VARCHAR(32) NOT NULL UNIQUE, qty INT DEFAULT 0, price DECIMAL(10,2), active BOOLEAN DEFAULT TRUE, meta JSON) ENGINE=InnoDB");
   require(has(db.sql("INSERT INTO items (sku, price) VALUES ('a', 9.5)"),R"("lastInsertId":1)"),"insert with defaults");
   require(has(db.sql("SELECT * FROM items WHERE id = 1"),R"({"rows":[{"id":1,"sku":"a","price":9.5,"qty":0,"active":true}]})"),"defaults filled");
   expect_error([&]{db.sql("INSERT INTO items (sku, qty) VALUES ('b', 'many')");},"expects an integer");
   expect_error([&]{db.sql("INSERT INTO items (sku, qty) VALUES ('b', 1.5)");},"expects an integer");
   expect_error([&]{db.sql("INSERT INTO items (price) VALUES (1)");},"NOT NULL");
   expect_error([&]{db.sql("INSERT INTO items (sku) VALUES (NULL)");},"cannot be NULL");
   expect_error([&]{db.sql("INSERT INTO items (sku, active) VALUES ('b', 1)");},"expects a boolean");
   expect_error([&]{db.sql("UPDATE items SET sku = NULL WHERE id = 1");},"cannot be NULL");
   expect_error([&]{db.sql("UPDATE items SET price = 'free' WHERE id = 1");},"expects a number");
   db.sql("INSERT INTO items (sku, meta, extra) VALUES ('b', ?, 'undeclared columns are allowed')","[{\"tags\":[1,2]}]");
   expect_error([&]{db.sql("INSERT INTO items (id, sku) VALUES (1, 'z') ON CONFLICT (id) DO UPDATE SET qty = 'x'");},"expects an integer");
   // ALTER TABLE: add with default (backfills), rename (index follows), drop (index goes).
   expect_error([&]{db.sql("ALTER TABLE items ADD COLUMN owner TEXT NOT NULL");},"needs a DEFAULT");
   require(has(db.sql("ALTER TABLE items ADD COLUMN warehouse TEXT NOT NULL DEFAULT 'main'"),R"({"changes":2})"),"add backfills");
   db.sql("CREATE INDEX ON items (warehouse)");
   require(has(db.sql("SELECT COUNT(*) AS n FROM items WHERE warehouse = 'main'"),R"({"rows":[{"n":2}]})"),"backfilled");
   require(has(db.sql("ALTER TABLE items RENAME COLUMN warehouse TO site"),R"({"changes":2})"),"rename");
   require(has(db.sql("SELECT sku, site FROM items ORDER BY id"),R"({"rows":[{"sku":"a","site":"main"},{"sku":"b","site":"main"}]})"),"renamed values");
   bool on_site=false;for(auto&ix:db.indexes())if(ix.field=="site")on_site=true;require(on_site,"index moved with rename");
   expect_error([&]{db.sql("INSERT INTO items (sku, site) VALUES ('c', 5)");},"expects text");
   require(has(db.sql("ALTER TABLE items DROP COLUMN site"),R"({"changes":2})"),"drop");
   require(has(db.sql("SELECT * FROM items WHERE id = 1"),R"({"rows":[{"id":1,"sku":"a","price":9.5,"qty":0,"active":true}]})"),"dropped from rows");
   for(auto&ix:db.indexes())require(ix.field!="site","index dropped with column");
   expect_error([&]{db.sql("ALTER TABLE items DROP COLUMN id");},"cannot be dropped");
  }
  for(int mode=0;mode<2;mode++){ignore_index_snapshot=mode==1;Store db(p);ignore_index_snapshot=false;
   expect_error([&]{db.sql("INSERT INTO items (sku, qty) VALUES ('q', 'x')");},"expects an integer");  // schema persisted
   require(has(db.sql("SELECT qty, active FROM items WHERE sku = 'b'"),R"({"rows":[{"qty":0,"active":true}]})"),"defaults persisted");
  }
 });
 test("compact index matches std::unordered_map: short and long keys, updates, erases, arena compaction",[&]{
  for(int pass=0;pass<2;pass++){
   CompactIndex<Ref> idx;std::unordered_map<std::string,Ref> ref;uint64_t rng=99+pass;
   auto hash=[&](const std::string&k){uint64_t h=index_hash(digest_key(),k);return pass?(h&7)*0x1000000000000001ULL:h;};
   for(int step=0;step<150000;step++){
    rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;
    std::string k=(rng%3?"k":std::string(40+rng%60,'L'))+std::to_string(rng%(pass?400:6000));  // long keys use the arena
    auto h=hash(k);
    if(rng%4==0){require(idx.erase(k,h)==ref.erase(k),"erase");continue;}
    Ref r;r.offset=rng%(uint64_t(1)<<50);r.version=rng%(uint64_t(1)<<38)+1;r.length=uint32_t(rng%(1u<<24));for(int b=0;b<16;b++)r.hash[b]=uint8_t(rng>>(b*3));
    Ref old;bool fresh=idx.put(k,h,r,&old);auto it=ref.find(k);
    require(fresh==(it==ref.end()),"put result");if(!fresh)require(old.offset==it->second.offset&&old.version==it->second.version&&old.length==it->second.length&&digest_eq(old.hash,it->second.hash),"old value");
    ref[k]=r;
    if(step%4999==0){
     require(idx.size()==ref.size(),"size");
     for(auto&[rk,rv]:ref){Ref f;require(idx.find(rk,hash(rk),f)&&f.offset==rv.offset&&f.version==rv.version&&f.length==rv.length&&digest_eq(f.hash,rv.hash),"lookup");}
     size_t seen=0;idx.for_each([&](std::string_view k2,const Ref&v){auto e=ref.find(std::string(k2));require(e!=ref.end()&&e->second.offset==v.offset,"iteration");seen++;});require(seen==ref.size(),"iteration count");
    }
   }
  }
  CompactIndex<Ref> lim;Ref big;big.version=uint64_t(1)<<39;expect_error([&]{lim.put("k",1,big);},"LIMIT");
 });
 test("concurrent compare-and-swap has exactly one winner",[&]{Store db(file("cas"),true);db.commit({{"key","initial",0,false}});std::atomic<int>wins{0};std::vector<std::thread> threads;for(int i=0;i<16;i++)threads.emplace_back([&]{try{db.commit({{"key","changed",1,false}});++wins;}catch(const std::exception&e){if(std::string(e.what()).find("CONFLICT")==std::string::npos)wins+=100;}});for(auto&t:threads)t.join();require(wins==1,"CAS winners");});
 test("streamed checkpoint larger than 64 MiB reopens and accepts CAS",[&]{auto p=file("large"),q=file("large-compact");{Options o;o.reads=ReadMode::Pread;Store db(p,true,o);std::string value(1024*1024,'x');for(int i=0;i<68;i++)db.commit({{std::to_string(i),value,0,false}});db.snapshot(q,true);}Store copy(q);require(copy.count()==68&&copy.txid()==68&&copy.get("20").version==21,"large checkpoint");copy.commit({{"20","small",21,false}});copy.verify();});
 test("paged index: random sets, deletes and CAS match a reference across merges and reopens",[&]{
  auto p=file("paged");Options o;o.indexMemory=16*1024;o.snapshotIdleMs=0;
  std::map<std::string,std::pair<uint64_t,std::string>> ref;uint64_t rng=12345;
  auto next=[&]{rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;};
  auto key=[&](uint64_t r){std::string k=(r%5?"key":std::string(30+r%90,'L'))+std::to_string(r%9000);return k;};  // short and long keys
  auto check=[&](Store&db,const char*what){
   require(db.count()==ref.size(),what);
   for(auto&[k,v]:ref){auto x=db.get(k);require(x.found&&x.version==v.first&&x.value==v.second,what);}
   for(int i=0;i<300;i++){auto k=key(next());auto x=db.get(k);auto it=ref.find(k);require(x.found==(it!=ref.end()),what);}
  };
  auto work=[&](Store&db,int txs){
   for(int t=0;t<txs;t++){
    std::vector<Op> ops;std::set<std::string> used;int n=1+int(next()%40);
    for(int i=0;i<n;i++){auto r=next();auto k=key(r);if(!used.insert(k).second)continue;
     if(r%7==0)ops.push_back({k,"",ANY,true});
     else if(r%11==0){auto it=ref.find(k);ops.push_back({k,"cas"+std::to_string(r%100),it==ref.end()?0:it->second.first,false});}
     else ops.push_back({k,r%13==0?std::string():std::string(size_t(r%200),char('a'+r%26)),ANY,false});}
    if(ops.empty())continue;
    auto v=db.commit(ops);
    for(auto&op:ops){if(op.erase)ref.erase(op.key);else ref[op.key]={v,op.value};}
   }
  };
  {Store db(p,true,o);work(db,600);db.flush_index();require(std::string(db.index_mode())=="paged","paged after flush");check(db,"after flush");
   work(db,600);check(db,"merges while writing");db.verify();}
  {Store db(p,false,o);require(std::string(db.opened_from())=="index-file"&&std::string(db.index_mode())=="paged","opened from the index file");check(db,"reopen");
   work(db,400);check(db,"writes after reopen");db.wait_for_verification();require(std::string(db.verification())=="passed","blocks verified");}
  {Store db(p,false,o);check(db,"second reopen");db.snapshot(file("paged-compact"),true);}
  {Store c(file("paged-compact"),false,o);check(c,"compacted copy");}
  // The whole index in memory again (paging off): same contents from the log.
  {ignore_index_snapshot=true;Options m;m.indexMemory=0;Store db(p,false,m);ignore_index_snapshot=false;require(std::string(db.opened_from())=="log-replay","replay");check(db,"replayed");}
 });
 test("paged index: a crash replays the log after each shard's table; damaged files fall back to the log",[&]{
  auto p=file("pcrash");Options o;o.indexMemory=8*1024;o.snapshotIdleMs=0;
  auto copy=[&](const std::string&from,const std::string&to){FD in(platform::open_file(from));auto bytes=read_at(in.n,size_t(file_size(in.n)),0);FD out(platform::open_file(to,true));write_at(out.n,bytes.data(),bytes.size(),0);};
  std::map<std::string,std::string> ref;
  Store db(p,true,o);
  for(int i=0;i<4000;i++){auto k="c"+std::to_string(i);db.commit({{k,"v"+std::to_string(i),ANY,false}});ref[k]="v"+std::to_string(i);}
  db.flush_index();
  for(int i=0;i<4000;i+=3){auto k="c"+std::to_string(i);if(i%2){db.commit({{k,"",ANY,true}});ref.erase(k);}else{db.commit({{k,"w",ANY,false}});ref[k]="w";}}
  for(int i=4000;i<4500;i++){auto k="c"+std::to_string(i);db.commit({{k,"n",ANY,false}});ref[k]="n";}
  auto same=[&](Store&c,const char*what){require(c.count()==ref.size(),what);for(int i=0;i<4600;i++){auto k="c"+std::to_string(i);auto x=c.get(k);auto it=ref.find(k);require(x.found==(it!=ref.end())&&(!x.found||x.value==it->second),what);}};
  copy(p,file("pc1"));copy(p+".idx",file("pc1")+".idx");
  {Store c(file("pc1"),false,o);require(std::string(c.opened_from())=="index-file","crash copy opened from the index file");same(c,"suffix replay");c.verify();}
  // Damaged header: the log is replayed.
  copy(p,file("pc2"));copy(p+".idx",file("pc2")+".idx");
  {FD f(platform::open_file(file("pc2")+".idx"));for(uint64_t at:{uint64_t(100),uint64_t(16384+100)}){auto b=read_at(f.n,1,at);b[0]^=1;write_at(f.n,b.data(),1,at);}}
  {Store c(file("pc2"),false,o);require(std::string(c.opened_from())=="log-replay","damaged header ignored");same(c,"after damaged header");}
  // Damaged table block: reads that touch it fail until the maintainer rebuilds from the log.
  copy(p,file("pc3"));copy(p+".idx",file("pc3")+".idx");
  {FD f(platform::open_file(file("pc3")+".idx"));auto at=uint64_t(65536)+10;auto b=read_at(f.n,1,at);b[0]^=0x55;write_at(f.n,b.data(),1,at);}
  {Store c(file("pc3"),false,o);bool ok=false;
   for(int attempt=0;attempt<200&&!ok;attempt++){try{same(c,"after damaged block");ok=true;}catch(const std::exception&e){require(std::string(e.what()).find("CORRUPT")!=std::string::npos,"damage reported as corruption");std::this_thread::sleep_for(std::chrono::milliseconds(20));}}
   require(ok,"rebuilt from the log");c.verify();}
  {Store c(file("pc3"),false,o);same(c,"reopen after rebuild");}
  db.verify();
 });
 test("paged index: readers and queries stay consistent while merges run",[&]{
  auto p=file("pconc");Options o;o.indexMemory=8*1024;o.snapshotIdleMs=0;Store db(p,true,o);
  for(int i=0;i<2000;i++)db.commit({{"r"+std::to_string(i),"0",ANY,false}});
  std::atomic<bool> stop{false};std::atomic<int> bad{0};std::vector<std::thread> readers;
  for(int t=0;t<4;t++)readers.emplace_back([&,t]{uint64_t r=t+1;while(!stop){r^=r<<13;r^=r>>7;r^=r<<17;try{auto x=db.get("r"+std::to_string(r%2000));if(!x.found||std::to_string(x.version)!=x.value&&x.value!="0")bad++;}catch(...){bad++;}}});
  for(int round=0;round<3000;round++){auto k="r"+std::to_string((round*7919)%2000);uint64_t v=db.txid()+1;db.commit({{k,std::to_string(v),ANY,false}});if(round%1000==999)db.flush_index();}
  stop=true;for(auto&t:readers)t.join();require(!bad,"reads during merges");require(db.count()==2000,"count");
  db.sql("CREATE TABLE pt (id INTEGER PRIMARY KEY, n INTEGER, tag TEXT)");
  for(int i=0;i<1500;i+=50){std::string q="INSERT INTO pt (n, tag) VALUES ";for(int j=i;j<i+50;j++){if(j>i)q+=",";q+="("+std::to_string(j)+",'"+(j%3?"a":"b")+"')";}db.sql(q);}
  db.flush_index();
  require(db.sql("SELECT COUNT(*) AS n FROM pt").find("\"n\":1500")!=std::string::npos,"count rows");
  require(db.sql("SELECT COUNT(*) AS n FROM pt WHERE tag = 'b'").find("\"n\":500")!=std::string::npos,"filtered count");
  db.sql("DELETE FROM pt WHERE n < 100");require(db.sql("SELECT COUNT(*) AS n FROM pt").find("\"n\":1400")!=std::string::npos,"after delete");
  require(db.sql("SELECT n FROM pt ORDER BY n LIMIT 1").find("\"n\":100")!=std::string::npos,"ordered");
 });
 test("paged index: a database closed with the whole index in memory opens with paging on",[&]{
  auto p=file("pmigrate");{Options m;m.indexMemory=0;Store db(p,true,m);for(int i=0;i<3000;i++)db.commit({{"m"+std::to_string(i),std::to_string(i),ANY,false}});}
  Options o;o.indexMemory=8*1024;o.snapshotIdleMs=0;
  {Store db(p,false,o);require(std::string(db.opened_from())=="index-snapshot","tail snapshot");db.commit({{"m0","x",ANY,false}});db.flush_index();require(std::string(db.index_mode())=="paged","paged");}
  {Store db(p,false,o);require(std::string(db.opened_from())=="index-file"&&db.count()==3000&&db.get("m0").value=="x"&&db.get("m2999").value=="2999","migrated");}
 });
 test("paged sorted set matches std::set while pages move to the scratch file and back",[&]{
  PageFile pf(file("set-pages"));SetMemory mem;
  {SortedSet set;set.attach(&pf,&mem);std::set<std::string> ref;uint64_t rng=777;
   auto rnd=[&]{rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;};
   auto key=[&](uint64_t r){return (r%4?std::string("k"):std::string(200+r%300,'w'))+std::to_string(r%20000);};
   for(int step=0;step<60000;step++){
    auto r=rnd();
    if(r%5==0){auto k=key(rnd());require(set.erase(k)==(ref.erase(k)==1),"erase");}
    else if(r%37==0){std::vector<std::string> b;for(int i=0;i<50;i++)b.push_back(key(rnd()));std::sort(b.begin(),b.end());b.erase(std::unique(b.begin(),b.end()),b.end());
     size_t want=0;for(auto&x:b)want+=ref.insert(x).second;std::vector<std::string_view> v(b.begin(),b.end());require(set.insert_sorted(v.begin(),v.end())==want,"insert_sorted");}
    else{auto k=key(r);require(set.insert(k)==ref.insert(k).second,"insert");}
    if(step%500==0){std::vector<size_t> pages;set.each_ram_page([&](uint64_t,size_t c){pages.push_back(c);});for(size_t i=0;i<pages.size();i+=2)set.spill_page(pages[i],false);pf.publish();pf.release_retired();}
    if(step%5000==4999){
     require(set.size()==ref.size(),"size");
     auto it=ref.begin();for(auto p=set.seek("");set.valid(p);p=set.next(p),++it)require(it!=ref.end()&&set.get(p)==*it,"forward");require(it==ref.end(),"forward end");
     auto rt=ref.rbegin();for(auto p=set.prev(set.end());set.valid(p);p=set.prev(p),++rt)require(rt!=ref.rend()&&set.get(p)==*rt,"backward");
     for(int i=0;i<200;i++){auto k=key(rnd());auto lb=ref.lower_bound(k);auto p=set.seek(k);require(set.valid(p)==(lb!=ref.end())&&(!set.valid(p)||set.get(p)==*lb),"seek");require(set.contains(k)==ref.count(k),"contains");}
    }
   }
   require(mem.bytes.load()>0,"accounted");
  }
  require(mem.bytes.load()==0,"memory returned");
  // Bulk build straight to the file, sorted by the external sorter in 64 KB runs.
  std::vector<std::string> all;uint64_t r2=5;for(int i=0;i<200000;i++){r2^=r2<<13;r2^=r2>>7;r2^=r2<<17;all.push_back("e"+std::to_string(r2%1000000)+std::string(r2%7,'x'));}
  ExternalSorter sorter(file("set-sort"),1<<16);for(auto&x:all)sorter.add(x);
  std::sort(all.begin(),all.end());std::vector<std::string> got;sorter.finish([&](std::string_view x){got.emplace_back(x);});require(got==all,"external sort");
  all.erase(std::unique(all.begin(),all.end()),all.end());
  SortedSet b;b.attach(&pf,&mem);for(auto&x:all)b.build_add(x,0);b.build_end();
  size_t i=0;for(auto p=b.seek("");b.valid(p);p=b.next(p))require(b.get(p)==all[i++],"built");require(i==all.size()&&b.size()==all.size(),"built size");
  require(b.contains(all[777])&&b.erase(all[777])&&!b.contains(all[777]),"edit a page from the file");
 });
 test("SQL with ordered keys and indexes paged to the scratch file gives the same answers",[&]{
  Options small;small.orderedMemory=64*1024;small.snapshotIdleMs=0;Options big;big.orderedMemory=0;
  Store a(file("sql-paged"),true,small),b(file("sql-ram"),true,big);
  auto both=[&](const std::string&q){auto x=a.sql(q),y=b.sql(q);require(x==y,("differs: "+q).c_str());return x;};
  both("CREATE TABLE people (id INTEGER PRIMARY KEY, email TEXT, city TEXT, age INTEGER)");
  both("CREATE UNIQUE INDEX people_email ON people (email)");both("CREATE INDEX people_city ON people (city)");
  uint64_t rng=9;auto rnd=[&]{rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;};
  for(int batch=0;batch<60;batch++){std::string q="INSERT INTO people (email, city, age) VALUES ";
   for(int j=0;j<200;j++){int n=batch*200+j;if(j)q+=",";q+="('u"+std::to_string(n)+"@x.org','city"+std::to_string(rnd()%50)+"',"+std::to_string(rnd()%90)+")";}both(q);}
  require(both("SELECT COUNT(*) AS n FROM people").find("\"n\":12000")!=std::string::npos,"rows");
  for(int i=0;i<40;i++){
   both("SELECT id, email FROM people WHERE city = 'city"+std::to_string(i)+"' ORDER BY id LIMIT 20");
   both("SELECT id FROM people WHERE id >= "+std::to_string(i*250)+" ORDER BY id DESC LIMIT 15");
   both("SELECT * FROM people WHERE email = 'u"+std::to_string(i*97)+"@x.org'");
   both("UPDATE people SET city = 'moved', email = 'm"+std::to_string(i)+"@x.org' WHERE id = "+std::to_string(i*31+1));
   both("DELETE FROM people WHERE id = "+std::to_string(i*53+7));
  }
  both("SELECT city, COUNT(*) AS n FROM people GROUP BY city ORDER BY city");
  both("SELECT id FROM people WHERE city = 'moved' ORDER BY id");
  expect_error([&]{a.sql("INSERT INTO people (email, city, age) VALUES ('u5@x.org','c',1)");},"CONFLICT");
  require(a.ordered_memory()<=512*1024&&a.ordered_file_bytes()>0,"pages moved to the scratch file");
  a.verify();
 });
 test("SQL writes from many threads share commits: no lost updates, unique conflicts per statement",[&]{
  Store db(file("sql-group"),true);
  db.sql("CREATE TABLE acct (id INTEGER PRIMARY KEY, email TEXT, n INTEGER)");db.sql("CREATE UNIQUE INDEX acct_email ON acct (email)");
  db.sql("INSERT INTO acct (id, email, n) VALUES (1, 'counter@x', 0)");
  std::atomic<int> conflicts{0},bad{0};std::vector<std::thread> th;std::mutex idm;std::set<long long> ids;
  for(int t=0;t<16;t++)th.emplace_back([&,t]{
   for(int i=0;i<150;i++){
    try{
     if(i%3==0){db.sql("UPDATE acct SET n = n + 1 WHERE id = 1");continue;}
     // Every 10th insert reuses an email another thread also uses: exactly one of each pair wins.
     std::string email=i%10==1?"shared"+std::to_string(i)+"@x":"t"+std::to_string(t)+"-"+std::to_string(i)+"@x";
     auto r=db.sql("INSERT INTO acct (email, n) VALUES (?, 0)","[\""+email+"\"]");
     auto p=r.find("\"lastInsertId\":");if(p==std::string::npos){bad++;continue;}
     std::lock_guard<std::mutex>g(idm);if(!ids.insert(atoll(r.c_str()+p+15)).second)bad++;
    }catch(const std::exception&e){if(std::string(e.what()).find("CONFLICT")!=std::string::npos)conflicts++;else bad++;}
   }});
  for(auto&x:th)x.join();
  require(!bad,"every statement answered correctly");
  require(db.sql("SELECT n FROM acct WHERE id = 1").find("\"n\":800")!=std::string::npos,"no lost updates");  // 16 threads x 50
  int shared=0;for(int i=0;i<150;i++)if(i%10==1&&i%3!=0)shared++;
  require(conflicts==15*shared,"one winner per shared email");
  size_t inserted=ids.size();require(db.sql("SELECT COUNT(*) AS c FROM acct").find("\"c\":"+std::to_string(inserted+1))!=std::string::npos,"row count");
  db.verify();
 });
 test("online compaction: writers and readers keep running; every value, index and reopen agree",[&]{
  for(int mode=0;mode<3;mode++){
   Options o;o.snapshotIdleMs=0;if(mode==1)o.indexMemory=8*1024;if(mode==2)o.reads=ReadMode::Pread;
   auto p=file("compact"+std::to_string(mode));
   std::map<std::string,std::string> ref;std::mutex rm;size_t extra_after=0;
   {
    Store db(p,true,o);
    db.sql("CREATE TABLE items (id INTEGER PRIMARY KEY, tag TEXT)");db.sql("CREATE INDEX items_tag ON items (tag)");
    for(int i=0;i<300;i++)db.sql("INSERT INTO items (tag) VALUES (?)","[\""+std::string(i%3?"a":"b")+"\"]");
    // Garbage: every key written five times.
    for(int round=0;round<5;round++){std::vector<Op> ops;for(int i=0;i<4000;i++){auto k="k"+std::to_string(i);auto v=k+":"+std::to_string(round)+std::string(60,'x');ops.push_back({k,v,ANY,false});if(ops.size()==500){db.commit(ops);ops.clear();}}}
    for(int i=0;i<4000;i++)ref["k"+std::to_string(i)]="k"+std::to_string(i)+":4"+std::string(60,'x');
    const size_t extra=db.count()-ref.size();  // SQL rows and the engine's own records
    std::atomic<bool> stop{false};std::atomic<int> bad{0},reads{0};
    std::thread writer([&]{uint64_t r=7;int n=0;while(!stop){r^=r<<13;r^=r>>7;r^=r<<17;auto k="k"+std::to_string(r%5000);
      try{if(r%4==0){db.commit({{k,"",ANY,true}});std::lock_guard<std::mutex>g(rm);ref.erase(k);}
       else{auto v=k+":w"+std::to_string(n++);db.commit({{k,v,ANY,false}});std::lock_guard<std::mutex>g(rm);ref[k]=v;}}catch(...){bad++;}}});
    std::vector<std::thread> readers;
    for(int t=0;t<2;t++)readers.emplace_back([&,t]{uint64_t r=t+11;std::string out;uint64_t ver;while(!stop){r^=r<<13;r^=r>>7;r^=r<<17;auto k="k"+std::to_string(r%5000);
      try{if(db.read(k,out,ver)&&out.compare(0,k.size()+1,k+":")!=0)bad++;reads++;}catch(...){bad++;}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto res=db.compact();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop=true;writer.join();for(auto&x:readers)x.join();
    require(!bad,"no failed read or write during compaction");require(reads>0,"readers ran");
    require(res.after<res.before,"file shrank");
    require(db.count()==ref.size()+extra,"count after compaction");
    for(auto&[k,v]:ref)require(db.get(k).value==v,"value after compaction");
    require(db.sql("SELECT COUNT(*) AS n FROM items WHERE tag = 'b'").find("\"n\":100")!=std::string::npos,"index after compaction");
    db.sql("INSERT INTO items (tag) VALUES ('c')");db.commit({{"after","x",ANY,false}});ref["after"]="x";
    db.verify();
    auto again=db.compact();require(again.after<=res.after+4096,"second compaction");
    require(db.get("after").value=="x","writes after the second compaction");
    extra_after=db.count()-ref.size();
   }
   require(!std::filesystem::exists(std::filesystem::u8path(p+".compacted-old-1"))&&!std::filesystem::exists(std::filesystem::u8path(p+".compacted-old-2")),"old files deleted on close");
   require(!std::filesystem::exists(std::filesystem::u8path(p+".compacted-old")),"leftover marker removed on a clean close");
   {Store db(p,false,o);require(db.count()==ref.size()+extra_after,"reopen count");for(auto&[k,v]:ref)require(db.get(k).value==v,"reopen value");db.verify();
    require(db.sql("SELECT COUNT(*) AS n FROM items").find("\"n\":301")!=std::string::npos,"rows after reopen");}
  }
  // A crash in the middle of the Windows switch leaves only <file>.compacted-old: it is renamed back.
  auto p=file("compact-crash");{Store db(p,true);db.commit({{"k","v",ANY,false}});}
  std::filesystem::rename(std::filesystem::u8path(p),std::filesystem::u8path(p+".compacted-old-1"));
  {Store db(p);require(db.get("k").value=="v","recovered after an interrupted switch");}
  // An old copy a crash left behind: removed on open when the marker says so; without the
  // marker the directory is not searched (and the copy, not ours to judge, stays).
  auto copy=[&](const std::string&n){std::ofstream(std::filesystem::u8path(n),std::ios::binary)<<"old";};
  copy(p+".compacted-old-7");
  {Store db(p);}require(std::filesystem::exists(std::filesystem::u8path(p+".compacted-old-7")),"no marker: the directory is not searched");
  copy(p+".compacted-old");
  {Store db(p);require(db.get("k").value=="v","open with a leftover marker");}
  require(!std::filesystem::exists(std::filesystem::u8path(p+".compacted-old-7"))&&!std::filesystem::exists(std::filesystem::u8path(p+".compacted-old")),"leftover copy and marker removed");
 });
 std::filesystem::remove_all(root);std::cout<<passed<<" portable tests passed on "<<platform::name<<"; SHA="<<hashing::backend<<std::endl;return 0;
 }catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<" at "<<root.u8string()<<std::endl;return 1;}
}

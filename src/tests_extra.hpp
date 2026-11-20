#pragma once
std::string hex(const Hash&h){std::string out;for(auto c:h){out.push_back("0123456789abcdef"[c>>4]);out.push_back("0123456789abcdef"[c&15]);}return out;}
void extra_tests(const std::string&root){
 test("SHA-256 known answers survive context reuse",[&]{
  require(hex(digest("",0))=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA empty");
  require(hex(digest("abc",3))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA abc");
  require(hex(digest("",0))=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA reset");
 });
 test("BLAKE3 official known answers across chunk boundaries",[&]{
  #include "blake3_vectors.inc"
 });
 test("mapped reads cross 64 MiB windows and reopen identically with pread",[&]{
  auto p=root+"/windows";std::string large(15*1024*1024,'a'),cross(8*1024*1024,'b');
  {Store db(p,true);for(int i=0;i<4;i++)db.commit({{std::to_string(i),large,ANY,false}});db.commit({{"cross",cross,ANY,false}});require(db.get("cross").value==cross,"mapped crossing");require(db.get("0").value==large,"old mapping");db.verify();}
  {Store db(p);require(db.get("cross").value==cross,"mapped reopen");}
  {Options o;o.reads=ReadMode::Pread;o.orderedSync=true;Store db(p,false,o);require(db.get("cross").value==cross,"pread reopen");db.commit({{"last","ok",ANY,false}});db.verify();}
 });
 test("both read paths reject live disk value corruption",[&]{for(int mode=0;mode<2;mode++){
  auto p=root+"/point-"+std::to_string(mode);Options o;if(mode)o.reads=ReadMode::Pread;Store db(p,true,o);db.commit({{"key","hello",ANY,false}});require(db.get("key").value=="hello","warm read");FD f(::open(p.c_str(),O_RDWR));unsigned char bad='X';write_at(f.n,&bad,1,START+64+17+3);expect_error([&]{db.get("key");});
 }});
 test("pread reports external truncation without returning unchecked bytes",[&]{Options o;o.reads=ReadMode::Pread;auto p=root+"/pread-trunc";Store db(p,true,o);db.commit({{"k",std::string(8192,'v'),ANY,false}});FD f(::open(p.c_str(),O_RDWR));require(!ftruncate(f.n,START),"truncate");expect_error([&]{db.get("k");});});
 test("single-barrier recovery: metadata ahead of lost or torn data gives way to the older slot; a damaged slot is recovered",[&]{
  auto p=root+"/reorder-source";Bytes before,after;uint64_t oldEnd;
  {Store db(p,true);db.commit({{"acknowledged","safe",ANY,false}});oldEnd=db.bytes();FD f(::open(p.c_str(),O_RDONLY));before=read_at(f.n,oldEnd,0);db.commit({{"pending","maybe",ANY,false}});after=read_at(f.n,db.bytes(),0);}
  for(int kind=0;kind<5;kind++){auto q=root+"/reorder-"+std::to_string(kind);Bytes bytes;
   if(kind==0){bytes=before;bytes.insert(bytes.end(),after.begin()+oldEnd,after.begin()+oldEnd+13);}
   if(kind==1){bytes=after;bytes.resize(oldEnd);}
   if(kind==2){bytes=after;std::fill(bytes.begin()+oldEnd,bytes.end(),0);}
   if(kind==3){bytes=after;bytes[3000]^=1;}
   if(kind==4)bytes=after;
   FD f(::open(q.c_str(),O_CREAT|O_EXCL|O_RDWR,0600));write_at(f.n,bytes.data(),bytes.size(),0);sync_file(f.n);
   // kinds 1 and 2: the newest slot reached the disk, its group did not (power loss before the
   // one flush returned: never acknowledged): the older slot is the start.
   // kind 3 damages the newest slot: the other slot and the intact chained group recover it.
   {Store db(q);require(db.get("acknowledged").value=="safe","acknowledged prefix lost");require(db.get("pending").found==(kind>=3),"wrong prefix");db.verify();}
  }
 });
 for(const auto&stage:{std::string("before_sync"),std::string("final_sync_error")})test("new process fault boundary "+stage,[&]{
  auto p=root+"/new-fault-"+stage;{Store db(p,true);db.commit({{"ack","safe",ANY,false}});}
  pid_t pid=fork();require(pid>=0,"fork");if(pid==0){setenv("KESTRO_FAULT",stage.c_str(),1);Store db(p);try{db.commit({{"pending","uncertain",ANY,false}});}catch(...){try{db.get("ack");_exit(90);}catch(...){_exit(76);}}_exit(91);}
  int status;waitpid(pid,&status,0);require(WIFEXITED(status)&&WEXITSTATUS(status)==(stage=="before_sync"?75:76),"fault not reached");
  // Process termination does not discard the kernel page cache: these complete
  // but unacknowledged bytes are visible. This is not a physical power-cut test.
  Store db(p);require(db.get("ack").value=="safe","ack lost");require(db.get("pending").value=="uncertain","complete unacked commit lost");db.verify();
 });
 test("eight writer threads preserve distinct transactions",[&]{
  auto p=root+"/writers";{Store db(p,true);std::atomic<bool>bad{false};std::vector<std::thread> threads;
   for(int t=0;t<8;t++)threads.emplace_back([&,t]{try{for(int i=0;i<40;i++)db.commit({{std::to_string(t)+":"+std::to_string(i),"ok",0,false}});}catch(...){bad=true;}});
   for(auto&t:threads)t.join();require(!bad&&db.count()==320&&db.txid()==320,"writer receipts");db.verify();}
  Store db(p);require(db.count()==320&&db.txid()==320,"writer reopen");
 });
 test("contending compare-and-swap has exactly one winner",[&]{Store db(root+"/cas-race",true);auto version=db.commit({{"k","initial",0,false}});std::atomic<int>wins{0};std::vector<std::thread> threads;
  for(int t=0;t<16;t++)threads.emplace_back([&]{try{db.commit({{"k","winner",version,false}});wins++;}catch(const std::exception&e){require(std::string(e.what()).find("CONFLICT")!=std::string::npos,"unexpected CAS error");}});
  for(auto&t:threads)t.join();require(wins==1&&db.txid()==2&&db.get("k").value=="winner","CAS not linearizable");db.verify();
 });
 test("cross-shard transactions do not expose partial publication",[&]{Store db(root+"/atomic-shards",true);db.commit({{"a","0",ANY,false},{"b","0",ANY,false}});std::atomic<bool>stop{false},bad{false};
  std::thread reader([&]{try{while(!stop){auto a=db.get("a");auto b=db.get("b");if(b.version<a.version)bad=true;}}catch(...){bad=true;}});
  for(int i=1;i<=300;i++)db.commit({{"a",std::to_string(i),ANY,false},{"b",std::to_string(i),ANY,false}});
  stop=true;reader.join();require(!bad,"partial transaction visible");db.verify();
 });
}

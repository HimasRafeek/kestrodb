#include "protocol.hpp"
#include <iostream>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
using namespace kestro;
using namespace kestro::protocol;

// Raw, unbuffered pipe I/O. A read returns whatever the client has already sent,
// so every request that queued up while the previous batch was being flushed to
// disk is visible at once.
static size_t raw_read(void*p,size_t n){
#ifdef _WIN32
 int got=_read(0,p,unsigned(std::min<size_t>(n,1u<<30)));if(got<0)throw std::runtime_error("IO stdin read");return size_t(got);
#else
 for(;;){auto got=::read(0,p,n);if(got>=0)return size_t(got);if(errno!=EINTR)throw std::runtime_error("IO stdin read");}
#endif
}
static void raw_write_all(const unsigned char*p,size_t n){
 while(n){
#ifdef _WIN32
  int put=_write(1,p,unsigned(std::min<size_t>(n,1u<<30)));if(put<=0)throw std::runtime_error("IO stdout write");
#else
  auto put=::write(1,p,n);if(put<0&&errno==EINTR)continue;if(put<=0)throw std::runtime_error("IO stdout write");
#endif
  p+=put;n-=size_t(put);
 }
}

struct Replies {
 Bytes buffer;
 void add(const Bytes&b){unsigned char head[4];put32(head,uint32_t(b.size()));buffer.insert(buffer.end(),head,head+4);buffer.insert(buffer.end(),b.begin(),b.end());if(buffer.size()>=4*1024*1024)flush();}
 void flush(){if(!buffer.empty()){raw_write_all(buffer.data(),buffer.size());buffer.clear();}}
};
static Bytes error_reply(const std::exception&e){Bytes out(1,255);std::string msg=e.what();out.insert(out.end(),msg.begin(),msg.end());return out;}
static Bytes error_reply(std::exception_ptr p){try{std::rethrow_exception(p);}catch(const std::exception&e){return error_reply(e);}catch(...){return error_reply(std::runtime_error("UNKNOWN error"));}}

// Handles every request except B (batched separately) and Q.
static Bytes handle(Store&db,const Options&options,const unsigned char*b,size_t n){
 Bytes out(1,0);char cmd=char(b[0]);std::string argument;
 if(cmd=='G'||cmd=='P'||cmd=='C')argument.assign(reinterpret_cast<const char*>(b+1),n-1);
 if(cmd=='G'){
  auto v=db.get(argument);if(!v.found)out[0]=1;else{out.resize(9+v.value.size());put64(out.data()+1,v.version);memcpy(out.data()+9,v.value.data(),v.value.size());}
 }else if(cmd=='M'){
  return encode_values(db.getMany(parse_keys(b,n)));
 }else if(cmd=='V'){db.verify();}
 else if(cmd=='P'||cmd=='C'){require(argument.find('\0')==std::string::npos,"INPUT path contains NUL");db.snapshot(argument,cmd=='C');}
 else if(cmd=='S'){auto s=std::string("{\"keys\":")+std::to_string(db.count())+",\"bytes\":"+std::to_string(db.bytes())+",\"txid\":"+std::to_string(db.txid())+",\"readMode\":\""+(options.reads==ReadMode::Mapped?"mapped":"pread")+"\",\"commitPolicy\":\""+(options.orderedSync?"ordered":"single-barrier")+"\",\"groupCommit\":true,\"openedFrom\":\""+std::string(db.opened_from())+"\",\"verification\":\""+db.verification()+"\",\"indexMode\":\""+std::string(db.index_mode())+"\",\"pointChecksum\":\"keyed SipHash-2-4-128 (<=2 KiB) or keyed BLAKE3\",\"platform\":\""+platform::name+"\",\"fileSync\":\""+platform::sync_name+"\",\"namespaceSync\":\""+platform::namespace_sync+"\",\"sha256Backend\":\""+hashing::backend+"\"}";out.insert(out.end(),s.begin(),s.end());}
 else throw std::runtime_error("INPUT unknown command");
 return out;
}

int worker_main(const std::vector<std::string>&args){
 int argc=int(args.size());
 try{
  platform::binary_stdio();
  require(argc>=2,"usage: kestro-worker FILE [--create] [--pread|--mapped] [--ordered-sync] [--verify-on-open]");
  bool create=false;Options options;
  for(int i=2;i<argc;i++){std::string arg=args[i];if(arg=="--create")create=true;else if(arg=="--pread")options.reads=ReadMode::Pread;else if(arg=="--mapped")options.reads=ReadMode::Mapped;else if(arg=="--ordered-sync")options.orderedSync=true;else if(arg=="--verify-on-open")options.verifyOnOpen=true;else throw std::runtime_error("INPUT unknown worker option");}
  Store db(args[1],create,options);
  Bytes in(1024*1024);size_t start=0,end=0;Replies replies;
  struct Frame{const unsigned char*data;size_t size;};
  for(;;){
   // Collect every complete request already received.
   std::vector<Frame> frames;size_t pos=start;
   while(end-pos>=4){auto n=get32(in.data()+pos);require(n>0&&n<=MAX_GROUP,"INPUT request length");if(end-pos-4<n)break;frames.push_back({in.data()+pos+4,n});pos+=4+n;}
   // Process them in order. A run of consecutive batches becomes one group commit,
   // so later requests still observe every earlier write.
   for(size_t i=0;i<frames.size();){
    char cmd=char(frames[i].data[0]);
    if(cmd=='B'){
     size_t j=i;while(j<frames.size()&&frames[j].data[0]=='B'&&j-i<4096)j++;
     std::vector<std::vector<Op>> parsed(j-i);std::vector<Bytes> early(j-i);std::vector<const std::vector<Op>*> group;std::vector<size_t> slot;
     for(size_t k=i;k<j;k++){try{parsed[k-i]=parse_batch(frames[k].data,frames[k].size);group.push_back(&parsed[k-i]);slot.push_back(k-i);}catch(const std::exception&e){early[k-i]=error_reply(e);}}
     if(!group.empty()){
      std::vector<Store::Outcome> results;
      try{results=db.commit_group(group);}catch(const std::exception&e){auto r=error_reply(e);for(auto s:slot)early[s]=r;}
      for(size_t g=0;g<results.size();g++){auto s=slot[g];if(results[g].error)early[s]=error_reply(results[g].error);else{Bytes ok(9,0);put64(ok.data()+1,results[g].version);early[s]=std::move(ok);}}
     }
     for(auto&r:early)replies.add(r);
     i=j;continue;
    }
    if(cmd=='Q'){replies.add(Bytes(1,0));replies.flush();return 0;}
    try{replies.add(handle(db,options,frames[i].data,frames[i].size));}catch(const std::exception&e){replies.add(error_reply(e));}
    i++;
   }
   start=pos;
   // Nothing complete is left: send every reply with one write, then wait for input.
   replies.flush();
   if(start==end){start=end=0;}
   else if(start>0){memmove(in.data(),in.data()+start,end-start);end-=start;start=0;}
   if(end>=4){auto need=size_t(4)+get32(in.data());if(need>in.size())in.resize(need);}
   if(end==in.size())in.resize(in.size()*2);
   auto got=raw_read(in.data()+end,in.size()-end);
   if(!got){require(end==0,"INPUT truncated request");break;}
   end+=got;
  }
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
 return 0;
}

#ifdef _WIN32
int wmain(int argc,wchar_t**argv){try{std::vector<std::string>args;for(int i=0;i<argc;i++)args.push_back(platform::utf8(argv[i]));return worker_main(args);}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
#else
int main(int argc,char**argv){return worker_main(std::vector<std::string>(argv,argv+argc));}
#endif

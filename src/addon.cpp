// In-process Node-API binding: the same Store as the worker, without the pipe.
// Reads and writes are synchronous, like node:sqlite's DatabaseSync. batchAsync
// hands writes to a background writer thread that group-commits everything queued
// under one flush, and resolves each promise after that flush. Wire encodings for
// batches and multi-key reads are shared with the worker (protocol.hpp).
#define NAPI_VERSION 8
#include "protocol.hpp"
#include <node_api.h>
#include <condition_variable>
#include <memory>
#include <thread>
using namespace kestro;

namespace {
// Background group commit. Owned by its thread-safe function: it is deleted in the
// function's finalizer, so a completion call already queued can never outlive it.
struct Async {
 // One request = every transaction the JS side queued in one tick, settled together
 // as an array: the committed version for each transaction, or its error message.
 struct Pending {std::vector<std::vector<Op>> txs;size_t bytes=0;napi_deferred deferred=nullptr;};
 struct Done {napi_deferred deferred=nullptr;std::vector<uint64_t> versions;std::vector<std::string> errors;};
 Store*store=nullptr;
 std::mutex m;std::condition_variable cv;std::vector<Pending> queue;std::vector<Done> done;bool stop=false;
 std::thread thread;
 napi_threadsafe_function tsfn=nullptr;
 // JS-thread state: unresolved promises, and a strong reference to the database
 // handle while any exist, so garbage collection cannot finalize it mid-write.
 size_t inflight=0;napi_ref keep_alive=nullptr;
 // The owning Db's pointer to this object. Node may finalize the thread-safe function
 // first at environment teardown; the finalizer then clears this pointer.
 Async**owner=nullptr;
 void stop_thread(){{std::lock_guard<std::mutex> lock(m);stop=true;}cv.notify_one();if(thread.joinable())thread.join();}
 void run(){
  for(;;){
   std::vector<Pending> work;
   {std::unique_lock<std::mutex> lock(m);cv.wait(lock,[&]{return stop||!queue.empty();});if(queue.empty())return;work.swap(queue);}
   // One flush per chunk of at most 64 MiB of requests.
   for(size_t first=0;first<work.size();){
    size_t last=first,bytes=0;while(last<work.size()&&(last==first||bytes+work[last].bytes<=MAX_GROUP)){bytes+=work[last].bytes;last++;}
    std::vector<const std::vector<Op>*> group;std::vector<Done> finished(last-first);
    for(size_t i=first;i<last;i++){auto&d=finished[i-first];d.deferred=work[i].deferred;d.versions.assign(work[i].txs.size(),0);d.errors.assign(work[i].txs.size(),{});for(auto&tx:work[i].txs)group.push_back(&tx);}
    try{auto results=store->commit_group(group);size_t g=0;
     for(auto&d:finished)for(size_t t=0;t<d.versions.size();t++,g++){d.versions[t]=results[g].version;if(results[g].error){try{std::rethrow_exception(results[g].error);}catch(const std::exception&e){d.errors[t]=e.what();}catch(...){d.errors[t]="UNKNOWN error";}}}
    }catch(const std::exception&e){for(auto&d:finished)for(auto&err:d.errors)err=e.what();}
    {std::lock_guard<std::mutex> lock(m);for(auto&f:finished)done.push_back(std::move(f));}
    napi_call_threadsafe_function(tsfn,nullptr,napi_tsfn_blocking);
    first=last;
   }
  }
 }
 // JS thread: settle every finished promise.
 void drain(napi_env env){
  std::vector<Done> ready;{std::lock_guard<std::mutex> lock(m);ready.swap(done);}
  for(auto&d:ready){
   napi_value arr;napi_create_array_with_length(env,d.versions.size(),&arr);
   for(size_t t=0;t<d.versions.size();t++){napi_value v;
    if(d.errors[t].empty())napi_create_double(env,double(d.versions[t]),&v);else napi_create_string_utf8(env,d.errors[t].data(),d.errors[t].size(),&v);
    napi_set_element(env,arr,uint32_t(t),v);}
   napi_resolve_deferred(env,d.deferred,arr);
  }
  if(ready.empty())return;
  inflight-=ready.size();
  if(!inflight){napi_unref_threadsafe_function(env,tsfn);if(keep_alive){napi_delete_reference(env,keep_alive);keep_alive=nullptr;}}
 }
 // JS thread: commit what is queued, settle it, and release the thread-safe function.
 void shutdown(napi_env env){
  owner=nullptr;stop_thread();
  drain(env);
  napi_release_threadsafe_function(tsfn,napi_tsfn_release);
 }
};
void async_complete(napi_env env,napi_value,void*context,void*){if(env)static_cast<Async*>(context)->drain(env);}
// Also runs at environment teardown without close(): commit what is queued, join
// the writer (a joinable std::thread must never be destroyed), detach from the Db.
void async_finalize(napi_env,void*data,void*){auto*a=static_cast<Async*>(data);a->stop_thread();if(a->owner)*a->owner=nullptr;delete a;}

// LISTEN / NOTIFY (store_notify.inc): the Store queues notifications here from whichever
// thread commits (under its writer lock, so in commit order); a thread-safe function hands
// them to the JS dispatcher on the JS thread, never inside a commit. Shared by the Db and the
// thread-safe function (whose finalizer drops its share), so a queued call never outlives it.
// The queue holds at most 64 MB; notifications past that are counted as lost and reported.
struct Notify: Store::Listener {
 std::mutex m;std::vector<Store::Note> queue;size_t bytes=0;uint64_t lost=0;bool signaled=false;
 napi_threadsafe_function tsfn=nullptr;  // under m; created at the first LISTEN
 bool refd=false;                        // JS thread: the function keeps the loop alive while channels are listened to
 static constexpr size_t LIMIT=size_t(64)<<20;
 void deliver(const Store::Note*const*notes,size_t n)override{
  std::lock_guard<std::mutex>g(m);
  for(size_t i=0;i<n;i++){size_t b=notes[i]->channel.size()+notes[i]->payload.size()+64;if(bytes+b>LIMIT){lost++;continue;}bytes+=b;queue.push_back(*notes[i]);}
  if(!signaled&&tsfn&&(lost||!queue.empty())){signaled=true;napi_call_threadsafe_function(tsfn,nullptr,napi_tsfn_nonblocking);}
 }
 // JS thread: dispatch(notes: {channel, payload, txid}[], lost: number).
 void drain(napi_env env,napi_value fn){
  std::vector<Store::Note> ready;uint64_t gone=0;{std::lock_guard<std::mutex>g(m);ready.swap(queue);bytes=0;gone=lost;lost=0;signaled=false;}
  if(ready.empty()&&!gone)return;
  napi_value arr,args[2];if(napi_create_array_with_length(env,ready.size(),&arr)!=napi_ok)return;
  for(size_t i=0;i<ready.size();i++){napi_value o,v;napi_create_object(env,&o);
   napi_create_string_utf8(env,ready[i].channel.data(),ready[i].channel.size(),&v);napi_set_named_property(env,o,"channel",v);
   napi_create_string_utf8(env,ready[i].payload.data(),ready[i].payload.size(),&v);napi_set_named_property(env,o,"payload",v);
   napi_create_double(env,double(ready[i].txid),&v);napi_set_named_property(env,o,"txid",v);
   napi_set_element(env,arr,uint32_t(i),o);}
  args[0]=arr;napi_create_double(env,double(gone),&args[1]);napi_value self;napi_get_undefined(env,&self);
  napi_call_function(env,self,fn,2,args,nullptr);  // the dispatcher reports its callbacks' errors itself
 }
};
void notify_call(napi_env env,napi_value fn,void*context,void*){if(env&&fn)(*static_cast<std::shared_ptr<Notify>*>(context))->drain(env,fn);}
void notify_finalize(napi_env,void*data,void*){auto*p=static_cast<std::shared_ptr<Notify>*>(data);{std::lock_guard<std::mutex>g((*p)->m);(*p)->tsfn=nullptr;}delete p;}

struct Db {
 std::unique_ptr<Store> store;
 double*version_out=nullptr;  // Float64Array(1) supplied at open; receives get() versions
 napi_ref out_ref=nullptr;
 Options options;
 Async*async=nullptr;
 Store::SqlSession session;  // BEGIN ... COMMIT state of this Database object
 std::shared_ptr<Notify> notify=std::make_shared<Notify>();napi_ref dispatch=nullptr;  // LISTEN: notify->tsfn calls dispatch
 static void ok(napi_status s){if(s!=napi_ok)throw std::runtime_error("UNKNOWN Node-API call failed");}
 // After LISTEN/UNLISTEN (JS thread): the thread-safe function exists once a channel is
 // listened to, and keeps the event loop alive only while one is.
 void listening(napi_env env){
  bool any=!notify->channels.empty();
  napi_threadsafe_function f;{std::lock_guard<std::mutex>g(notify->m);f=notify->tsfn;}
  if(any&&!f){
   napi_value fn,name;ok(napi_get_reference_value(env,dispatch,&fn));ok(napi_create_string_utf8(env,"kestro-notify",NAPI_AUTO_LENGTH,&name));
   auto*ctx=new std::shared_ptr<Notify>(notify);
   if(napi_create_threadsafe_function(env,fn,nullptr,name,0,1,ctx,notify_finalize,ctx,notify_call,&f)!=napi_ok){delete ctx;throw std::runtime_error("UNKNOWN cannot create the notification function");}
   notify->refd=true;
   std::lock_guard<std::mutex>g(notify->m);notify->tsfn=f;if(!notify->signaled&&(notify->lost||!notify->queue.empty())){notify->signaled=true;napi_call_threadsafe_function(f,nullptr,napi_tsfn_nonblocking);}
   return;
  }
  if(f&&any!=notify->refd){notify->refd=any;if(any)napi_ref_threadsafe_function(env,f);else napi_unref_threadsafe_function(env,f);}
 }
 // An open transaction is rolled back first: queued async writes wait for the write gate.
 void close(napi_env env){
  if(store)store->end_session(session);
  if(async){async->shutdown(env);async=nullptr;}
  if(store)store->remove_listener(*notify);  // after the last commit: no delivery after this
  napi_threadsafe_function f;{std::lock_guard<std::mutex>g(notify->m);f=notify->tsfn;notify->tsfn=nullptr;}
  if(f)napi_release_threadsafe_function(f,napi_tsfn_release);  // notifications already queued are still dispatched
  store.reset();
 }
};

struct Fail: std::runtime_error {using std::runtime_error::runtime_error;};
void check(napi_env env,napi_status s){if(s!=napi_ok){const napi_extended_error_info*info=nullptr;napi_get_last_error_info(env,&info);throw Fail(info&&info->error_message?info->error_message:"Node-API call failed");}}

template<class F> napi_value guard(napi_env env,F&&f){
 try{return f();}
 catch(const std::exception&e){bool pending=false;napi_is_exception_pending(env,&pending);if(!pending){std::string m=e.what();auto sp=m.find(' ');std::string code=sp==std::string::npos?m:m.substr(0,sp);bool upper=!code.empty();for(char c:code)upper&=(c>='A'&&c<='Z')||c=='_';napi_throw_error(env,upper?code.c_str():nullptr,m.c_str());}}
 catch(...){napi_throw_error(env,nullptr,"UNKNOWN native error");}
 return nullptr;
}
template<size_t N> std::array<napi_value,N> args(napi_env env,napi_callback_info info){std::array<napi_value,N> a{};size_t n=N;check(env,napi_get_cb_info(env,info,&n,a.data(),nullptr,nullptr));require(n==N,"INPUT argument count");return a;}
Db&handle(napi_env env,napi_value v){void*p=nullptr;check(env,napi_get_value_external(env,v,&p));auto*db=static_cast<Db*>(p);require(db&&db->store,"CLOSED");return *db;}
std::string text(napi_env env,napi_value v){size_t n=0;check(env,napi_get_value_string_utf8(env,v,nullptr,0,&n));std::string s(n,'\0');if(n)check(env,napi_get_value_string_utf8(env,v,s.data(),n+1,&n));return s;}
// Keys are short; read them through a stack buffer to avoid a length probe.
size_t small_text(napi_env env,napi_value v,char(&buf)[2048]){size_t n=0;check(env,napi_get_value_string_utf8(env,v,buf,sizeof buf,&n));require(n<sizeof buf-1,"LIMIT key");return n;}
std::pair<const unsigned char*,size_t> bytes(napi_env env,napi_value v){void*p=nullptr;size_t n=0;check(env,napi_get_buffer_info(env,v,&p,&n));return {static_cast<const unsigned char*>(p),n};}
napi_value number(napi_env env,double d){napi_value r;check(env,napi_create_double(env,d,&r));return r;}
napi_value undefined(napi_env env){napi_value r;check(env,napi_get_undefined(env,&r));return r;}
// Runs only with no write in flight (Async::keep_alive), so shutdown has nothing to settle.
void finalize(napi_env env,void*data,void*){auto*db=static_cast<Db*>(data);try{db->close(env);}catch(...){}if(db->out_ref)napi_delete_reference(env,db->out_ref);if(db->dispatch)napi_delete_reference(env,db->dispatch);delete db;}

// open(path, create, readMode 0=auto 1=mapped 2=pread, orderedSync, Float64Array(1), verifyOnOpen, indexMemoryBytes?, orderedMemoryBytes?, dispatch) -> handle
// dispatch(notes, lost) receives LISTEN notifications (see Notify).
napi_value Open(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<9>(env,info);auto path=text(env,a[0]);bool create=false,ordered=false;int32_t mode=0;
 check(env,napi_get_value_bool(env,a[1],&create));check(env,napi_get_value_int32(env,a[2],&mode));check(env,napi_get_value_bool(env,a[3],&ordered));
 napi_typedarray_type type;size_t length=0;void*data=nullptr;napi_value ab;size_t offset=0;
 check(env,napi_get_typedarray_info(env,a[4],&type,&length,&data,&ab,&offset));require(type==napi_float64_array&&length>=1,"INPUT version buffer");
 require(!path.empty()&&path.find('\0')==std::string::npos,"INPUT path");
 auto db=std::make_unique<Db>();
 if(mode==1)db->options.reads=ReadMode::Mapped;else if(mode==2)db->options.reads=ReadMode::Pread;else require(mode==0,"INPUT readMode");
 db->options.orderedSync=ordered;{bool v=false;check(env,napi_get_value_bool(env,a[5],&v));db->options.verifyOnOpen=v;}
 {napi_valuetype t;check(env,napi_typeof(env,a[6],&t));if(t==napi_number){double m=0;check(env,napi_get_value_double(env,a[6],&m));require(m>=0&&m<9e15,"INPUT indexMemory");db->options.indexMemory=size_t(m);}}
 {napi_valuetype t;check(env,napi_typeof(env,a[7],&t));if(t==napi_number){double m=0;check(env,napi_get_value_double(env,a[7],&m));require(m>=0&&m<9e15,"INPUT orderedMemory");db->options.orderedMemory=size_t(m);}}
 db->store=std::make_unique<Store>(path,create,db->options);
 db->version_out=static_cast<double*>(data);
 check(env,napi_create_reference(env,a[4],1,&db->out_ref));
 {napi_valuetype t;check(env,napi_typeof(env,a[8],&t));require(t==napi_function,"INPUT dispatch");check(env,napi_create_reference(env,a[8],1,&db->dispatch));}
 db->session.listener=db->notify.get();
 napi_value external;check(env,napi_create_external(env,db.get(),finalize,nullptr,&external));db.release();return external;
});}
// Waits for queued async writes to commit and settles their promises before closing.
napi_value Close(napi_env env,napi_callback_info info){return guard(env,[&]{auto a=args<1>(env,info);void*p=nullptr;check(env,napi_get_value_external(env,a[0],&p));if(auto*db=static_cast<Db*>(p))db->close(env);return undefined(env);});}

// get(handle, key) -> JSON text or null; the version goes to the Float64Array.
// A value as a JS string. JSON is nearly always ASCII: then a one-byte (Latin-1) string,
// which V8 builds with a copy instead of decoding UTF-8 (the same text for ASCII bytes).
static napi_value make_text(napi_env env,const char*p,size_t n){
 bool ascii=true;size_t i=0;
 for(;i+8<=n;i+=8){uint64_t w;memcpy(&w,p+i,8);if(w&0x8080808080808080ULL){ascii=false;break;}}
 if(ascii)for(;i<n;i++)if((unsigned char)p[i]&0x80){ascii=false;break;}
 napi_value r;check(env,ascii?napi_create_string_latin1(env,p,n,&r):napi_create_string_utf8(env,p,n,&r));return r;
}
napi_value Get(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<2>(env,info);auto&db=handle(env,a[0]);char buf[2048];auto n=small_text(env,a[1],buf);
 thread_local std::string value;uint64_t version=0;napi_value r;
 if(!db.store->read(std::string_view(buf,n),value,version)){check(env,napi_get_null(env,&r));return r;}
 db.version_out[0]=double(version);return make_text(env,value.data(),value.size());
});}
// getMany(handle, request Buffer in worker 'M' layout) -> response Buffer in worker layout
napi_value GetMany(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<2>(env,info);auto&db=handle(env,a[0]);auto [p,n]=bytes(env,a[1]);require(n>=4,"INPUT getMany");
 auto out=protocol::encode_values(db.store->getMany(protocol::parse_keys(p+4,n-4)));
 napi_value r;check(env,napi_create_buffer_copy(env,out.size(),out.data(),nullptr,&r));return r;
});}
// getManyArray(handle, keys: string[], versions: Float64Array) -> (string|null)[]
// One consistent snapshot, like getMany, without the encode/decode round trip.
// getEach(handle, keys: string[], versions: Float64Array) -> (string|null)[]
// One call, one point read per key. Not one locked snapshot on its own: index.mjs uses it
// only when no async write is outstanding, so nothing can commit between the reads.
napi_value GetEach(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<3>(env,info);auto&db=handle(env,a[0]);uint32_t n=0;check(env,napi_get_array_length(env,a[1],&n));require(n<=4096,"LIMIT getMany count");
 napi_typedarray_type type;size_t length=0;void*data=nullptr;napi_value ab;size_t offset=0;
 check(env,napi_get_typedarray_info(env,a[2],&type,&length,&data,&ab,&offset));require(type==napi_float64_array&&length>=n,"INPUT version buffer");
 auto*versions=static_cast<double*>(data);thread_local std::string value;char buf[2048];
 napi_value out;check(env,napi_create_array_with_length(env,n,&out));
 for(uint32_t i=0;i<n;i++){
  napi_value k,v;check(env,napi_get_element(env,a[1],i,&k));auto len=small_text(env,k,buf);require(len>0&&len<=1024,"LIMIT key");
  uint64_t version=0;
  if(db.store->read(std::string_view(buf,len),value,version)){versions[i]=double(version);v=make_text(env,value.data(),value.size());}
  else check(env,napi_get_null(env,&v));
  check(env,napi_set_element(env,out,i,v));
 }
 return out;
});}
napi_value GetManyArray(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<3>(env,info);auto&db=handle(env,a[0]);uint32_t n=0;check(env,napi_get_array_length(env,a[1],&n));require(n<=4096,"LIMIT getMany count");
 napi_typedarray_type type;size_t length=0;void*data=nullptr;napi_value ab;size_t offset=0;
 check(env,napi_get_typedarray_info(env,a[2],&type,&length,&data,&ab,&offset));require(type==napi_float64_array&&length>=n,"INPUT version buffer");
 std::vector<std::string> keys(n);char buf[2048];
 for(uint32_t i=0;i<n;i++){napi_value k;check(env,napi_get_element(env,a[1],i,&k));auto len=small_text(env,k,buf);require(len>0&&len<=1024,"LIMIT key");keys[i].assign(buf,len);}
 auto values=db.store->getMany(keys);auto*versions=static_cast<double*>(data);
 napi_value out;check(env,napi_create_array_with_length(env,n,&out));
 for(uint32_t i=0;i<n;i++){napi_value v;
  if(values[i].found){versions[i]=double(values[i].version);v=make_text(env,values[i].value.data(),values[i].value.size());}
  else check(env,napi_get_null(env,&v));
  check(env,napi_set_element(env,out,i,v));}
 return out;
});}
// batch(handle, request Buffer in worker 'B' layout) -> committed version
napi_value Batch(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<2>(env,info);auto&db=handle(env,a[0]);auto [p,n]=bytes(env,a[1]);require(n>=4,"INPUT batch");
 auto ops=protocol::parse_batch(p+4,n-4);return number(env,double(db.store->commit(ops)));
});}
// batchAsync(handle, Buffer[] in worker 'B' layout) -> Promise<(number|string)[]>
// Every transaction one JS tick queued, committed together. Each array entry is that
// transaction's version, or its error message. Parsing happens here, so malformed
// input throws synchronously.
napi_value BatchAsync(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<2>(env,info);auto&db=handle(env,a[0]);uint32_t count=0;check(env,napi_get_array_length(env,a[1],&count));require(count>0,"INPUT batch count");
 Async::Pending item;item.txs.reserve(count);
 for(uint32_t i=0;i<count;i++){napi_value b;check(env,napi_get_element(env,a[1],i,&b));auto [p,n]=bytes(env,b);require(n>=4,"INPUT batch");item.txs.push_back(protocol::parse_batch(p+4,n-4));item.bytes+=n;}
 if(!db.async){
  auto state=std::make_unique<Async>();state->store=db.store.get();
  napi_value name;check(env,napi_create_string_utf8(env,"kestro-group-commit",NAPI_AUTO_LENGTH,&name));
  check(env,napi_create_threadsafe_function(env,nullptr,nullptr,name,0,1,state.get(),async_finalize,state.get(),async_complete,&state->tsfn));
  check(env,napi_unref_threadsafe_function(env,state->tsfn));
  auto*raw=state.release();db.async=raw;raw->owner=&db.async;  // now owned by the thread-safe function
  raw->thread=std::thread([raw]{raw->run();});
 }
 auto&as=*db.async;
 napi_value promise;check(env,napi_create_promise(env,&item.deferred,&promise));
 if(!as.inflight++){check(env,napi_ref_threadsafe_function(env,as.tsfn));check(env,napi_create_reference(env,a[0],1,&as.keep_alive));}
 {std::lock_guard<std::mutex> lock(as.m);as.queue.push_back(std::move(item));}as.cv.notify_one();
 return promise;
});}
napi_value Stats(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<1>(env,info);auto&db=handle(env,a[0]);auto&s=*db.store;
 auto json=std::string("{\"keys\":")+std::to_string(s.count())+",\"indexMemory\":"+std::to_string(s.index_memory())+",\"indexFileBytes\":"+std::to_string(s.index_file_bytes())+",\"orderedMemory\":"+std::to_string(s.ordered_memory())+",\"orderedFileBytes\":"+std::to_string(s.ordered_file_bytes())+",\"bytes\":"+std::to_string(s.bytes())+",\"txid\":"+std::to_string(s.txid())+",\"flushes\":"+std::to_string(s.flushes())+",\"readMode\":\""+(db.options.reads==ReadMode::Mapped?"mapped":"pread")+"\",\"commitPolicy\":\""+(db.options.orderedSync?"ordered":"single-barrier")+"\",\"inProcess\":true,\"openedFrom\":\""+std::string(s.opened_from())+"\",\"verification\":\""+s.verification()+"\",\"indexMode\":\""+std::string(s.index_mode())+"\",\"pointChecksum\":\"keyed SipHash-2-4-128 (<=2 KiB) or keyed BLAKE3\",\"platform\":\""+platform::name+"\",\"fileSync\":\""+platform::sync_name+"\",\"namespaceSync\":\""+platform::namespace_sync+"\",\"sha256Backend\":\""+hashing::backend+"\"}";
 napi_value r;check(env,napi_create_string_utf8(env,json.data(),json.size(),&r));return r;
});}
napi_value Verify(napi_env env,napi_callback_info info){return guard(env,[&]{auto a=args<1>(env,info);handle(env,a[0]).store->verify();return undefined(env);});}
// sql(handle, text, paramsJson) -> result JSON text
napi_value Sql(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<3>(env,info);auto&db=handle(env,a[0]);auto out=db.store->sql(&db.session,text(env,a[1]),text(env,a[2]));
 if(db.notify->refd||!db.notify->channels.empty())db.listening(env);  // after SQL LISTEN / UNLISTEN
 napi_value r;check(env,napi_create_string_utf8(env,out.data(),out.size(),&r));return r;
});}
// listen(handle, channel) / unlisten(handle, channel | null: every channel)
napi_value Listen(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<2>(env,info);auto&db=handle(env,a[0]);db.store->listen(*db.notify,text(env,a[1]));db.listening(env);return undefined(env);
});}
napi_value Unlisten(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<2>(env,info);auto&db=handle(env,a[0]);napi_valuetype t;check(env,napi_typeof(env,a[1],&t));
 if(t==napi_string)db.store->unlisten(*db.notify,text(env,a[1]));else db.store->unlisten(*db.notify,{},true);
 db.listening(env);return undefined(env);
});}
// snapshot(handle, path, compact)
napi_value Snapshot(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<3>(env,info);auto&db=handle(env,a[0]);auto path=text(env,a[1]);bool compact=false;check(env,napi_get_value_bool(env,a[2],&compact));
 require(!path.empty()&&path.find('\0')==std::string::npos,"INPUT path");db.store->snapshot(path,compact);return undefined(env);
});}
}

// compact(handle) -> JSON {before, after, keys}
napi_value Compact(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<1>(env,info);auto&db=handle(env,a[0]);auto r=db.store->compact();auto s=std::string("{\"before\":")+std::to_string(r.before)+",\"after\":"+std::to_string(r.after)+",\"keys\":"+std::to_string(r.keys)+"}";
 napi_value v;check(env,napi_create_string_utf8(env,s.data(),s.size(),&v));return v;
});}
// archive(handle, dir) -> [txid, end]
napi_value Archive(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<2>(env,info);auto&db=handle(env,a[0]);auto dir=text(env,a[1]);
 require(!dir.empty()&&dir.find('\0')==std::string::npos,"INPUT directory");auto p=db.store->archive(dir);
 auto s=std::to_string(p.txid)+" "+std::to_string(p.end);napi_value r;check(env,napi_create_string_utf8(env,s.data(),s.size(),&r));return r;
});}
// restore(source, dest, txid, atMs) -> "txid end" (txid 0: everything; atMs < 0: unused)
napi_value Restore(napi_env env,napi_callback_info info){return guard(env,[&]{
 auto a=args<4>(env,info);auto src=text(env,a[0]),dest=text(env,a[1]);double tx=0,at=-1;
 check(env,napi_get_value_double(env,a[2],&tx));check(env,napi_get_value_double(env,a[3],&at));
 require(!src.empty()&&!dest.empty()&&src.find('\0')==std::string::npos&&dest.find('\0')==std::string::npos,"INPUT path");
 require(tx>=0&&tx<9e15,"INPUT txid");
 auto p=Store::restore(src,dest,uint64_t(tx),at<0?-1:int64_t(at));
 auto s=std::to_string(p.txid)+" "+std::to_string(p.end);napi_value r;check(env,napi_create_string_utf8(env,s.data(),s.size(),&r));return r;
});}
NAPI_MODULE_INIT(){
 const napi_property_descriptor methods[]={
  {"open",nullptr,Open,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"close",nullptr,Close,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"get",nullptr,Get,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"getMany",nullptr,GetMany,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"getManyArray",nullptr,GetManyArray,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"getEach",nullptr,GetEach,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"batch",nullptr,Batch,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"batchAsync",nullptr,BatchAsync,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"stats",nullptr,Stats,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"verify",nullptr,Verify,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"snapshot",nullptr,Snapshot,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"archive",nullptr,Archive,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"compact",nullptr,Compact,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"restore",nullptr,Restore,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"sql",nullptr,Sql,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"listen",nullptr,Listen,nullptr,nullptr,nullptr,napi_default,nullptr},
  {"unlisten",nullptr,Unlisten,nullptr,nullptr,nullptr,napi_default,nullptr},
 };
 if(napi_define_properties(env,exports,sizeof methods/sizeof methods[0],methods)!=napi_ok)return nullptr;
 return exports;
}

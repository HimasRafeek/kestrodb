// Included inside class Store (kestro.hpp): change notifications, as PostgreSQL's LISTEN /
// NOTIFY. A Listener (an in-process Database, a server connection) names its channels.
// NOTIFY channel, 'payload' reaches every listener of the channel once its transaction
// commits (outside a transaction: at once), in commit order; a transaction sends each
// distinct channel + payload once, and none if it rolls back. Listeners of the channel
// "table_changes:<collection>" get {"op":"insert"|"update"|"delete","table","key"} for every
// key (or SQL row, keyed by its id) a commit changes there. Delivery runs on the committing
// thread, under the writer lock, so listeners see commits in order; a listener only queues
// (bounded) and wakes its consumer. With no listener a commit pays one relaxed atomic load.
// Only commits of this Store are seen: not another process writing the same file.
public:
 struct Note {std::string channel,payload;uint64_t txid=0;};
 struct Listener {
  virtual ~Listener()=default;
  // Called under the writer lock, in commit order, from any committing thread: queue and return.
  virtual void deliver(const Note*const*notes,size_t n)=0;
  std::set<std::string,std::less<>> channels;  // changed only through the Store (nt_m_)
 };
 static constexpr size_t MAX_NOTIFY_PAYLOAD=8000,MAX_CHANNEL=255,MAX_TRANSACTION_NOTES=65536;
 static constexpr std::string_view CHANGES_PREFIX="table_changes:";
 static bool changes_channel(std::string_view c){return c.size()>CHANGES_PREFIX.size()&&c.compare(0,CHANGES_PREFIX.size(),CHANGES_PREFIX)==0;}
 static void check_channel(std::string_view c){require(!c.empty()&&c.size()<=MAX_CHANNEL&&c.find('\0')==std::string_view::npos,"INPUT a channel name is 1 to 255 bytes, without NUL");}
 void listen(Listener&l,const std::string&channel){
  check_channel(channel);std::lock_guard<std::mutex>g(nt_m_);
  if(std::find(nt_listeners_.begin(),nt_listeners_.end(),&l)==nt_listeners_.end())nt_listeners_.push_back(&l);
  if(!l.channels.insert(channel).second)return;
  nt_channels_[channel]++;nt_count_++;if(changes_channel(channel))nt_watch_++;
 }
 // all: every channel of l (UNLISTEN *).
 void unlisten(Listener&l,std::string_view channel,bool all=false){
  std::lock_guard<std::mutex>g(nt_m_);
  for(auto it=l.channels.begin();it!=l.channels.end();){
   if(!all&&*it!=channel){++it;continue;}
   auto c=nt_channels_.find(*it);if(c!=nt_channels_.end()&&!--c->second)nt_channels_.erase(c);
   nt_count_--;if(changes_channel(*it))nt_watch_--;it=l.channels.erase(it);
  }
 }
 // Before l goes away: no delivery reaches it after this returns.
 void remove_listener(Listener&l){unlisten(l,{},true);std::lock_guard<std::mutex>g(nt_m_);nt_listeners_.erase(std::remove(nt_listeners_.begin(),nt_listeners_.end(),&l),nt_listeners_.end());}
private:
 std::mutex nt_m_;std::vector<Listener*> nt_listeners_;std::unordered_map<std::string,size_t> nt_channels_;  // channel -> listeners
 std::atomic<size_t> nt_count_{0},nt_watch_{0};  // (listener, channel) pairs; of them table_changes ones
 // Writer lock held: hands each listener the notes of its channels, in order.
 void emit_locked(const std::vector<Note>&notes){
  if(notes.empty()||!nt_count_.load(std::memory_order_relaxed))return;
  std::lock_guard<std::mutex>g(nt_m_);std::vector<const Note*> mine;
  for(auto*l:nt_listeners_){
   mine.clear();for(const auto&n:notes)if(l->channels.count(n.channel))mine.push_back(&n);
   if(!mine.empty())try{l->deliver(mine.data(),mine.size());}catch(...){}
  }
 }
 // An id as JSON from a SQL row key's order-preserving encoding (json::encode_into); a key
 // not so encoded (a key/value write into a SQL table's collection) as a string.
 static std::string id_json(std::string_view e){
  json::Scalar s;uint8_t tag=e.empty()?0:uint8_t(e[0]);
  if(tag==json::NUMBER&&e.size()==9){
   s.tag=json::NUMBER;uint64_t b=0;for(size_t i=1;i<9;i++)b=b<<8|uint8_t(e[i]);b=(b>>63)?(b&~(uint64_t(1)<<63)):~b;memcpy(&s.num,&b,8);
   if(std::isfinite(s.num))return sql::to_json(s);
  }else if(tag==json::STRING&&e.size()>=3&&e.compare(e.size()-2,2,std::string_view("\0\1",2))==0){
   s.tag=json::STRING;for(size_t i=1;i+2<e.size();i++){s.str+=e[i];if(e[i]==0)i++;}  // 0x00 0xFF: 0x00
   return sql::to_json(s);
  }
  return json::quote(e);
 }
 // Writer lock held, before the group installs: a note per key that the accepted transactions
 // change in a watched collection (insert or update by whether the key exists before it).
 void change_notes_locked(const std::vector<size_t>&accepted,const std::vector<const std::vector<Op>*>&effective,const std::vector<Outcome>&out,std::vector<Note>&notes){
  try{
   std::lock_guard<std::mutex>g(nt_m_);std::unordered_map<std::string_view,bool> live;std::string channel(CHANGES_PREFIX);
   for(auto t:accepted)for(const auto&o:*effective[t]){
    if(o.key.empty()||uint8_t(o.key[0])==1)continue;  // the engine's own records
    auto z=o.key.find('\0');if(z==std::string::npos)continue;
    channel.resize(CHANGES_PREFIX.size());channel.append(o.key,0,z);
    if(!nt_channels_.count(channel))continue;
    bool existed;auto it=live.find(o.key);
    if(it!=live.end())existed=it->second;else{auto h=index_hash(key_,o.key);Ref f;existed=lookup(shard_of(h),o.key,h,f);}
    live[o.key]=!o.erase;
    if(o.erase&&!existed)continue;
    std::string table=o.key.substr(0,z);std::string_view id=std::string_view(o.key).substr(z+1);
    notes.push_back({channel,std::string("{\"op\":\"")+(o.erase?"delete":existed?"update":"insert")+"\",\"table\":"+json::quote(table)+",\"key\":"+(table_exists(table)?id_json(id):json::quote(id))+"}",out[t].version});
   }
  }catch(...){notes.clear();}
 }
 static void check_note(std::string_view channel,std::string_view payload){
  check_channel(channel);
  if(changes_channel(channel))throw std::runtime_error("INPUT channels starting with table_changes: carry the engine's change notifications; NOTIFY another channel");
  if(payload.size()>MAX_NOTIFY_PAYLOAD)throw std::runtime_error("LIMIT a notification payload is at most 8000 bytes");
 }
 std::string run_sql(const sql::Listen&s,const std::vector<json::Scalar>&){
  auto*ses=tl_session();
  if(!ses||!ses->listener)throw std::runtime_error("UNSUPPORTED SQL: LISTEN needs a connection that receives notifications (Database, or RemoteDB over tcp:// or unix://)");
  if(s.unlisten)unlisten(*ses->listener,s.channel,s.all);else listen(*ses->listener,s.channel);
  return "{\"changes\":0}";
 }
 // In a transaction: kept until COMMIT (one per distinct channel + payload). Otherwise sent at
 // once, in order with commits.
 std::string run_sql(const sql::Notify&s,const std::vector<json::Scalar>&params){
  std::string channel=s.channel,payload;
  auto text=[&](const sql::Expr&e,const char*what){sql::Row ctx{"{}",&params};auto v=sql::eval(e,ctx);if(sql::is_null(v))return std::string();if(v.tag!=json::STRING)throw std::runtime_error(std::string("INPUT SQL: the notification ")+what+" must be text");return v.str;};
  if(s.channel_expr)channel=text(*s.channel_expr,"channel");
  if(s.payload)payload=text(*s.payload,"payload");
  check_note(channel,payload);
  const char*result=s.select?"{\"rows\":[{\"pg_notify\":null}]}":"{\"changes\":0}";
  if(auto*ses=active_session()){
   if(!ses->noted.emplace(channel,payload).second)return result;
   require(ses->notes.size()<MAX_TRANSACTION_NOTES,"LIMIT a transaction sends at most 65,536 notifications");
   ses->notes.push_back({std::move(channel),std::move(payload),0});
   return result;
  }
  if(!nt_count_.load(std::memory_order_relaxed))return result;
  WriterLock w(*this);emit_locked({Note{std::move(channel),std::move(payload),meta_.txid}});
  return result;
 }
public:

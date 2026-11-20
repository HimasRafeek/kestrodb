#pragma once
#include "platform.hpp"
#include "sha256.hpp"
#include "blake3.h"
#include "siphash.hpp"
#include "keyindex.hpp"
#include "pool.hpp"
#include "background.hpp"
#include <array>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <atomic>
#include <system_error>
#include <exception>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <climits>
#include <cmath>
#include <deque>
#include <optional>
#include "json.hpp"
#include "query.hpp"
#include "sql.hpp"
#include <tuple>
#include <map>
#include <set>
#include <fstream>
#include <functional>
#include "sortedset.hpp"

namespace kestro {
using Bytes=std::vector<unsigned char>;
constexpr uint64_t START=8192, ANY=UINT64_MAX, MAX_GROUP=64*1024*1024;
using FD=platform::FD;
using File=platform::File;
using Mapping=platform::Mapping;
using ReadWriteMutex=platform::ReadWriteMutex;
inline void ioerror(){platform::error("file operation");}
// Unkeyed BLAKE3-256, as published (test vectors).
inline Hash blake3_digest(const void*p,size_t n){Hash h{};blake3_hasher state;blake3_hasher_init(&state);blake3_hasher_update(&state,p,n);blake3_hasher_finalize(&state,h.data(),h.size());return h;}
// Point-read checksum. It lives only in the RAM index: rebuilt from the SHA-256-verified
// file on every open and never stored, so changing it does not change the format.
// It is keyed with a secret drawn at process start: without the key nobody can make
// altered bytes match. Values up to 2 KiB use SipHash-2-4-128 (85 ns for 226 bytes
// against 230 ns for BLAKE3); larger values use keyed BLAKE3 truncated to 128 bits,
// which wins once BLAKE3's SIMD paths engage.
using Digest=std::array<unsigned char,16>;
// index_seed seeds the in-memory key hash. All three are per store and persisted in
// the index snapshot, so any process loading it lays the index out identically.
struct DigestKey{uint64_t sip[2];unsigned char blake[32];uint64_t index_seed;};
// Process-wide secret: seeds the in-memory key hash, and keys tests.
inline const DigestKey&digest_key(){static const DigestKey key=[]{DigestKey k{};platform::random_bytes(&k,sizeof k);return k;}();return key;}
constexpr size_t SIPHASH_LIMIT=2048;
inline Digest value_digest(const DigestKey&k,const void*p,size_t n){
 Digest d{};
 if(n<=SIPHASH_LIMIT)sip::hash128(k.sip[0],k.sip[1],p,n,d.data());
 else{blake3_hasher s;blake3_hasher_init_keyed(&s,k.blake);blake3_hasher_update(&s,p,n);blake3_hasher_finalize(&s,d.data(),d.size());}
 return d;
}
inline void put64(unsigned char*p,uint64_t v){for(int i=0;i<8;i++)p[i]=(v>>(i*8))&255;}
inline void put32(unsigned char*p,uint32_t v){for(int i=0;i<4;i++)p[i]=(v>>(i*8))&255;}
inline uint64_t get64(const unsigned char*p){uint64_t v=0;for(int i=7;i>=0;i--)v=(v<<8)|p[i];return v;}
inline uint32_t get32(const unsigned char*p){uint32_t v=0;for(int i=3;i>=0;i--)v=(v<<8)|p[i];return v;}
inline void write_at(File fd,const void*p,size_t n,uint64_t off){platform::write_exact(fd,p,n,off);}
inline Bytes read_at(File fd,size_t n,uint64_t off){Bytes b(n);platform::read_exact(fd,b.data(),n,off);return b;}
// Bounded, buffered parsing of large checkpoints. No full-frame allocation.
class FrameReader {
 File fd_;uint64_t pos_,end_;Bytes buffer_;size_t cursor_=0,valid_=0;StreamHash hash_;
public:
 FrameReader(File fd,uint64_t start,uint64_t bytes):fd_(fd),pos_(start),end_(start+bytes),buffer_(1024*1024){}
 uint64_t position()const{return pos_;}
 uint64_t remaining()const{return end_-pos_;}
 void take(void*out,size_t n){require(n<=remaining(),"CORRUPT checkpoint bounds");auto dst=static_cast<unsigned char*>(out);
  while(n){if(cursor_==valid_){valid_=std::min<uint64_t>(buffer_.size(),end_-pos_);cursor_=0;platform::read_exact(fd_,buffer_.data(),valid_,pos_);}
   auto k=std::min(n,valid_-cursor_);memcpy(dst,buffer_.data()+cursor_,k);hash_.update(dst,k);cursor_+=k;pos_+=k;dst+=k;n-=k;
  }
 }
 Hash finish(){require(!remaining(),"CORRUPT checkpoint trailing bytes");return hash_.finish();}
};
class FrameWriter {
 File fd_;uint64_t pos_;Bytes buffer_;StreamHash hash_;
public:
 FrameWriter(File fd,uint64_t start):fd_(fd),pos_(start){buffer_.reserve(1024*1024);}
 void flush(){if(!buffer_.empty()){write_at(fd_,buffer_.data(),buffer_.size(),pos_);pos_+=buffer_.size();buffer_.clear();}}
 void add(const void*p,size_t n){hash_.update(p,n);auto src=static_cast<const unsigned char*>(p);while(n){auto k=std::min(n,1024*1024-buffer_.size());buffer_.insert(buffer_.end(),src,src+k);src+=k;n-=k;if(buffer_.size()==1024*1024)flush();}}
 Hash finish(){flush();return hash_.finish();}
};
inline void sync_file(File fd){platform::sync(fd);}
inline uint64_t file_size(File fd){return platform::size(fd);}
inline void sync_parent(const std::string&p){platform::sync_parent(p);}
struct Meta {uint64_t generation=0,txid=0,end=START;std::array<unsigned char,16> uuid{};Hash chain{};};
// Format markers. Files written before the rename to KestroDB use the "HDB" spelling of
// the commit slot and group markers; both are read, new ones are written as "KDB". (The
// index snapshot and the .idx cache only use "KDB": older ones are rebuilt.)
constexpr const char MAGIC_SLOT[]="KDBNAT02",MAGIC_GROUP[]="KDBG0002";
inline bool is_slot_magic(const void*p){return !memcmp(p,MAGIC_SLOT,8)||!memcmp(p,"HDBNAT02",8);}
inline bool is_group_magic(const void*p){return !memcmp(p,MAGIC_GROUP,8)||!memcmp(p,"HDBG0002",8);}
inline Bytes meta_bytes(const Meta&m){Bytes b(4096);memcpy(b.data(),MAGIC_SLOT,8);put64(b.data()+8,m.generation);put64(b.data()+16,m.txid);put64(b.data()+24,m.end);memcpy(b.data()+32,m.uuid.data(),16);memcpy(b.data()+48,m.chain.data(),32);auto f=digest(b.data(),80);memcpy(b.data()+80,f.data(),32);auto h=digest(b.data(),4064);memcpy(b.data()+4064,h.data(),32);return b;}
inline bool decode_meta(const unsigned char*b,Meta&m){
 bool zero=true;for(int i=0;i<4096;i++)if(b[i]){zero=false;break;}if(zero)return false;
 require(is_slot_magic(b),"FORMAT unsupported native file");
 // A slot is written without waiting (the log group is the commit point), so a power loss can
 // tear it at sector boundaries: its fields and their own digest (bytes 0..111) share the
 // first sector, so a slot whose whole-page digest fails is still whole when that one holds.
 auto h=digest(b,4064);if(memcmp(h.data(),b+4064,32)){auto f=digest(b,80);require(!memcmp(f.data(),b+80,32),"CORRUPT commit slot");}
 m.generation=get64(b+8);m.txid=get64(b+16);m.end=get64(b+24);memcpy(m.uuid.data(),b+32,16);memcpy(m.chain.data(),b+48,32);
 require(m.end>=START&&m.end<(uint64_t(1)<<60),"CORRUPT boundary");return true;
}
inline Meta read_meta(File fd){auto b=read_at(fd,START,0);Meta a,c;bool x=decode_meta(b.data(),a),y=decode_meta(b.data()+4096,c);require(x||y,"FORMAT no metadata");if(!x)a=c;else if(y){if(a.generation<c.generation)std::swap(a,c);require(a.uuid==c.uuid&&a.generation>c.generation&&a.end>=c.end&&a.txid>=c.txid,"CORRUPT metadata sequence");}require(file_size(fd)>=a.end,"TRUNCATED committed file");return a;}
// Opening as the owner: the newest valid slot (one damaged slot is tolerated: since 0.8 a
// slot may be written after its commit without waiting for the disk), then every
// complete group after its boundary that continues the chain (next transaction ID,
// previous hash, SHA-256 intact). A group on disk is committed; the slot is a hint of
// where to start. rewrite: the slots should be rewritten to match.
// Where a flush covers a commit's group and its slot together (POSIX), a power loss can keep
// the slot and lose or tear the group: a newer slot counts only when the groups after the
// older slot lead to it; otherwise the older slot is the start (the commit was never
// acknowledged).
inline Meta recover_meta(File fd,bool&rewrite){
 auto b=read_at(fd,START,0);Meta a,c;bool x=false,y=false;std::string why;
 auto one=[&](const unsigned char*p,Meta&m){try{return decode_meta(p,m);}catch(const std::exception&e){why=e.what();rewrite=true;return false;}};
 x=one(b.data(),a);y=one(b.data()+4096,c);
 if(!x&&!y){if(!why.empty())throw std::runtime_error(why);throw std::runtime_error("FORMAT no metadata");}
 if(!x)a=c;else if(y){if(a.generation<c.generation)std::swap(a,c);require(a.uuid==c.uuid&&a.generation>c.generation&&a.end>=c.end&&a.txid>=c.txid,"CORRUPT metadata sequence");if(a.generation!=c.generation+1)rewrite=true;}
 auto size=file_size(fd);
 // Follows complete groups continuing m's chain, up to stop; returns whether any was taken.
 auto advance=[&](Meta&m,uint64_t stop){bool any=false;
  while(m.end<stop&&size>=m.end&&size-m.end>=96){
   auto head=read_at(fd,64,m.end);const unsigned char*h=head.data();
   if(!is_group_magic(h))break;
   uint64_t len=get64(h+8),seq=get64(h+16);uint32_t flags=get32(h+24),count=get32(h+28);
   if(flags||seq!=m.txid+1||len>MAX_GROUP||len>size-m.end-96||count>1000000||memcmp(h+32,m.chain.data(),32))break;
   auto whole=read_at(fd,size_t(64+len+32),m.end);auto d=digest(whole.data(),size_t(64+len));if(memcmp(d.data(),whole.data()+64+len,32))break;
   m.end+=96+len;m.txid=seq;m.chain=d;any=true;
  }
  return any;};
 if(x&&y&&a.end>c.end){
  Meta w=c;advance(w,a.end);
  if(w.end!=a.end||w.txid!=a.txid||w.chain!=a.chain){a=w;rewrite=true;}  // the newer slot's groups are not all on disk
 }
 require(size>=a.end,"TRUNCATED committed file");
 if(advance(a,UINT64_MAX))rewrite=true;
 return a;
}
// incr: value holds a JSON number to add to the current value (resolved inside the commit).
struct Op {std::string key,value;uint64_t expected=ANY;bool erase=false;bool incr=false;};
struct Ref {uint64_t offset=0,version=0;uint32_t length=0;Digest hash{};const char* mapped=nullptr;};
using Index=CompactIndex<Ref>;
// Read checksums are compared on their first 8 bytes: the compact index keeps 64 bits of
// the keyed checksum per value (enough to catch corruption; the snapshot format keeps 16).
inline bool digest_eq(const Digest&a,const Digest&b){return !memcmp(a.data(),b.data(),8);}
// A read's check: value_digest(k,p,n) matches h on those 8 bytes. Same result, computed
// without the second half of SipHash-128's output.
inline bool value_matches(const DigestKey&k,const void*p,size_t n,const Digest&h){
 if(n>SIPHASH_LIMIT)return digest_eq(value_digest(k,p,n),h);
 uint64_t low=sip::hash128_low(k.sip[0],k.sip[1],p,n);unsigned char b[8];for(int i=0;i<8;i++)b[i]=(unsigned char)(low>>(8*i));return !memcmp(b,h.data(),8);
}
// One hash per key: its top 6 bits pick the shard, its low bits the slot.
inline uint64_t index_hash(const DigestKey&d,std::string_view k){return key_hash(k.data(),k.size(),d.index_seed);}
struct Value {bool found=false;uint64_t version=0;std::string value;};
// Tests only: open by replaying the log even when a valid index snapshot is present.
inline std::atomic<bool> ignore_index_snapshot{false};
// Tests only: replay through bounded reads, as when the file cannot be mapped.
inline std::atomic<bool> force_streaming_replay{false};
enum class ReadMode { Mapped, Pread };
struct Options {
 ReadMode reads=platform::mapped_supported?ReadMode::Mapped:ReadMode::Pread;
 // Both policies wait for the platform file flush before acknowledgement. Ordered mode also
 // forces data to stable storage before modifying the commit slot.
 bool orderedSync=false;
 // Write an index snapshot on clean close so the next open can skip replaying the log.
 bool indexSnapshot=true;
 // After a snapshot open, finish verifying the log before the constructor returns
 // (a corrupt file then fails to open, as with a replay) instead of in the background.
 bool verifyOnOpen=false;
 // Background verification after a snapshot open starts this long after the open. A
 // store closed sooner skips it (reads still check every value's checksum).
 unsigned verifyDelayMs=50;
 // Background index checkpoint once commits have been quiet this long (0: only on close).
 unsigned snapshotIdleMs=50;
 // RAM for the key index. Past it, the index moves to a memory-mapped file next to the
 // database (<path>.idx) and RAM keeps only recent changes; 0 keeps it all in memory.
 size_t indexMemory=size_t(32)<<20;
 // RAM for ordered keys and secondary indexes (SQL ORDER BY, ranges, indexed WHERE). Past
 // it, their least recently changed pages move to a scratch file (<path>.sets) read
 // through the OS cache, and builds sort in bounded RAM; 0 keeps them all in memory.
 size_t orderedMemory=size_t(16)<<20;
};

class Reader;
class Store {
 friend class Reader;
 FD fd_;std::string path_;Meta meta_;Options options_;
 // The handle readers use when they read a value outside every lock (read, get): published
 // when the file opens and when an online compaction switches files (whose old file stays
 // open until close, so a reader that loaded the old handle still reads valid bytes).
 std::atomic<File> rfd_{platform::invalid_file()};
 File read_fd()const{return rfd_.load(std::memory_order_acquire);}
 // Write-through platforms: commit slots go through this cached handle, so a commit waits
 // for one disk write (its group), not two. Flushed on close.
 FD slot_fd_;
 struct alignas(64) Shard {mutable ReadWriteMutex mutex;Index index;};
 std::array<Shard,64> shards_;size_t index_count_=0;
 static size_t shard_of(uint64_t h){return size_t(h>>58);}
 static constexpr uint64_t MAP_CHUNK=64*1024*1024;
 // Overlap windows by maximum value length so a value can cross a boundary.
 // Mappings never move or unmap while Store is alive; published references
 // therefore stay valid for readers overlapping append-only writes.
 std::unordered_map<uint64_t,std::unique_ptr<Mapping>> maps_;
 // Where a platform cannot map past end of file (Windows), a view covers only the
 // reserved bytes and is replaced by a longer one as data grows. Replaced views stay
 // mapped until the Store closes, for readers still holding their pointers.
 std::vector<std::unique_ptr<Mapping>> retired_;
 mutable std::mutex writer_;mutable ReadWriteMutex readers_;std::atomic<bool> failed_{false};
 std::atomic<uint64_t> syncs_{0};  // file flushes made by commits (stats)
 std::atomic<uint64_t> commits_{0},commit_groups_{0};  // transactions and groups committed (stats)
 // File bytes already written (committed data plus zero-filled reserve). Commits
 // overwrite reserved bytes instead of growing the file on every flush, which spares
 // the file system a size/allocation update per commit and lets Windows map them.
 // Bytes past meta_.end are the format's uncommitted tail: every version validates
 // the committed prefix and truncates them on open, so the format is unchanged.
 uint64_t reserved_=START;
 // Point-read checksum key: random for a store opened by replay, or taken from the
 // index snapshot it was opened from (the snapshot's checksums were made with it).
 DigestKey key_{};
 // Opened from an index snapshot that still sits, unchanged, in the file's tail.
 bool snapshot_intact_=false;bool from_snapshot_=false;
 // Background verification after a snapshot open: 0 verified while opening (replay),
 // 1 running, 2 passed, 3 failed (every operation then refuses, with fault_).
 // Idle checkpoints: a thread that writes the index snapshot once commits go quiet.
 std::thread maintainer_;std::mutex mt_m_;std::condition_variable mt_cv_;bool mt_stop_=false,mt_dirty_=false;
 std::chrono::steady_clock::time_point last_commit_{};
 uint64_t snapshot_bytes_=0;  // size of the snapshot in the tail while snapshot_intact_
 // Lazy index: after a snapshot open, lookups probe the snapshot's hash tables in the
 // mapped file until the first write builds the in-memory index (materialize).
 // lazy_ is read under a shard lock and changed only with every shard lock held.
 bool lazy_=false;std::array<const unsigned char*,64> lazy_sec_{};std::array<uint64_t,64> lazy_len_{};
 const char*refs_base_=nullptr;  // read view base for entries, when mapped reads cover the file
 // Each section's BLAKE3 is checked the first time it is used (0 unchecked, 1 good, 2 bad).
 std::array<Hash,64> sec_hash_{};mutable std::array<std::atomic<uint8_t>,64> sec_state_{};
 struct SnapshotDamaged{};  // internal: a lazily checked section failed; rebuild and retry
 std::atomic<int> verify_state_{0};std::atomic<bool> stop_verify_{false};std::string fault_;std::shared_ptr<Background::Task> verify_task_;
 void reserve(uint64_t end){
  if(end<=reserved_)return;
  uint64_t step=std::clamp<uint64_t>(meta_.end/8,uint64_t(1)<<20,uint64_t(64)<<20);
  uint64_t target=(end/step+1)*step;
  static const Bytes zeros(1<<20,0);
  for(uint64_t off=reserved_;off<target;){auto n=std::min<uint64_t>(zeros.size(),target-off);write_at(fd_.n,zeros.data(),size_t(n),off);off+=n;}
  // Write-through commits need the new size and allocation durable before they rely on it.
  if(platform::write_through)sync_file(fd_.n);
  reserved_=target;
 }
 static void init(File fd){Meta m;platform::random_bytes(m.uuid.data(),m.uuid.size());auto b=meta_bytes(m);b.resize(START);write_at(fd,b.data(),b.size(),0);sync_file(fd);}

 void healthy()const{if(verify_state_.load(std::memory_order_acquire)==3)throw std::runtime_error(fault_);require(!failed_,"UNKNOWN_COMMIT connection failed; reopen after storage repair");}
 using Sharded=std::array<Index,64>;
 static size_t total(const Sharded&s){size_t n=0;for(const auto&i:s)n+=i.size();return n;}
 static bool all_empty(const Sharded&s){for(const auto&i:s)if(!i.empty())return false;return true;}
 template<class F> static void parallel_for(size_t n,bool worth,F&&f){Pool::get().parallel_for(n,worth,std::forward<F>(f));}
 // Applies one normal group's operations. whole points at the group (header, payload,
 // hash) found at file offset pos. Read checksums are computed here only when
 // digests is true; the mapped replay computes them afterwards for surviving values.
 static void apply_group(const unsigned char*whole,uint64_t pos,uint64_t len,uint64_t seq,uint32_t count,Sharded&out,bool digests,const DigestKey&dkey){
  size_t p=64;
  // Duplicate keys within a group: a flat open-addressing set, reused across groups.
  thread_local std::vector<std::pair<uint64_t,std::string_view>> seen;size_t mask=0;
  if(count>1){size_t cap=4;while(cap<2*size_t(count))cap*=2;seen.assign(cap,{});mask=cap-1;}
  for(uint32_t i=0;i<count;i++){
   require(p+17<=64+len,"CORRUPT operation header");auto kind=whole[p];auto expect=get64(whole+p+1);auto kn=get32(whole+p+9),vn=get32(whole+p+13);p+=17;
   require(kind<=1&&kn&&kn<=1024&&vn<=16*1024*1024&&uint64_t(p)+kn+vn<=64+len&&(!kind?vn==0:true),"CORRUPT operation bounds");
   std::string_view key(reinterpret_cast<const char*>(whole+p),kn);p+=kn;auto kh=index_hash(dkey,key);
   if(count>1){size_t j=kh&mask;for(;seen[j].second.data();j=(j+1)&mask)require(!(seen[j].first==kh&&seen[j].second==key),"CORRUPT duplicate key");seen[j]={kh,key};}
   auto&idx=out[shard_of(kh)];
   if(kind){
    // One lookup: a failed expectation aborts the whole replay, so the speculative insert never survives.
    Ref old;bool fresh=idx.put(key,kh,Ref{pos+p,seq,vn,digests?value_digest(dkey,whole+p,vn):Digest{}},&old);require(expect==ANY||expect==(fresh?0:old.version),"CORRUPT expected version");
   }else{Ref it;bool have=idx.find(key,kh,it);require(expect==ANY||expect==(have?it.version:0),"CORRUPT expected version");idx.erase(key,kh);}
   p+=vn;
  }
  require(p==64+len,"CORRUPT trailing operation bytes");
 }
 // Fallback when the file cannot be mapped: bounded reads, one group at a time.
 static Hash replay_checkpoint(File fd,uint64_t pos,uint64_t len,uint64_t seq,uint32_t count,Sharded&out,const DigestKey&dkey){
  require(all_empty(out),"CORRUPT checkpoint must be first");
  FrameReader reader(fd,pos,64+len);std::array<unsigned char,64> head{};reader.take(head.data(),head.size());
  require(count<=len/18,"CORRUPT checkpoint count");
  for(uint32_t i=0;i<count;i++){
   unsigned char op[17];reader.take(op,sizeof(op));auto version=get64(op+1);auto kn=get32(op+9),vn=get32(op+13);
   require(op[0]==1&&version>0&&version<=seq&&kn&&kn<=1024&&vn<=16*1024*1024&&uint64_t(kn)+vn<=reader.remaining(),"CORRUPT checkpoint operation");
   std::string key(kn,'\0'),value(vn,'\0');reader.take(key.data(),kn);auto offset=reader.position();reader.take(value.data(),vn);
   auto kh=index_hash(dkey,key);require(out[shard_of(kh)].put(key,kh,Ref{offset,version,vn,value_digest(dkey,value.data(),vn)}),"CORRUPT duplicate checkpoint key");
  }
  auto h=reader.finish();auto stored=read_at(fd,32,pos+64+len);require(!memcmp(h.data(),stored.data(),32),"CORRUPT checkpoint hash");return h;
 }
 static void replay_streaming(File fd,const Meta&m,Sharded&out,const DigestKey&dkey){
  uint64_t pos=START,tx=0;Hash prev{};bool first=true;
  while(pos<m.end){
   require(m.end-pos>=96,"CORRUPT short frame");auto head=read_at(fd,64,pos);require(is_group_magic(head.data()),"CORRUPT group magic");
   auto len=get64(head.data()+8),seq=get64(head.data()+16);uint32_t flags=get32(head.data()+24),count=get32(head.data()+28);
   require(flags<=1&&(!flags||first),"CORRUPT checkpoint location");require(flags?seq>=tx:seq==tx+1,"CORRUPT transaction sequence");
   require((flags||len<=MAX_GROUP)&&len<=m.end-pos-96,"CORRUPT frame length");require(flags||count<=1000000,"CORRUPT count");require(!memcmp(head.data()+32,prev.data(),32),"CORRUPT chain");
   if(flags){auto h=replay_checkpoint(fd,pos,len,seq,count,out,dkey);pos+=96+len;tx=seq;prev=h;first=false;continue;}
   auto whole=read_at(fd,64+len+32,pos);auto h=digest(whole.data(),64+len);require(!memcmp(h.data(),whole.data()+64+len,32),"CORRUPT frame hash");
   apply_group(whole.data(),pos,len,seq,count,out,true,dkey);pos+=96+len;tx=seq;prev=h;first=false;
  }
  require(tx==m.txid&&prev==m.chain,"CORRUPT final boundary");
 }
 // Groups [pos, end) appended after a replayed prefix (tx and prev: its last transaction and
 // hash), applied to out with the same checks as replay_streaming (online compaction: the
 // new file's last groups, replayed while the writer lock is held).
 static void replay_tail(File fd,uint64_t pos,uint64_t tx,Hash prev,uint64_t end,Sharded&out,const DigestKey&dkey){
  while(pos<end){
   require(end-pos>=96,"CORRUPT short frame");auto head=read_at(fd,64,pos);require(is_group_magic(head.data()),"CORRUPT group magic");
   auto len=get64(head.data()+8),seq=get64(head.data()+16);uint32_t flags=get32(head.data()+24),count=get32(head.data()+28);
   require(!flags&&seq==tx+1,"CORRUPT transaction sequence");require(len<=MAX_GROUP&&len<=end-pos-96,"CORRUPT frame length");require(count<=1000000,"CORRUPT count");require(!memcmp(head.data()+32,prev.data(),32),"CORRUPT chain");
   auto whole=read_at(fd,64+len+32,pos);auto h=digest(whole.data(),64+len);require(!memcmp(h.data(),whole.data()+64+len,32),"CORRUPT frame hash");
   apply_group(whole.data(),pos,len,seq,count,out,true,dkey);pos+=96+len;tx=seq;prev=h;
  }
 }
 // Default replay: the committed range mapped once. The same checks run in the same
 // order as replay_streaming. Groups are taken in windows of up to WINDOW_GROUPS, so
 // memory stays bounded however long the log is. In each window, pool workers compute
 // the groups' SHA-256 in log order while this thread checks and applies them; it
 // waits only when it catches up, so every group is verified before it is used. Read
 // checksums are computed at the end, per shard in parallel, only for surviving values.
 static constexpr size_t WINDOW_GROUPS=65536;
 static void replay_mapped(const unsigned char*b,const Meta&m,Sharded&out,const DigestKey&dkey){
  bool worth=m.end-START>=(uint64_t(256)<<10);
  struct Span{uint64_t pos,len;};std::vector<Span> spans;std::vector<Hash> computed;
  std::unique_ptr<std::atomic<uint8_t>[]> ready(new std::atomic<uint8_t>[WINDOW_GROUPS]);
  uint64_t pos=START,tx=0;Hash prev{};bool first=true;
  while(pos<m.end){
   // Locate this window's groups without judging them; the checks below report problems.
   spans.clear();
   for(uint64_t p=pos;spans.size()<WINDOW_GROUPS&&p<m.end&&m.end-p>=96&&is_group_magic(b+p);){auto len=get64(b+p+8);if(len>m.end-p-96)break;spans.push_back({p,len});p+=96+len;}
   computed.assign(spans.size(),Hash{});for(size_t i=0;i<spans.size();i++)ready[i].store(0,std::memory_order_relaxed);
   auto hash_one=[&](size_t i){try{computed[i]=digest(b+spans[i].pos,size_t(64+spans[i].len));ready[i].store(1,std::memory_order_release);}catch(...){ready[i].store(2,std::memory_order_release);throw;}};
   std::unique_ptr<Pool::Batch> hashing;
   if(worth&&spans.size()>1&&Pool::get().workers())hashing=std::make_unique<Pool::Batch>(Pool::get(),spans.size(),hash_one);
   else for(size_t i=0;i<spans.size();i++)hash_one(i);
   auto hash_of=[&](size_t i)->const Hash&{
    if(hashing){while(ready[i].load(std::memory_order_acquire)==0)if(!hashing->help_one())std::this_thread::yield();
     if(ready[i].load(std::memory_order_acquire)==2)hashing->finish();}  // rethrows the hashing error
    return computed[i];};
   size_t k=0;
   do{  // at least one group per window, so a bad header is always reported
    require(m.end-pos>=96,"CORRUPT short frame");const unsigned char*head=b+pos;require(is_group_magic(head),"CORRUPT group magic");
    auto len=get64(head+8),seq=get64(head+16);uint32_t flags=get32(head+24),count=get32(head+28);
    require(flags<=1&&(!flags||first),"CORRUPT checkpoint location");require(flags?seq>=tx:seq==tx+1,"CORRUPT transaction sequence");
    require((flags||len<=MAX_GROUP)&&len<=m.end-pos-96,"CORRUPT frame length");require(flags||count<=1000000,"CORRUPT count");require(!memcmp(head+32,prev.data(),32),"CORRUPT chain");
    Hash h=k<spans.size()&&spans[k].pos==pos?hash_of(k):digest(head,size_t(64+len));k++;
    if(flags){
     require(all_empty(out),"CORRUPT checkpoint must be first");require(count<=len/18,"CORRUPT checkpoint count");
     uint64_t cur=pos+64,end=pos+64+len;
     for(uint32_t i=0;i<count;i++){
      require(end-cur>=17,"CORRUPT checkpoint bounds");const unsigned char*op=b+cur;cur+=17;auto version=get64(op+1);auto kn=get32(op+9),vn=get32(op+13);
      require(op[0]==1&&version>0&&version<=seq&&kn&&kn<=1024&&vn<=16*1024*1024&&uint64_t(kn)+vn<=end-cur,"CORRUPT checkpoint operation");
      std::string_view key(reinterpret_cast<const char*>(b+cur),kn);cur+=kn;
      auto kh=index_hash(dkey,key);require(out[shard_of(kh)].put(key,kh,Ref{cur,version,vn,Digest{}}),"CORRUPT duplicate checkpoint key");cur+=vn;
     }
     require(cur==end,"CORRUPT checkpoint trailing bytes");require(!memcmp(h.data(),b+end,32),"CORRUPT checkpoint hash");
    }else{
     require(!memcmp(h.data(),head+64+len,32),"CORRUPT frame hash");
     apply_group(head,pos,len,seq,count,out,false,dkey);
    }
    pos+=96+len;tx=seq;prev=h;first=false;
   }while(pos<m.end&&k<spans.size());
   if(hashing)hashing->finish();
  }
  require(tx==m.txid&&prev==m.chain,"CORRUPT final boundary");
  parallel_for(out.size(),worth,[&](size_t s){out[s].update_each([&](std::string_view,Ref&r){r.hash=value_digest(dkey,b+r.offset,r.length);});});
 }
 // keep, when given, receives the view of [0, m.end) the replay used, so reads can reuse it.
 static void replay(File fd,const Meta&m,Sharded&out,const DigestKey&key,std::unique_ptr<Mapping>*keep=nullptr){
  std::unique_ptr<Mapping> view;
  if(m.end>START){view=std::make_unique<Mapping>(fd,0,size_t(m.end));}
  if(view&&view->address&&!force_streaming_replay){platform::prefetch(view->address,size_t(m.end));replay_mapped(static_cast<const unsigned char*>(view->address),m,out,key);if(keep)*keep=std::move(view);}
  else replay_streaming(fd,m,out,key);
 }
 // Appends one transaction's group (header, operations, SHA-256 chained to chain) to out.
 // Every limit is checked before anything is appended.
 static void encode_into(Bytes&out,const std::vector<Op>&ops,const Hash&chain,uint64_t tx){
  uint64_t bytes=0;for(const auto&o:ops){require(!o.key.empty()&&o.key.size()<=1024&&o.value.size()<=16*1024*1024,"LIMIT key or value");bytes+=17+o.key.size()+(o.erase?0:o.value.size());require(bytes<=MAX_GROUP,"LIMIT group/checkpoint exceeds 64 MiB");}
  require(ops.size()<=1000000,"LIMIT operation count");
  size_t at=out.size();out.resize(at+96+size_t(bytes));unsigned char*b=out.data()+at;
  memcpy(b,MAGIC_GROUP,8);put64(b+8,bytes);put64(b+16,tx);put32(b+24,0);put32(b+28,uint32_t(ops.size()));memcpy(b+32,chain.data(),32);
  size_t p=64;for(const auto&o:ops){b[p]=!o.erase;put64(b+p+1,o.expected);put32(b+p+9,uint32_t(o.key.size()));put32(b+p+13,o.erase?0:uint32_t(o.value.size()));p+=17;memcpy(b+p,o.key.data(),o.key.size());p+=o.key.size();if(!o.erase){memcpy(b+p,o.value.data(),o.value.size());p+=o.value.size();}}
  auto h=digest(b,size_t(64+bytes));memcpy(b+64+bytes,h.data(),32);
 }
 // Requires ref's bytes to lie inside reserved_ (always true: commits reserve first).
 // Most consecutive references fall in the same window; remember the last one.
 uint64_t last_base_=UINT64_MAX;const Mapping*last_view_=nullptr;
 // The current read view of each 64 MB chunk (256 TB of file), published with release so
 // readers can turn an offset into an address without a per-value pointer in the index.
 // Replaced views stay mapped (retired_) until the store closes.
 std::array<std::atomic<const Mapping*>,4096> views_{};
 void publish_view(uint64_t base,const Mapping*v){size_t c=size_t(base/MAP_CHUNK);if(c<views_.size())views_[c].store(v,std::memory_order_release);}
 const char*addr_of(uint64_t off,uint64_t len)const{
  if(options_.reads!=ReadMode::Mapped)return nullptr;
  size_t c=size_t(off/MAP_CHUNK);if(c>=views_.size())return nullptr;
  const Mapping*v=views_[c].load(std::memory_order_acquire);if(!v||!v->address)return nullptr;
  uint64_t base=uint64_t(c)*MAP_CHUNK;if(off+len-base>v->length)return nullptr;
  return static_cast<const char*>(v->address)+(off-base);
 }
 // Maps every chunk of the committed file (after open).
 void map_all(){if(options_.reads!=ReadMode::Mapped)return;for(uint64_t b=0;b<meta_.end;b+=MAP_CHUNK){Ref r;r.offset=b;r.length=uint32_t(std::min<uint64_t>(MAP_CHUNK,meta_.end-b));map_ref(r);}}
 void map_ref(Ref&ref){
  if(options_.reads!=ReadMode::Mapped||!ref.length)return;
  auto base=ref.offset/MAP_CHUNK*MAP_CHUNK;auto need=ref.offset+ref.length-base;
  if(base!=last_base_||!last_view_||last_view_->length<need){
   auto it=maps_.find(base);
   if(it==maps_.end()||it->second->length<need){
    uint64_t window=MAP_CHUNK+16*1024*1024;if(!platform::mapping_beyond_eof)window=std::min(window,reserved_-base);
    auto view=std::make_unique<Mapping>(fd_.n,base,size_t(window));
    if(it==maps_.end())it=maps_.emplace(base,std::move(view)).first;
    else{retired_.push_back(std::move(it->second));it->second=std::move(view);}
    publish_view(base,it->second.get());
   }
   last_base_=base;last_view_=it->second.get();
  }
  if(last_view_->address)ref.mapped=static_cast<const char*>(last_view_->address)+(ref.offset-base);
 }
 void prepare_refs(const std::vector<Op>&ops,uint64_t tx,uint64_t frame,std::vector<Ref>&refs){
  size_t p=64;
  for(const auto&o:ops){p+=17+o.key.size();Ref ref;
   if(!o.erase){ref={frame+p,tx,uint32_t(o.value.size()),value_digest(key_,o.value.data(),o.value.size())};p+=o.value.size();}
   refs.push_back(ref);
  }
 }
 // lower (paged index): per operation, whether the key is live below the delta.
 // held: shard locks the caller took (in shard order) and keeps after install returns.
 void install(const std::vector<Op>&ops,const Ref*refs,const uint8_t*lower=nullptr,std::array<std::unique_lock<ReadWriteMutex>,64>*held=nullptr){
  std::array<bool,64> touched{};std::vector<uint64_t> hs(ops.size());for(size_t i=0;i<ops.size();i++){hs[i]=index_hash(key_,ops[i].key);touched[shard_of(hs[i])]=true;}
  std::array<std::unique_lock<ReadWriteMutex>,64> own;auto&locks=held?*held:own;
  for(size_t i=0;i<shards_.size();i++)if(touched[i]&&!locks[i].owns_lock())locks[i]=std::unique_lock<ReadWriteMutex>(shards_[i].mutex);
  // All affected shards are locked together; no reader sees partial install.
  try{for(size_t i=0;i<ops.size();i++){size_t s=shard_of(hs[i]);auto&idx=shards_[s].index;
   if(lower){pg_apply(s,ops[i].key,hs[i],ops[i].erase?nullptr:&refs[i],lower[i]);continue;}
   pg_dirty_[s]=true;
   if(ops[i].erase){size_t d=idx.erase(ops[i].key,hs[i]);index_count_-=d;live_[s]-=d;}
   else{size_t d=idx.put(ops[i].key,hs[i],refs[i]);index_count_+=d;live_[s]+=d;}
  }}catch(...){failed_=true;throw;}
 }
 // Index snapshot. On clean close (and after commits go quiet) the index is written into
 // the file's uncommitted tail, right after the committed boundary, where every reader
 // (0.4 onward) treats bytes as garbage to discard. It is bound to the exact committed
 // state and BLAKE3-protected, so a stale or damaged snapshot is detected and ignored.
 //   header (160): "KDBIDX02", generation, txid, end, uuid[16], chain[32], entries,
 //                 total bytes, SipHash key[16], BLAKE3 key[32], index hash seed, 8 zero bytes
 //   64 x (u64 section offset from the snapshot start, u64 section length,
 //         BLAKE3-256 of the section): sections are checked in parallel
 //   section:      u32 count, u32 slots (a power of two, load at most 2/3), slots, entries.
 //                 A slot is 0 (empty) or (low 32 bits of the key hash) << 32 | entry
 //                 offset from the section start, linear probing from hash & (slots-1).
 //                 Entry: u16 key length, key, u64 offset, u32 length, u64 version,
 //                 16-byte read checksum.
 //   trailer:      BLAKE3-256 of the header and the table
 static constexpr size_t SNAP_HEAD=160,SNAP_ENTRY=48,SNAP_TABLE=64*SNAP_ENTRY;
 static uint32_t snap_slots(size_t count){uint32_t c=4;while(c*2<count*3)c*=2;return c;}
 bool section_ok(size_t s)const{
  auto st=sec_state_[s].load(std::memory_order_acquire);if(st)return st==1;
  auto h=blake3_digest(lazy_sec_[s],size_t(lazy_len_[s]));bool ok=h==sec_hash_[s];
  sec_state_[s].store(ok?1:2,std::memory_order_release);return ok;
 }
 bool sections_ok()const{bool ok=true;for(size_t s=0;s<64;s++)ok&=section_ok(s);return ok;}
 // Replaces a damaged lazy index with one rebuilt from the log. Writer lock held.
 void rebuild_locked()const{
  auto self=const_cast<Store*>(this);if(!self->lazy_)return;
  Sharded rec;replay(fd_.n,meta_,rec,key_);
  // Lock order everywhere: readers_, then the shards (as a commit and a compaction take them).
  std::unique_lock<ReadWriteMutex>r(readers_);self->catalog_epoch_++;
  std::array<std::unique_lock<ReadWriteMutex>,64> locks;for(size_t i=0;i<shards_.size();i++)locks[i]=std::unique_lock<ReadWriteMutex>(shards_[i].mutex);
  for(size_t i=0;i<shards_.size();i++){self->live_[i]=rec[i].size();self->shards_[i].index=std::move(rec[i]);}
  self->index_count_=total(shards_);self->lazy_=false;
 }
 void rebuild_index()const{std::lock_guard<std::mutex>w(writer_);rebuild_locked();}
 static size_t total(const std::array<Shard,64>&sh){size_t n=0;for(const auto&x:sh)n+=x.index.size();return n;}
 // Calls f(key, ref, hash) for every live entry of shard s, from whichever index is current.
 // Callers hold the shard's lock, or the writer lock (no materialization can run).
 template<class F> void for_each_live(size_t s,F&&f)const{
  if(!lazy_){
   const auto&d=shards_[s].index;
   d.for_each([&](std::string_view k,const Ref&r){if(r.offset)f(k,r,index_hash(key_,k));});
   if(has_frozen_[s])frozen_[s].for_each([&](std::string_view k,const Ref&r){if(!r.offset)return;auto h=index_hash(key_,k);Ref t;if(!d.find(k,h,t))f(k,r,h);});
   if(pgb_[s].p)pg_each_base(s,[&](std::string_view k,const Ref&r){auto h=index_hash(key_,k);Ref t;if(d.find(k,h,t)||(has_frozen_[s]&&frozen_[s].find(k,h,t)))return;f(k,r,h);});
   return;
  }
  if(!section_ok(s))throw SnapshotDamaged{};
  const unsigned char*sec=lazy_sec_[s];uint32_t count=get32(sec),slots=get32(sec+4);const unsigned char*p=sec+8+8*size_t(slots);
  for(uint32_t j=0;j<count;j++){size_t kn=size_t(p[0])|(size_t(p[1])<<8);std::string_view key(reinterpret_cast<const char*>(p+2),kn);p+=2+kn;
   Ref r;r.offset=get64(p);r.length=get32(p+8);r.version=get64(p+12);memcpy(r.hash.data(),p+20,16);p+=36;if(refs_base_&&r.length)r.mapped=refs_base_+r.offset;
   f(key,r,index_hash(key_,key));}
 }
 // Finds key in shard s (hash kh) in whichever index is current; same locking as above.
 bool lookup(size_t s,std::string_view key,uint64_t kh,Ref&out)const{
  if(!lazy_){if(shards_[s].index.find(key,kh,out))return out.offset!=0;return layered(s)&&lower_find(s,key,kh,out);}
  if(!section_ok(s))throw SnapshotDamaged{};
  const unsigned char*sec=lazy_sec_[s];uint32_t slots=get32(sec+4),mask=slots-1,tag=uint32_t(kh);uint64_t len=lazy_len_[s];
  for(uint32_t i=uint32_t(kh)&mask,n=0;n<slots;i=(i+1)&mask,n++){
   uint64_t slot=get64(sec+8+8*size_t(i));if(!slot)return false;if(uint32_t(slot>>32)!=tag)continue;
   uint64_t off=uint32_t(slot);require(off+2<=len,"CORRUPT index snapshot slot");const unsigned char*p=sec+off;size_t kn=size_t(p[0])|(size_t(p[1])<<8);
   require(off+2+kn+36<=len,"CORRUPT index snapshot slot");if(kn!=key.size()||memcmp(p+2,key.data(),kn))continue;p+=2+kn;
   out=Ref{};out.offset=get64(p);out.length=get32(p+8);out.version=get64(p+12);memcpy(out.hash.data(),p+20,16);
   require(out.offset>=START+64&&out.length<=16*1024*1024&&out.offset+out.length<=meta_.end&&out.version>=1,"CORRUPT index snapshot entry");
   if(refs_base_&&out.length)out.mapped=refs_base_+out.offset;return true;
  }
  return false;
 }
 // Decodes one snapshot section into an in-memory index, checking every entry.
 void parse_section(size_t s,const unsigned char*sec,uint64_t len,const DigestKey&k,uint64_t txid,Index&idx)const{
  require(len>=8,"snapshot section");uint32_t count=get32(sec),slots=get32(sec+4);require(slots>=4&&!(slots&(slots-1))&&count<=slots&&8+8*uint64_t(slots)<=len,"snapshot section");
  const unsigned char*p=sec+8+8*size_t(slots),*e=sec+len;idx.reserve(count);
  for(uint32_t j=0;j<count;j++){
   require(e-p>=2,"snapshot entry");size_t kn=size_t(p[0])|(size_t(p[1])<<8);require(kn&&kn<=1024&&size_t(e-p)>=2+kn+36,"snapshot entry");
   std::string_view key(reinterpret_cast<const char*>(p+2),kn);p+=2+kn;Ref r;r.offset=get64(p);r.length=get32(p+8);r.version=get64(p+12);memcpy(r.hash.data(),p+20,16);p+=36;
   require(r.offset>=START+64&&r.length<=16*1024*1024&&r.offset+r.length<=meta_.end&&r.version>=1&&r.version<=txid,"snapshot entry");if(refs_base_&&r.length)r.mapped=refs_base_+r.offset;
   auto kh=index_hash(k,key);require(shard_of(kh)==s,"snapshot shard");require(idx.put(key,kh,r),"snapshot duplicate");
  }
  require(p==e,"snapshot section");
 }
 // Builds the in-memory index from the mapped snapshot (before the first write can
 // overwrite the tail). Called with the writer lock held.
 void materialize(){
  if(!lazy_)return;
  if(!sections_ok()){rebuild_locked();return;}
  Sharded built;parallel_for(64,index_count_>=4096,[&](size_t s){parse_section(s,lazy_sec_[s],lazy_len_[s],key_,meta_.txid,built[s]);});
  std::array<std::unique_lock<ReadWriteMutex>,64> locks;for(size_t i=0;i<shards_.size();i++)locks[i]=std::unique_lock<ReadWriteMutex>(shards_[i].mutex);
  for(size_t i=0;i<shards_.size();i++)shards_[i].index=std::move(built[i]);
  lazy_=false;
 }
 // live: written while the store is open (read views exist), so the file is only extended.
 void write_index_snapshot(bool live=false){
  materialize();
  std::array<uint64_t,65> at{};std::array<uint32_t,64> slots{};at[0]=SNAP_HEAD+SNAP_TABLE;
  for(size_t i=0;i<shards_.size();i++){slots[i]=snap_slots(shards_[i].index.size());uint64_t bytes=8+8*uint64_t(slots[i]);shards_[i].index.for_each([&](std::string_view k,const Ref&){bytes+=2+k.size()+36;});at[i+1]=at[i]+bytes;}
  size_t n=size_t(at[64]+32);std::unique_ptr<unsigned char[]> buf(new unsigned char[n]);unsigned char*s=buf.get();
  memset(s,0,SNAP_HEAD+SNAP_TABLE);memcpy(s,"KDBIDX02",8);put64(s+8,meta_.generation);put64(s+16,meta_.txid);put64(s+24,meta_.end);
  memcpy(s+32,meta_.uuid.data(),16);memcpy(s+48,meta_.chain.data(),32);put64(s+80,index_count_);put64(s+88,n);
  put64(s+96,key_.sip[0]);put64(s+104,key_.sip[1]);memcpy(s+112,key_.blake,32);put64(s+144,key_.index_seed);
  // Every shard's section is filled and hashed in parallel.
  parallel_for(shards_.size(),index_count_>=4096,[&](size_t i){
   unsigned char*sec=s+at[i];uint32_t mask=slots[i]-1;put32(sec,uint32_t(shards_[i].index.size()));put32(sec+4,slots[i]);memset(sec+8,0,8*size_t(slots[i]));
   unsigned char*p=sec+8+8*size_t(slots[i]);
   shards_[i].index.for_each([&](std::string_view k,const Ref&r){uint64_t kh=index_hash(key_,k);
    uint32_t j=uint32_t(kh)&mask;while(get64(sec+8+8*size_t(j)))j=(j+1)&mask;put64(sec+8+8*size_t(j),(uint64_t(uint32_t(kh))<<32)|uint64_t(p-sec));
    p[0]=(unsigned char)(k.size()&255);p[1]=(unsigned char)(k.size()>>8);memcpy(p+2,k.data(),k.size());p+=2+k.size();
    put64(p,r.offset);put32(p+8,r.length);put64(p+12,r.version);memcpy(p+20,r.hash.data(),16);p+=36;});
   unsigned char*t=s+SNAP_HEAD+SNAP_ENTRY*i;put64(t,at[i]);put64(t+8,at[i+1]-at[i]);
   auto sh=blake3_digest(sec,size_t(at[i+1]-at[i]));memcpy(t+16,sh.data(),32);
  });
  auto h=blake3_digest(s,SNAP_HEAD+SNAP_TABLE);memcpy(s+n-32,h.data(),32);
  // A cache, not data: no write-through and no flush. A torn or lost snapshot just
  // fails its checks on the next open, which then replays the log.
  // While open, a plain positioned write: a writable mapping would disturb the pages
  // behind the read views. On close no views remain: one size change (dropping any
  // reserve past the snapshot), then a mapped write, which skips the write-through
  // handle's synchronous disk write.
  if(live)write_at(fd_.n,s,n,meta_.end);
  else{if(file_size(fd_.n)!=meta_.end+n)platform::truncate(fd_.n,meta_.end+n);platform::write_cached(fd_.n,s,n,meta_.end);}
  snapshot_bytes_=n;
 }
 // Opens the index from a snapshot matching the committed state: lazily (probing the
 // mapped hash tables) when that view will become the read view, otherwise by parsing
 // it. Returns false, leaving the index empty, when there is none or any check fails.
 bool load_index_snapshot(uint64_t fsize,std::unique_ptr<Mapping>&view){
  try{
   if(fsize<meta_.end+SNAP_HEAD+SNAP_TABLE+32||fsize-meta_.end>(uint64_t(1)<<40))return false;
   auto head=read_at(fd_.n,SNAP_HEAD,meta_.end);const unsigned char*hp=head.data();
   if(memcmp(hp,"KDBIDX02",8)||get64(hp+16)!=meta_.txid||get64(hp+24)!=meta_.end||memcmp(hp+32,meta_.uuid.data(),16)||memcmp(hp+48,meta_.chain.data(),32)||get64(hp+88)>fsize-meta_.end||get64(hp+88)<SNAP_HEAD+SNAP_TABLE+32)return false;
   // Bytes after the snapshot (an older reserve) are ignored.
   size_t n=size_t(get64(hp+88));Bytes copy;const unsigned char*sp;
   view=std::make_unique<Mapping>(fd_.n,0,size_t(fsize));
   if(view->address){platform::prefetch(static_cast<const char*>(view->address)+meta_.end,n);sp=static_cast<const unsigned char*>(view->address)+meta_.end;}
   else{view.reset();copy=read_at(fd_.n,n,meta_.end);sp=copy.data();}
   auto h=blake3_digest(sp,SNAP_HEAD+SNAP_TABLE);if(memcmp(h.data(),sp+n-32,32))return false;
   std::array<uint64_t,64> off{},len{};uint64_t expect=SNAP_HEAD+SNAP_TABLE;
   for(size_t i=0;i<64;i++){off[i]=get64(sp+SNAP_HEAD+SNAP_ENTRY*i);len[i]=get64(sp+SNAP_HEAD+SNAP_ENTRY*i+8);if(off[i]!=expect||len[i]<8||len[i]>n-32-off[i])return false;expect+=len[i];}
   if(expect!=n-32)return false;
   DigestKey k{};k.sip[0]=get64(sp+96);k.sip[1]=get64(sp+104);memcpy(k.blake,sp+112,32);k.index_seed=get64(sp+144);
   // Lazy when this view becomes the read view (see the constructor): it stays mapped.
   bool lazy=options_.reads==ReadMode::Mapped&&view&&view->address&&fsize<=MAP_CHUNK+16*1024*1024;
   refs_base_=lazy?static_cast<const char*>(view->address):nullptr;
   std::array<uint64_t,64> counts{};
   parallel_for(64,!lazy&&n>=(uint64_t(64)<<10),[&](size_t i){
    const unsigned char*sec=sp+off[i];
    uint32_t count=get32(sec),slots=get32(sec+4);require(slots>=4&&!(slots&(slots-1))&&count<=slots&&8+8*uint64_t(slots)<=len[i],"snapshot section");
    // Lazy: each section's hash is checked when it is first used (section_ok).
    if(lazy){memcpy(sec_hash_[i].data(),sp+SNAP_HEAD+SNAP_ENTRY*i+16,32);sec_state_[i].store(0);}
    else{auto sh=blake3_digest(sec,size_t(len[i]));require(!memcmp(sh.data(),sp+SNAP_HEAD+SNAP_ENTRY*i+16,32),"snapshot section hash");parse_section(i,sec,len[i],k,meta_.txid,shards_[i].index);}
    counts[i]=count;});
   uint64_t sum=0;for(auto c:counts)sum+=c;
   if(sum!=get64(sp+80)){for(auto&sh:shards_)sh.index.clear();refs_base_=nullptr;return false;}
   if(lazy){for(size_t i=0;i<64;i++){lazy_sec_[i]=sp+off[i];lazy_len_[i]=len[i];}lazy_=true;}
   for(size_t i=0;i<64;i++)live_[i]=size_t(counts[i]);
   key_=k;index_count_=size_t(sum);snapshot_bytes_=n;return true;
  }catch(...){}
  for(auto&sh:shards_)sh.index.clear();lazy_=false;refs_base_=nullptr;
  return false;
 }
 // Writes the index snapshot once commits have been quiet for snapshotIdleMs, so a
 // later close has nothing left to write and, after a crash, the next open can still
 // skip the replay (provided nothing was committed after the checkpoint).
 void maintain(){
  std::unique_lock<std::mutex>l(mt_m_);
  const auto quiet=std::chrono::milliseconds(options_.snapshotIdleMs);
  for(;;){
   mt_cv_.wait(l,[&]{return mt_stop_||mt_dirty_||mt_merge_;});
   if(mt_stop_)return;
   // Paged index: merges (and rebuilds) as soon as asked, not after commits go quiet.
   if(mt_merge_){mt_merge_=false;l.unlock();try{pg_run();}catch(...){}l.lock();continue;}
   while(!mt_stop_&&!mt_merge_&&std::chrono::steady_clock::now()<last_commit_+quiet)mt_cv_.wait_until(l,last_commit_+quiet);
   if(mt_stop_)return;
   if(mt_merge_)continue;
   mt_dirty_=false;l.unlock();
   try{
    std::lock_guard<std::mutex>sc(sc_m_);
    if(paged_)pg_checkpoint();
    else{std::lock_guard<std::mutex>w(writer_);if(!failed_&&verify_state_!=3&&!snapshot_intact_){write_index_snapshot(true);snapshot_intact_=true;}}
   }catch(...){}
   l.lock();
  }
 }
 // base: the store's read view when it already maps [0, m.end); otherwise a view of its own.
 void background_verify(Meta m,const unsigned char*base=nullptr){
  try{
   std::unique_ptr<Mapping> own;const unsigned char*b=base;
   if(!b){own=std::make_unique<Mapping>(fd_.n,0,size_t(m.end));b=static_cast<const unsigned char*>(own->address);}
   Bytes buf;
   auto bytes=[&](uint64_t off,uint64_t n)->const unsigned char*{if(b)return b+off;buf=read_at(fd_.n,size_t(n),off);return buf.data();};
   uint64_t pos=START,tx=0;Hash prev{};bool first=true;
   while(pos<m.end){
    if(stop_verify_)return;
    require(m.end-pos>=96,"CORRUPT short frame");const unsigned char*head=bytes(pos,64);require(is_group_magic(head),"CORRUPT group magic");
    auto len=get64(head+8),seq=get64(head+16);uint32_t flags=get32(head+24),count=get32(head+28);
    require(flags<=1&&(!flags||first),"CORRUPT checkpoint location");require(flags?seq>=tx:seq==tx+1,"CORRUPT transaction sequence");
    require((flags||len<=MAX_GROUP)&&len<=m.end-pos-96,"CORRUPT frame length");require(flags||count<=1000000,"CORRUPT count");require(!memcmp(head+32,prev.data(),32),"CORRUPT chain");
    const unsigned char*whole=bytes(pos,96+len);auto h=digest(whole,size_t(64+len));require(!memcmp(h.data(),whole+64+len,32),flags?"CORRUPT checkpoint hash":"CORRUPT frame hash");
    pos+=96+len;tx=seq;prev=h;first=false;
   }
   require(tx==m.txid&&prev==m.chain,"CORRUPT final boundary");
   {std::lock_guard<std::mutex>w(writer_);if(lazy_&&!sections_ok())rebuild_locked();}
   // Paged index: every block of the on-disk tables (values are checked on every read).
   if(paged_){pg_verify_blocks();if(!stop_verify_)verify_state_.store(2,std::memory_order_release);return;}
   for(auto&sh:shards_){
    if(stop_verify_)return;
    std::shared_lock<ReadWriteMutex>r(sh.mutex);
    for_each_live(size_t(&sh-shards_.data()),[&](std::string_view,const Ref&ref,uint64_t){if(ref.offset+ref.length<=m.end)require(value_matches(key_,bytes(ref.offset,ref.length),ref.length,ref.hash),"CORRUPT value on disk");});
   }
   verify_state_.store(2,std::memory_order_release);
  }catch(const std::exception&e){fault_=std::string("CORRUPT found by background verification: ")+e.what();verify_state_.store(3,std::memory_order_release);}
  catch(...){fault_="CORRUPT found by background verification";verify_state_.store(3,std::memory_order_release);}
 }
 static std::string temp_name(const std::string&p){static std::mutex m;static uint64_t id=0;std::lock_guard<std::mutex>g(m);return p+".kdb-tmp-"+std::to_string(platform::process_id())+"-"+std::to_string(++id);}
 static void publish(const std::string&temp,const std::string&dest){platform::publish(temp,dest);}
 // Point reads copy the value (from the mapping, or with pread) while they hold their shard
 // lock: a compaction, which takes every lock, can then unmap and close the old file at once.
 // The checksum is checked after the lock is released.
 bool copy_mapped(const Ref&ref,std::string&out)const{
  if(const char*m=ref.mapped?ref.mapped:addr_of(ref.offset,ref.length)){out.assign(m,ref.length);return true;}
  out.resize(ref.length);if(ref.length)platform::read_exact(read_fd(),out.data(),ref.length,ref.offset);return true;
 }
 Value finish_ref(const Ref&ref,std::string&&value,bool copied)const{
  if(!copied){value.resize(ref.length);platform::read_exact(read_fd(),value.data(),ref.length,ref.offset);}
  require(value_matches(key_,value.data(),value.size(),ref.hash),"CORRUPT value on disk");return {true,ref.version,std::move(value)};
 }
 Value load_ref(const Ref&ref)const{
  std::string value;
  const char*m=ref.mapped?ref.mapped:addr_of(ref.offset,ref.length);
  if(m)value.assign(m,ref.length);
  else {value.resize(ref.length);platform::read_exact(read_fd(),value.data(),ref.length,ref.offset);}
  require(value_matches(key_,value.data(),value.size(),ref.hash),"CORRUPT value on disk");return {true,ref.version,std::move(value)};
 }
public:
 explicit Store(const std::string&path,bool create=false,Options options={}):path_(path),options_(options){
  require(options_.reads!=ReadMode::Mapped||platform::mapped_supported,"UNSUPPORTED mapped reads on this platform; use pread or auto");
  // A crash in the middle of a compaction's switch (Windows) can leave only the old file.
  auto old_copies=[&]{std::vector<std::filesystem::path> v;auto f=std::filesystem::u8path(path);auto dir=f.parent_path().empty()?std::filesystem::path("."):f.parent_path();auto stem=f.filename().u8string()+".compacted-old-";
   std::error_code ec;for(const auto&e:std::filesystem::directory_iterator(dir,ec))if(e.path().filename().u8string().rfind(stem,0)==0)v.push_back(e.path());std::sort(v.begin(),v.end());return v;};
  if(!create&&!std::filesystem::exists(std::filesystem::u8path(path))){auto v=old_copies();if(!v.empty())std::filesystem::rename(v.back(),std::filesystem::u8path(path));}
  fd_.n=platform::open_file(path,create,true);rfd_.store(fd_.n,std::memory_order_release);
  // Left by a compaction: the temporary file, and old copies a crash (or, on Windows, a
  // file still in use) kept from being deleted. Those are listed only when the marker says
  // some may exist: listing the directory on every open costs time in busy directories.
  platform::remove_noexcept(path+".compacting");
  if(std::filesystem::exists(std::filesystem::u8path(path+".compacted-old"))){
   bool all=true;for(const auto&o:old_copies()){platform::remove_noexcept(o.u8string());std::error_code ec;all&=!std::filesystem::exists(o,ec);}
   if(all)platform::remove_noexcept(path+".compacted-old");
  }
  if(create){init(fd_.n);sync_parent(path);}
  {bool rewrite=false;meta_=recover_meta(fd_.n,rewrite);
   if(rewrite){meta_.generation++;auto h=meta_bytes(meta_);write_at(fd_.n,h.data(),h.size(),(meta_.generation%2)*4096);sync_file(fd_.n);}}
  for(auto ext:{".sets",".sort",".sorting"})platform::remove_noexcept(path_+ext);  // scratch files left by a crash
  auto size=file_size(fd_.n);
  // Paged index file: deltas loaded, the log replayed from where each shard's table ends.
  if(!create&&!ignore_index_snapshot&&pg_open()){
   if(size>meta_.end){platform::truncate(fd_.n,meta_.end);sync_file(fd_.n);}
   reserved_=meta_.end;verify_state_=1;
   if(options_.verifyOnOpen)background_verify(meta_);
   else verify_task_=Background::get().schedule(std::chrono::milliseconds(options_.verifyDelayMs),[this,m=meta_]{background_verify(m);});
   if(verify_state_==3)throw std::runtime_error(fault_);
   map_all();
   if(options_.indexMemory){size_t mem=0;for(const auto&sh:shards_)mem+=sh.index.approx_memory();if(mem>options_.indexMemory)wake(true);}
   return;
  }
  std::unique_ptr<Mapping> snapview;
  bool snapshot_ok=!create&&size>meta_.end&&!ignore_index_snapshot&&load_index_snapshot(size,snapview);
  if(!snapshot_ok)snapview.reset();  // a mapped file cannot be truncated on Windows
  if(snapshot_ok){
   // Opened from the index snapshot. It stays in the tail until the first commit's
   // reserve overwrites it; verification of the log continues in the background.
   from_snapshot_=snapshot_intact_=true;reserved_=meta_.end;
   // The mapping the snapshot was parsed from covers the data too: keep it as the read
   // view. The verifier maps the file itself: touching every page through the read view
   // would make that view (unmapped on close) expensive to tear down.
   const unsigned char*base=nullptr;
   if(options_.reads==ReadMode::Mapped&&snapview&&snapview->address&&size<=MAP_CHUNK+16*1024*1024){base=static_cast<const unsigned char*>(snapview->address);last_view_=snapview.get();last_base_=0;publish_view(0,snapview.get());maps_.emplace(0,std::move(snapview));}
   snapview.reset();
   verify_state_=1;
   if(options_.verifyOnOpen)background_verify(meta_);
   // Starts after verifyDelayMs on the shared background thread, so a database that is
   // closed again sooner does not pay for starting and stopping it.
   else verify_task_=Background::get().schedule(std::chrono::milliseconds(options_.verifyDelayMs),[this,m=meta_]{background_verify(m);});
   if(verify_state_==3)throw std::runtime_error(fault_);
   if(!base)map_all();  // otherwise the snapshot's view covers the file
   require(!lazy_||base,"internal: lazy index without its view");
   return;
  }
  platform::random_bytes(&key_,sizeof key_);
  std::unique_ptr<Mapping> view;
  {Sharded recovered;replay(fd_.n,meta_,recovered,key_,options_.reads==ReadMode::Mapped?&view:nullptr);index_count_=total(recovered);for(size_t i=0;i<shards_.size();i++){live_[i]=recovered[i].size();shards_[i].index=std::move(recovered[i]);}}
  // A mapped file cannot be truncated on Windows, so drop the view before removing a tail.
  if(file_size(fd_.n)>meta_.end){view.reset();platform::truncate(fd_.n,meta_.end);sync_file(fd_.n);}
  reserved_=meta_.end;
  // Reuse the replay's view as the read view for the first window: same bytes, already mapped.
  if(view&&view->address&&meta_.end<=MAP_CHUNK+16*1024*1024){last_view_=view.get();last_base_=0;publish_view(0,view.get());maps_.emplace(0,std::move(view));}
  view.reset();
  map_all();
 }
 // Best effort: unmap, then drop the zero-filled reserve so a closed file ends at its
 // committed boundary. If this fails or is skipped, the next open truncates the tail.
 ~Store(){
  // Last: the file a compaction replaced (open and mapped until now).
  struct OldFile{Store&s;~OldFile(){
   s.old_files_.clear();bool all=true;
   for(const auto&n:s.old_names_){platform::remove_noexcept(n);std::error_code ec;all&=!std::filesystem::exists(std::filesystem::u8path(n),ec);}
   if(!s.old_names_.empty()&&all)platform::remove_noexcept(s.path_+".compacted-old");  // nothing left for the next open to clean
  }}old_file{*this};
  {std::lock_guard<std::mutex>l(mt_m_);mt_stop_=true;}mt_cv_.notify_all();if(maintainer_.joinable())maintainer_.join();
  stop_verify_=true;Background::cancel_or_wait(verify_task_);
  if(platform::valid(slot_fd_.n)&&!failed_){try{sync_file(slot_fd_.n);}catch(...){}}
  try{
   maps_.clear();retired_.clear();last_view_=nullptr;
   if(failed_||verify_state_==3||fd_.n==platform::invalid_file())return;
   // Both slots at the committed end: a clean close leaves no newer slot that recovery
   // could set aside (see recover_meta), so damage to the last commit is reported, not
   // taken for an interrupted one. (The index snapshot matches by txid, end and chain.)
   {Meta other;bool same=false;try{auto o=read_at(fd_.n,4096,((meta_.generation+1)%2)*4096);same=decode_meta(o.data(),other)&&other.end==meta_.end&&other.txid==meta_.txid;}catch(...){}
    if(!same){meta_.generation++;auto h=meta_bytes(meta_);write_at(fd_.n,h.data(),h.size(),(meta_.generation%2)*4096);sync_file(fd_.n);}}
   // Paged index: record the in-memory changes in the index file; no snapshot in the tail.
   if(paged_){
    {std::lock_guard<std::mutex>sc(sc_m_);try{pg_checkpoint();}catch(...){}}
    for(size_t s=0;s<64;s++)pg_drop_base(s);
    if(file_size(fd_.n)>meta_.end)platform::truncate(fd_.n,meta_.end);
    return;
   }
   // Leave an index snapshot for a fast next open; one already there is kept as is.
   // An intact snapshot is kept; reserve bytes after it are trimmed. A smaller file is
   // also cheaper for the OS to scan when it is opened again.
   if(snapshot_intact_){if(file_size(fd_.n)>meta_.end+snapshot_bytes_)platform::truncate(fd_.n,meta_.end+snapshot_bytes_);}
   else if(options_.indexSnapshot)write_index_snapshot();
   else if(file_size(fd_.n)>meta_.end)platform::truncate(fd_.n,meta_.end);
  }catch(...){}
 }
 // How the index was built, and the state of verification.
 const char*opened_from()const{return from_sidecar_?"index-file":from_snapshot_?"index-snapshot":"log-replay";}
 // "mapped": lookups still probe the snapshot in the file; "memory": the in-memory index.
 // "paged": the index lives in its file, RAM holds recent changes.
 const char*index_mode()const{std::shared_lock<ReadWriteMutex>r(shards_[0].mutex);return lazy_?"mapped":paged_?"paged":"memory";}
 const char*verification()const{switch(verify_state_.load()){case 1:return "running";case 2:return "passed";case 3:return "failed";default:return "at-open";}}
 // Tests and tools: waits for background verification to finish.
 void wait_for_verification(){Background::run_or_wait(verify_task_);}
 Store(const Store&)=delete;Store&operator=(const Store&)=delete;
 // values: the resulting value of each increment in the transaction, in order.
 struct Outcome {uint64_t version=0;std::exception_ptr error;std::vector<std::string> values;};
 // Group commit: independent transactions, in order, sharing one metadata update and
 // one flush. Each keeps its own frame, transaction ID and atomicity, and validates
 // against the effects of the accepted transactions before it. A transaction that
 // fails validation fails alone. Nothing is visible or acknowledged before the
 // shared flush succeeds; an I/O failure fails every accepted transaction.
 // The file format already permits several frames between metadata updates.
 // Waits while a SQL transaction holds the write gate (see store_sql.inc).
 std::vector<Outcome> commit_group(const std::vector<const std::vector<Op>*>&txs){
  char tok;GateHold gate(*this,&tok);
  std::lock_guard<std::mutex>w(writer_);return commit_group_locked(txs);
 }
 // internal: the engine's own records (reserved keys starting with byte 0x01).
 // overlap(io_done): runs while the group's data is being written (after validation,
 // before it is durable or visible); io_done() tells whether the write has finished.
 // Not called if nothing is written.
 std::vector<Outcome> commit_group_locked(const std::vector<const std::vector<Op>*>&txs,bool internal=false,const std::function<void(const std::function<bool()>&)>*overlap=nullptr){
  {std::shared_lock<ReadWriteMutex>r(readers_);healthy();}
  materialize();  // the commit will overwrite the snapshot in the tail
  load_catalog_locked();
  auto timer=commit_observer;std::chrono::steady_clock::time_point began;if(timer)began=std::chrono::steady_clock::now();  // only when observed (kestro-server --metrics)
  std::vector<Outcome> out(txs.size());std::vector<size_t> accepted;accepted.reserve(txs.size());
  // One buffer for every group, one array of index references (ref_at[i] is where the
  // i-th accepted transaction's references start), both sized up front.
  size_t total_ops=0;uint64_t estimate=0;bool cas=false,incr=false,indexed=false;
  for(const auto*t:txs){total_ops+=t->size();estimate+=96;for(const auto&o:*t){estimate+=17+o.key.size()+o.value.size();cas|=o.expected!=ANY;incr|=o.incr;if(!indexed&&!sec_.empty()&&has_index(collection_of(o.key)))indexed=true;}}
  // Unique indexes on written collections must exist before their checks run.
  if(indexed)for(auto&ix:sec_)if(ix->unique&&!ix->built){bool hit=false;for(const auto*t:txs)for(const auto&o:*t)if(collection_of(o.key)==ix->collection)hit=true;if(hit)build_index_locked(*ix);}
  std::vector<Ref> refs;refs.reserve(total_ops);std::vector<size_t> ref_at;ref_at.reserve(txs.size());
  Meta next=meta_;Bytes frames;frames.reserve(size_t(std::min<uint64_t>(estimate,4*MAX_GROUP)));
  // Versions set by earlier transactions of this group (0 = deleted). Only compare-and-swap
  // checks read them, so they are tracked only when the group contains one.
  std::unordered_map<std::string_view,uint64_t> pending;bool track=cas&&txs.size()>1;if(track)pending.reserve(total_ops);
  // Values set by earlier transactions of this group, for increments and index upkeep.
  // Only needed across transactions of one group (a transaction cannot repeat a key).
  bool track_values=(incr||indexed)&&txs.size()>1;std::unordered_map<std::string_view,std::optional<std::string>> pv;
  auto current=[&](std::string_view k)->std::optional<std::string>{if(track_values){auto it=pv.find(k);if(it!=pv.end())return it->second;}return current_value(k);};
  std::deque<std::vector<Op>> resolved;std::vector<const std::vector<Op>*> effective(txs.size());
  std::vector<std::vector<IndexDelta>> deltas(txs.size());std::unordered_map<SecIndex*,UniqueGroup> unique_groups;
  for(size_t t=0;t<txs.size();t++){
   try{
    const auto&given=*txs[t];
    require(!given.empty()&&given.size()<=65536,"LIMIT transaction operation count");require(next.txid<UINT64_MAX-1&&next.generation<UINT64_MAX-1,"LIMIT revision");
    if(given.size()>1&&given.size()<=16){for(size_t i=1;i<given.size();i++)for(size_t j=0;j<i;j++)require(given[i].key!=given[j].key,"INPUT duplicate transaction key");}
    else if(given.size()>16){std::unordered_set<std::string_view> seen;seen.reserve(given.size());for(const auto&o:given)require(seen.insert(o.key).second,"INPUT duplicate transaction key");}
    if(!internal)for(const auto&o:given)require(o.key.empty()||uint8_t(o.key[0])!=1,"INPUT keys starting with byte 0x01 are reserved");
    // Increments become ordinary sets of the computed value; the log stores the result.
    const std::vector<Op>*use=&given;
    bool has_incr=false;for(const auto&o:given)has_incr|=o.incr;
    if(has_incr){
     std::vector<Op> copy=given;
     for(auto&o:copy)if(o.incr){require(!o.erase,"INPUT increment cannot delete");o.value=add_number(current(o.key),o.value);o.incr=false;out[t].values.push_back(o.value);}
     resolved.push_back(std::move(copy));use=&resolved.back();
    }
    const auto&ops=*use;
    if(cas)for(const auto&o:ops){if(o.expected==ANY)continue;
     uint64_t cur=0;auto q=track?pending.find(o.key):pending.end();
     if(q!=pending.end())cur=q->second;else{auto h=index_hash(key_,o.key);Ref f;if(lookup(shard_of(h),o.key,h,f))cur=f.version;}
     require(o.expected==cur,"CONFLICT version mismatch");}
    if(indexed)index_deltas(ops,current,deltas[t],unique_groups);
    uint64_t tx=next.txid+1;size_t at=frames.size();
    encode_into(frames,ops,next.chain,tx);
    if(frames.size()>4*MAX_GROUP){frames.resize(at);throw std::runtime_error("LIMIT group commit bytes");}
    ref_at.push_back(refs.size());prepare_refs(ops,tx,next.end,refs);
    next.txid=tx;next.end+=frames.size()-at;memcpy(next.chain.data(),frames.data()+frames.size()-32,32);
    if(track)for(const auto&o:ops)pending[o.key]=o.erase?0:tx;
    if(track_values)for(const auto&o:ops)pv[o.key]=o.erase?std::nullopt:std::optional<std::string>(o.value);
    if(indexed&&txs.size()>1)merge_unique(deltas[t],ops,unique_groups);
    effective[t]=use;accepted.push_back(t);out[t].version=tx;
   }catch(...){out[t].error=std::current_exception();out[t].values.clear();deltas[t].clear();}
  }
  if(accepted.empty())return out;
  // Paged index: whether each key is live below the delta, looked up before anything is
  // written (a damaged index file then fails the commit instead of the store).
  std::vector<uint8_t> lowers;
  if(paged_){
   try{lowers.resize(refs.size());
    for(size_t i=0;i<accepted.size();i++){size_t k=ref_at[i];for(const auto&o:*effective[accepted[i]]){auto h=index_hash(key_,o.key);size_t s=shard_of(h);Ref f;lowers[k++]=layered(s)&&lower_find(s,o.key,h,f);}}
   }catch(...){auto e=std::current_exception();for(auto t:accepted){out[t].version=0;out[t].error=e;out[t].values.clear();}return out;}
  }
  std::vector<Note> changes;if(nt_watch_.load(std::memory_order_relaxed))change_notes_locked(accepted,effective,out,changes);  // LISTEN table_changes:...
  next.generation++;Bytes header;
  try{
   snapshot_intact_=false;reserve(next.end);
   for(auto&ref:refs)map_ref(ref);
   // Early install (write-through platforms, no overlap): the commit is installed while its
   // group is being written, with readers_ and every touched shard locked until the write is
   // durable, so nothing is visible before; a failure marks the store failed before any lock
   // is released. Shard locks are taken in shard order, as readers take them.
   std::unique_lock<ReadWriteMutex> early_r;std::array<std::unique_lock<ReadWriteMutex>,64> held;bool early=false;
   auto release_early=[&]{for(auto&l:held)if(l.owns_lock())l.unlock();if(early_r.owns_lock())early_r.unlock();};
   {platform::PendingWrite pw;platform::write_begin(pw,fd_.n,frames.data(),frames.size(),meta_.end);
    header=meta_bytes(next);  // computed while the group is being written
    if(overlap){try{(*overlap)([&pw]{return platform::write_done(pw);});}catch(...){}}
    else if(platform::write_through){
     early=true;install_waiting_.fetch_add(1,std::memory_order_relaxed);try{early_r=std::unique_lock<ReadWriteMutex>(readers_);}catch(...){install_waiting_.fetch_sub(1);throw;}install_waiting_.fetch_sub(1,std::memory_order_relaxed);
     {uint64_t at=meta_.end;for(auto t:accepted)record_undo(*effective[t],at);}  // snapshots: before the shards are locked (lookup)
     try{
      std::array<bool,64> touched{};for(auto t:accepted)for(const auto&o:*effective[t])touched[shard_of(index_hash(key_,o.key))]=true;
      for(size_t i=0;i<shards_.size();i++)if(touched[i])held[i]=std::unique_lock<ReadWriteMutex>(shards_[i].mutex);
      for(size_t i=0;i<accepted.size();i++){auto t=accepted[i];install(*effective[t],refs.data()+ref_at[i],lowers.empty()?nullptr:lowers.data()+ref_at[i],&held);apply_catalog(*effective[t],deltas[t]);}
      meta_=next;
     }catch(...){failed_=true;release_early();throw;}
    }
    try{platform::write_end(pw);}catch(...){if(early){failed_=true;release_early();}throw;}}
   platform::PendingWrite ph;
   try{
   // Write-through: each write is already durable when it returns, data before metadata.
   if(options_.orderedSync&&!platform::write_through){sync_file(fd_.n);syncs_++;}
#ifdef KESTRO_TEST_FAULTS
   if(const char*s=getenv("KESTRO_FAULT")){if(!strcmp(s,"after_data"))platform::exit_now(71);if(!strcmp(s,"torn_meta")){write_at(fd_.n,header.data(),512,(next.generation%2)*4096);sync_file(fd_.n);platform::exit_now(72);}if(!strcmp(s,"sync_error"))throw std::runtime_error("injected sync error");}
#endif
   // The group above is durable (write-through): it is the commit. The slot goes to the
   // cache (readers in other processes see it at once); recovery walks past a stale one.
   if(platform::write_through&&!options_.orderedSync&&!platform::valid(slot_fd_.n))slot_fd_.n=platform::open_file(path_,false,false);  // opened at the first commit
   platform::write_begin(ph,platform::valid(slot_fd_.n)?slot_fd_.n:fd_.n,header.data(),header.size(),(next.generation%2)*4096);
#ifdef KESTRO_TEST_FAULTS
   if(const char*s=getenv("KESTRO_FAULT")){if(!strcmp(s,"before_sync"))platform::exit_now(75);if(!strcmp(s,"final_sync_error"))throw std::runtime_error("injected final sync failure");}
#endif
   // The platform file flush covers both writes before publishing/acknowledging.
   // Without the earlier barrier an interrupted, unacknowledged commit can
   // leave metadata ahead of its data. Recovery rejects that state.
   if(!platform::write_through){sync_file(fd_.n);syncs_++;}
#ifdef KESTRO_TEST_FAULTS
   if(const char*s=getenv("KESTRO_FAULT"))if(!strcmp(s,"after_meta"))platform::exit_now(73);
#endif
   }catch(...){if(early){failed_=true;release_early();}throw;}
   release_early();
   // Install transaction by transaction: readers see each one atomically, with its
   // ordered-key and secondary-index changes.
   if(!early){install_waiting_.fetch_add(1,std::memory_order_relaxed);std::unique_lock<ReadWriteMutex>r(readers_,std::defer_lock);try{r.lock();}catch(...){install_waiting_.fetch_sub(1);throw;}install_waiting_.fetch_sub(1,std::memory_order_relaxed);{uint64_t at=meta_.end;for(auto t:accepted)record_undo(*effective[t],at);}for(size_t i=0;i<accepted.size();i++){auto t=accepted[i];install(*effective[t],refs.data()+ref_at[i],lowers.empty()?nullptr:lowers.data()+ref_at[i]);try{apply_catalog(*effective[t],deltas[t]);}catch(...){failed_=true;throw;}}meta_=next;}
   publish_commit(next.end);  // replication waiters
   commit_groups_.store(commit_groups_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);commits_.store(commits_.load(std::memory_order_relaxed)+accepted.size(),std::memory_order_relaxed);  // one writer: the writer lock
   if(timer)timer(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count()));
   if(!changes.empty())emit_locked(changes);
   platform::write_end(ph);  // the slot write ran while the commit was installed
   if(options_.indexSnapshot&&options_.snapshotIdleMs){
    // The maintainer re-reads last_commit_ while it waits for quiet: wake it only when it
    // has nothing pending (not on every commit: a thread wake-up costs microseconds).
    bool idle;{std::lock_guard<std::mutex>l(mt_m_);last_commit_=std::chrono::steady_clock::now();idle=!mt_dirty_;}
    if(idle)wake(false);
   }
   // Over the index memory budget with a shard worth merging, or crash recovery would
   // replay too much of the log: hand the maintainer a merge round.
   if(options_.indexMemory){
    size_t mem=0;bool worth=false;
    for(size_t s=0;s<64;s++){const auto&ix=shards_[s].index;mem+=ix.approx_memory();worth|=ix.size()>=std::max<size_t>(64,size_t(pgb_[s].count/32));}
    if((mem>options_.indexMemory&&worth)||(paged_&&next.end-pg_min_end_.load()>(uint64_t(256)<<20)))wake(true);
   }
  }catch(...){
   {std::unique_lock<ReadWriteMutex>r(readers_);failed_=true;}
   auto e=std::current_exception();for(auto t:accepted){out[t].version=0;out[t].error=e;out[t].values.clear();}
  }
  return out;
 }
 uint64_t commit(const std::vector<Op>&ops){auto r=commit_group({&ops});if(r[0].error)std::rethrow_exception(r[0].error);return r[0].version;}
 // Point read into a caller-owned buffer, reused across calls to avoid an allocation
 // per read. The bytes are copied first and the copy is checksum-verified, exactly as
 // get() does. Returns false when the key is absent.
 bool read(std::string_view key,std::string&out,uint64_t&version)const{
  for(int again=0;;again++){uint64_t g=file_gen_.load(std::memory_order_acquire);
   try{return read_once(key,out,version);}catch(const std::runtime_error&){if(again>=3||g==file_gen_.load(std::memory_order_acquire))throw;}
  }
 }
 bool read_once(std::string_view key,std::string&out,uint64_t&version)const{
  Ref ref;auto h=index_hash(key_,key);
  bool copied=false;
  for(int attempt=0;;attempt++){try{const auto&shard=shards_[shard_of(h)];std::shared_lock<ReadWriteMutex>r(shard.mutex);healthy();if(!lookup(shard_of(h),key,h,ref))return false;copied=copy_mapped(ref,out);break;}catch(const SnapshotDamaged&){if(attempt)throw std::runtime_error("CORRUPT index rebuild");}rebuild_index();}
  if(!copied){out.resize(ref.length);platform::read_exact(read_fd(),out.data(),ref.length,ref.offset);}
  require(value_matches(key_,out.data(),out.size(),ref.hash),"CORRUPT value on disk");version=ref.version;return true;
 }
 Value get(const std::string&key)const{
  for(int again=0;;again++){uint64_t g=file_gen_.load(std::memory_order_acquire);
   try{return get_once(key);}catch(const std::runtime_error&){if(again>=3||g==file_gen_.load(std::memory_order_acquire))throw;}
  }
 }
 Value get_once(const std::string&key)const{
  Ref ref;auto h=index_hash(key_,key);
  std::string value;bool copied=false;
  for(int attempt=0;;attempt++){try{const auto&shard=shards_[shard_of(h)];std::shared_lock<ReadWriteMutex>r(shard.mutex);healthy();if(!lookup(shard_of(h),key,h,ref))return {};copied=copy_mapped(ref,value);break;}catch(const SnapshotDamaged&){if(attempt)throw std::runtime_error("CORRUPT index rebuild");}rebuild_index();}
  return finish_ref(ref,std::move(value),copied);
 }
 std::vector<Value> getMany(const std::vector<std::string>&keys)const{
  for(int again=0;;again++){uint64_t g=file_gen_.load(std::memory_order_acquire);
   try{return get_many_once(keys);}catch(const std::runtime_error&){if(again>=3||g==file_gen_.load(std::memory_order_acquire))throw;}
  }
 }
 std::vector<Value> get_many_once(const std::vector<std::string>&keys)const{
  require(keys.size()<=4096,"LIMIT getMany key count");std::vector<Ref> refs(keys.size());std::vector<std::string> vals;std::vector<char> copied;
  for(int attempt=0;;attempt++)try{std::fill(refs.begin(),refs.end(),Ref{});std::array<bool,64> touched{};std::vector<uint64_t> hs(keys.size());for(size_t i=0;i<keys.size();i++){const auto&k=keys[i];require(!k.empty()&&k.size()<=1024,"LIMIT key");hs[i]=index_hash(key_,k);touched[shard_of(hs[i])]=true;}
   std::array<std::shared_lock<ReadWriteMutex>,64> locks;
   for(size_t i=0;i<64;i++)if(touched[i])locks[i]=std::shared_lock<ReadWriteMutex>(shards_[i].mutex);
   healthy();uint64_t bytes=5+13*keys.size();
   for(size_t i=0;i<keys.size();i++){Ref f;if(lookup(shard_of(hs[i]),keys[i],hs[i],f)){refs[i]=f;bytes+=f.length;}}
   require(bytes<=MAX_GROUP,"LIMIT getMany response bytes");
   vals.assign(keys.size(),std::string());copied.assign(keys.size(),0);
   for(size_t i=0;i<keys.size();i++)if(refs[i].version)copied[i]=copy_mapped(refs[i],vals[i]);
   break;
  }catch(const SnapshotDamaged&){if(attempt)throw std::runtime_error("CORRUPT index rebuild");rebuild_index();}
  std::vector<Value> result;result.reserve(keys.size());for(size_t i=0;i<refs.size();i++)result.push_back(refs[i].version?finish_ref(refs[i],std::move(vals[i]),copied[i]!=0):Value{});return result;
 }
 // File flushes made by commits so far (Windows write-through commits need none).
 uint64_t flushes()const{return syncs_.load(std::memory_order_relaxed);}
 // Transactions and commit groups made durable so far (replicated ones included).
 uint64_t commits()const{return commits_.load(std::memory_order_relaxed);}
 uint64_t commit_groups()const{return commit_groups_.load(std::memory_order_relaxed);}
 // Called with each commit group's duration in nanoseconds (under the writer lock); set before
 // any commit. Null (the default): no clock is read.
 void(*commit_observer)(uint64_t)=nullptr;
 uint64_t count()const{std::shared_lock<ReadWriteMutex>r(readers_);healthy();return index_count_;}
 uint64_t bytes()const{std::shared_lock<ReadWriteMutex>r(readers_);healthy();return meta_.end;}
 uint64_t txid()const{std::shared_lock<ReadWriteMutex>r(readers_);healthy();return meta_.txid;}
 void verify()const{std::lock_guard<std::mutex>w(writer_);healthy();if(lazy_&&!sections_ok())rebuild_locked();auto disk=read_meta(fd_.n);require(disk.end==meta_.end&&disk.chain==meta_.chain,"CORRUPT disk metadata changed");Sharded checked;replay(fd_.n,disk,checked,key_);require(total(checked)==index_count_,"CORRUPT index count");for(const auto&part:checked)part.for_each([&](std::string_view k,const Ref&e){uint64_t h=index_hash(key_,k);Ref f;bool found=lookup(shard_of(h),k,h,f);require(found&&f.offset==e.offset&&f.version==e.version&&f.length==e.length&&digest_eq(f.hash,e.hash),"CORRUPT index reference");});}
 // compact=false: the online backup (see store_backup.inc); compact=true: a copy holding only current values.
 void snapshot(const std::string&dest,bool compact=false)const{
  if(!compact){backup(dest);return;}
  std::lock_guard<std::mutex>w(writer_);healthy();if(lazy_&&!sections_ok())rebuild_locked();auto temp=temp_name(dest);bool created=false;
  try{
   FD out(platform::open_file(temp,true));created=true;Meta m=meta_;
   if(compact){
    require(index_count_<=UINT32_MAX,"LIMIT checkpoint key count");uint64_t size=0;
    for(size_t si=0;si<shards_.size();si++)for_each_live(si,[&](std::string_view k,const Ref&r,uint64_t){size+=17+k.size()+r.length;});
    require(size<(uint64_t(1)<<60)-START-96,"LIMIT checkpoint size");
    std::array<unsigned char,64> head{};memcpy(head.data(),MAGIC_GROUP,8);put64(head.data()+8,size);put64(head.data()+16,meta_.txid);put32(head.data()+24,1);put32(head.data()+28,index_count_);
    FrameWriter writer(out.n,START);writer.add(head.data(),head.size());
    for(size_t si=0;si<shards_.size();si++)for_each_live(si,[&](std::string_view k,const Ref&r,uint64_t){auto value=load_ref(r);unsigned char op[17]{};op[0]=1;put64(op+1,r.version);put32(op+9,uint32_t(k.size()));put32(op+13,uint32_t(value.value.size()));writer.add(op,sizeof(op));writer.add(k.data(),k.size());writer.add(value.value.data(),value.value.size());});
    m.chain=writer.finish();write_at(out.n,m.chain.data(),32,START+64+size);m.generation=1;m.end=START+96+size;
    Bytes headers(START);auto h=meta_bytes(m);memcpy(headers.data()+4096,h.data(),4096);write_at(out.n,headers.data(),headers.size(),0);
   }else{
    Bytes headers(START);auto h=meta_bytes(m);memcpy(headers.data()+(m.generation%2)*4096,h.data(),4096);write_at(out.n,headers.data(),headers.size(),0);
    for(uint64_t off=START;off<meta_.end;){auto n=std::min<uint64_t>(1024*1024,meta_.end-off);auto b=read_at(fd_.n,n,off);write_at(out.n,b.data(),b.size(),off);off+=n;}
   }
   sync_file(out.n);Sharded verify;auto checked=read_meta(out.n);replay(out.n,checked,verify,key_);require(total(verify)==index_count_,"CORRUPT snapshot count");publish(temp,dest);created=false;
  }catch(...){if(created)platform::remove_noexcept(temp);throw;}
 }
#include "store_paged.inc"
#include "store_backup.inc"
#include "store_compact.inc"
#include "store_replica.inc"
#include "store_catalog.inc"
#include "store_mvcc.inc"
#include "store_notify.inc"
#include "store_sql.inc"
#include "store_fulltext.inc"
#include "store_rules.inc"
};
}

#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <fcntl.h>
#else
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <cerrno>
#endif

namespace kestro {
inline void require(bool ok,const char*message){if(!ok)throw std::runtime_error(message);}
inline void require(bool ok,const std::string&message){if(!ok)throw std::runtime_error(message);}
namespace platform {
// I/O recording, for test builds only (KESTRO_IO_RECORDER, src/tests_powerloss.cpp): every
// open, write, write completion, flush and truncation of a file is reported, so a test can
// rebuild what a sudden power loss could leave on disk. Without the define the hooks are
// empty inline functions.
enum IoKind{IoOpen=1,IoWrite,IoWriteCached,IoComplete,IoSync,IoTruncate};
struct IoEvent{int kind=0;uint64_t file=0,id=0,off=0;std::string data;bool through=false;std::string path;};
#ifdef KESTRO_IO_RECORDER
}  // namespace platform
}  // namespace kestro
#include <atomic>
#include <functional>
namespace kestro {
namespace platform {
inline std::function<void(const IoEvent&)>&io_recorder(){static std::function<void(const IoEvent&)> r;return r;}
inline uint64_t io_emit(int kind,uint64_t file,uint64_t off=0,const void*p=nullptr,size_t n=0,bool through=false,const std::string*path=nullptr,uint64_t id=0){
 auto&r=io_recorder();if(!r)return 0;static std::atomic<uint64_t> ids{0};
 IoEvent e;e.kind=kind;e.file=file;e.off=off;e.through=through;e.id=id?id:++ids;if(p)e.data.assign(static_cast<const char*>(p),n);if(path)e.path=*path;r(e);return e.id;
}
#else
inline uint64_t io_emit(int,uint64_t,uint64_t=0,const void* =nullptr,size_t=0,bool=false,const std::string* =nullptr,uint64_t=0){return 0;}
#endif
#ifdef _WIN32
using File=HANDLE;
inline uint64_t io_h(File f){return uint64_t(reinterpret_cast<uintptr_t>(f));}
inline File invalid_file(){return INVALID_HANDLE_VALUE;}
inline bool valid(File f){return f!=INVALID_HANDLE_VALUE&&f!=nullptr;}
[[noreturn]] inline void error(const char*operation,DWORD code=GetLastError()){
 throw std::runtime_error(std::string("IO ")+operation+" (Windows error "+std::to_string(code)+")");
}
inline std::wstring wide(const std::string&s){
 require(!s.empty()&&s.find('\0')==std::string::npos&&s.size()<INT_MAX,"INPUT invalid path");
 int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);require(n>0,"INPUT path must be valid UTF-8");
 std::wstring out(n,L'\0');require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),out.data(),n)==n,"INPUT UTF-8 path conversion");return out;
}
inline std::string utf8(const std::wstring&s){
 if(s.empty())return {};require(s.size()<INT_MAX,"INPUT path too long");int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);require(n>0,"INPUT invalid UTF-16");
 std::string out(n,'\0');require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),out.data(),n,nullptr,nullptr)==n,"INPUT UTF-16 conversion");return out;
}
inline std::wstring path(const std::string&s){
 auto in=wide(s);std::replace(in.begin(),in.end(),L'/',L'\\');
 require(in.rfind(L"\\\\.\\",0)!=0,"INPUT device paths unsupported");
 if(in.rfind(L"\\\\?\\UNC\\",0)==0)in=L"\\\\"+in.substr(8);
 else if(in.rfind(L"\\\\?\\",0)==0)in=in.substr(4);
 for(size_t i=0;i<in.size();i++)require(in[i]!=L':'||(i==1&&((in[0]>=L'A'&&in[0]<=L'Z')||(in[0]>=L'a'&&in[0]<=L'z'))),"INPUT alternate data streams unsupported");
 DWORD n=GetFullPathNameW(in.c_str(),0,nullptr,nullptr);if(!n)error("GetFullPathNameW");require(n<32760,"INPUT path too long");std::wstring full(n,L'\0');DWORD got=GetFullPathNameW(in.c_str(),n,full.data(),nullptr);if(!got||got>=n)error("GetFullPathNameW");full.resize(got);
 return full.rfind(L"\\\\",0)==0?L"\\\\?\\UNC\\"+full.substr(2):L"\\\\?\\"+full;
}
inline void close(File f)noexcept{if(valid(f))CloseHandle(f);}
inline File open_file(const std::string&p,bool create=false,bool owner=false){
 auto w=path(p);File f=CreateFileW(w.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,create?CREATE_NEW:OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED|FILE_FLAG_OPEN_REPARSE_POINT|(owner?FILE_FLAG_WRITE_THROUGH:0),nullptr);
 if(!valid(f)){if(create&&GetLastError()==ERROR_FILE_EXISTS)throw std::runtime_error("INPUT "+p+" already exists (create makes a new file; open an existing one without it)");error("CreateFileW");}
 try{BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(f,&info))error("GetFileInformationByHandle");require(GetFileType(f)==FILE_TYPE_DISK&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"INPUT requires a regular non-reparse file");
  if(owner){OVERLAPPED lock{};lock.OffsetHigh=0x40000000; // ownership byte at 2^62, outside valid database offsets
   if(!LockFileEx(f,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&lock)){auto e=GetLastError();if(e==ERROR_IO_PENDING){CancelIoEx(f,&lock);DWORD ignored;GetOverlappedResult(f,&lock,&ignored,TRUE);throw std::runtime_error("LOCKED file has another owner");}if(e==ERROR_LOCK_VIOLATION)throw std::runtime_error("LOCKED file has another owner");error("LockFileEx",e);}}
 }catch(...){close(f);throw;}io_emit(IoOpen,io_h(f),0,nullptr,0,owner,&p);return f;
}
struct Event{HANDLE handle=CreateEventW(nullptr,TRUE,FALSE,nullptr);Event(){if(!handle)error("CreateEventW");}~Event(){CloseHandle(handle);}Event(const Event&)=delete;};
inline size_t io(File f,void*p,size_t n,uint64_t off,bool writing){
 require(off<(uint64_t(1)<<63)&&n<=(uint64_t(1)<<63)-off,"LIMIT file offset");if(!n)return 0;
 // ReadFile/WriteFile reset the OVERLAPPED event themselves when the I/O starts,
 // so no separate ResetEvent system call is needed on this hot path.
 thread_local Event event;OVERLAPPED op{};op.Offset=DWORD(off);op.OffsetHigh=DWORD(off>>32);op.hEvent=event.handle;
 DWORD amount=DWORD(std::min<size_t>(n,64*1024*1024)),done=0;
 BOOL ok=writing?WriteFile(f,p,amount,nullptr,&op):ReadFile(f,p,amount,nullptr,&op);
 if(!ok){auto code=GetLastError();if(!writing&&code==ERROR_HANDLE_EOF)return 0;if(code!=ERROR_IO_PENDING)error(writing?"WriteFile":"ReadFile",code);}
 if(!GetOverlappedResult(f,&op,&done,TRUE)){auto code=GetLastError();if(!writing&&code==ERROR_HANDLE_EOF)return 0;error("GetOverlappedResult",code);}return done;
}
inline size_t read_some(File f,void*p,size_t n,uint64_t off){return io(f,p,n,off,false);}
inline size_t write_some(File f,const void*p,size_t n,uint64_t off){return io(f,const_cast<void*>(p),n,off,true);}
inline uint64_t size(File f){LARGE_INTEGER n{};if(!GetFileSizeEx(f,&n))error("GetFileSizeEx");require(n.QuadPart>=0,"CORRUPT negative file size");return uint64_t(n.QuadPart);}
inline void truncate(File f,uint64_t n){require(n<(uint64_t(1)<<63),"LIMIT file size");FILE_END_OF_FILE_INFO info{};info.EndOfFile.QuadPart=LONGLONG(n);if(!SetFileInformationByHandle(f,FileEndOfFileInfo,&info,sizeof(info)))error("SetFileInformationByHandle");io_emit(IoTruncate,io_h(f),n);}
inline void sync(File f){if(!FlushFileBuffers(f))error("FlushFileBuffers");io_emit(IoSync,io_h(f));}
// Windows has no portable equivalent of POSIX directory fsync. File content
// is flushed above; publish() requests MOVEFILE_WRITE_THROUGH. Namespace
// persistence across actual power loss is explicitly not certified.
inline void sync_parent(const std::string&){}
inline void remove(const std::string&p){if(!DeleteFileW(path(p).c_str()))error("DeleteFileW");}
inline void remove_noexcept(const std::string&p)noexcept{try{DeleteFileW(path(p).c_str());}catch(...){}}
inline void remove_directory(const std::string&p){if(!RemoveDirectoryW(path(p).c_str()))error("RemoveDirectoryW");}
inline void publish(const std::string&temp,const std::string&dest){if(!MoveFileExW(path(temp).c_str(),path(dest).c_str(),MOVEFILE_WRITE_THROUGH))error("MoveFileExW");}
// file becomes keep (handles stay open), then temp becomes file. A crash between the two
// leaves keep alone; the next open renames it back.
inline void replace_keeping(const std::string&file,const std::string&temp,const std::string&keep){
 // Access denied is retried for up to ~2 s: antivirus scanners briefly open new files.
 auto move=[](const std::wstring&from,const std::wstring&to,DWORD flags)->DWORD{for(int i=0;;i++){if(MoveFileExW(from.c_str(),to.c_str(),flags))return 0;DWORD e=GetLastError();if((e!=ERROR_ACCESS_DENIED&&e!=ERROR_SHARING_VIOLATION)||i>=40)return e;Sleep(50);}};
 if(DWORD e=move(path(file),path(keep),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))error("MoveFileExW (database to .compacted-old)",e);
 if(DWORD e=move(path(temp),path(file),MOVEFILE_WRITE_THROUGH)){move(path(keep),path(file),MOVEFILE_WRITE_THROUGH);error("MoveFileExW (new file to database)",e);}
}
inline void hardlink(const std::string&source,const std::string&dest){if(!CreateHardLinkW(path(dest).c_str(),path(source).c_str(),nullptr))error("CreateHardLinkW");}
inline void random_bytes(void*p,size_t n){require(n<=ULONG_MAX,"LIMIT random size");if(BCryptGenRandom(nullptr,static_cast<PUCHAR>(p),ULONG(n),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("IO BCryptGenRandom");}
inline uint64_t process_id(){return GetCurrentProcessId();}
[[noreturn]] inline void exit_now(int code){ExitProcess(UINT(code));}
inline void binary_stdio(){if(_setmode(_fileno(stdin),_O_BINARY)<0||_setmode(_fileno(stdout),_O_BINARY)<0)throw std::runtime_error("IO binary standard streams");}
inline uint64_t size(File f);
// Writes [p, p+n) at off through a writable view of the file. Mapped writes go to the
// page cache even on a write-through handle, and need no second handle (which, on a
// file just modified, would cost an extra on-access scan when it closes).
// Extends the file if it ends before off+n; never shrinks it (shrinking is impossible
// while read views exist).
inline void write_cached(File f,const void*p,size_t n,uint64_t off){
 if(!n)return;
 SYSTEM_INFO si;GetSystemInfo(&si);uint64_t base=off/si.dwAllocationGranularity*si.dwAllocationGranularity;
 uint64_t need=std::max(size(f),off+n);
 HANDLE m=CreateFileMappingW(f,nullptr,PAGE_READWRITE,DWORD(need>>32),DWORD(need),nullptr);if(!m)error("CreateFileMappingW");
 void*v=MapViewOfFile(m,FILE_MAP_WRITE,DWORD(base>>32),DWORD(base),SIZE_T(off-base+n));DWORD code=GetLastError();CloseHandle(m);if(!v)error("MapViewOfFile",code);
 auto io=io_emit(IoWriteCached,io_h(f),off,p,n);memcpy(static_cast<char*>(v)+(off-base),p,n);UnmapViewOfFile(v);io_emit(IoComplete,io_h(f),0,nullptr,0,false,nullptr,io);
}
// Asks the OS to bring a mapped range in now: one call instead of a fault per page.
inline void prefetch(const void*p,size_t n)noexcept{WIN32_MEMORY_RANGE_ENTRY r{const_cast<void*>(p),n};PrefetchVirtualMemory(GetCurrentProcess(),1,&r,0);}
inline constexpr bool mapped_supported=true;
// A view must lie inside the file: the engine maps only reserved (already written) bytes.
inline constexpr bool mapping_beyond_eof=false;
inline constexpr const char* name="windows";
// The owning database handle is write-through: each write returns only once it is on
// stable media (forced unit access), so commits that overwrite preallocated bytes are
// durable without a FlushFileBuffers call. Write-through may persist a file-size change
// lazily, so the engine flushes once whenever it grows its zero-filled reserve.
inline constexpr bool write_through=true;
inline constexpr const char* sync_name="WRITE_THROUGH; FlushFileBuffers when the reserve grows";
inline constexpr const char* namespace_sync="MoveFileExW-write-through; no-directory-fsync";
class ReadWriteMutex{SRWLOCK lock_{};public:ReadWriteMutex(){InitializeSRWLock(&lock_);}ReadWriteMutex(const ReadWriteMutex&)=delete;ReadWriteMutex&operator=(const ReadWriteMutex&)=delete;void lock(){AcquireSRWLockExclusive(&lock_);}void unlock(){ReleaseSRWLockExclusive(&lock_);}void lock_shared(){AcquireSRWLockShared(&lock_);}void unlock_shared(){ReleaseSRWLockShared(&lock_);}};
// Read-only view of [off, off+n). Views of local files are coherent with WriteFile, so
// bytes written later into an already-mapped (preallocated) range are visible here.
// A failed mapping leaves address null and the engine falls back to positioned reads.
struct Mapping{void*address=nullptr;size_t length=0;
 Mapping(File f,uint64_t off,size_t n):length(n){HANDLE h=CreateFileMappingW(f,nullptr,PAGE_READONLY,0,0,nullptr);if(!h)return;address=MapViewOfFile(h,FILE_MAP_READ,DWORD(off>>32),DWORD(off),n);CloseHandle(h);}
 ~Mapping(){if(address)UnmapViewOfFile(address);}Mapping(const Mapping&)=delete;Mapping&operator=(const Mapping&)=delete;};
#else
using File=int;
inline uint64_t io_h(File f){return uint64_t(f);}
static_assert(sizeof(off_t)>=8,"KestroDB requires 64-bit file offsets");
inline File invalid_file(){return -1;}
inline bool valid(File f){return f>=0;}
[[noreturn]] inline void error(const char*operation){throw std::system_error(errno,std::generic_category(),operation);}
inline void close(File f)noexcept{if(valid(f))::close(f);}
inline File open_file(const std::string&p,bool create=false,bool owner=false){
 require(!p.empty()&&p.find('\0')==std::string::npos,"INPUT invalid path");int f=::open(p.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW|(create?(O_CREAT|O_EXCL):0),0600);if(f<0){if(create&&errno==EEXIST)throw std::runtime_error("INPUT "+p+" already exists (create makes a new file; open an existing one without it)");error("open");}
 try{struct stat st{};if(fstat(f,&st))error("fstat");require(S_ISREG(st.st_mode),"INPUT requires regular file");// A lock held by a process that was just killed can outlive it briefly: the kernel may release
 // a memory-mapped file's last reference from a worker (seen on Linux 6.18: 100-300 ms), so a
 // restart right after a crash retries for up to a second before reporting LOCKED.
 if(owner)for(int tries=0;flock(f,LOCK_EX|LOCK_NB);tries++){if(errno!=EWOULDBLOCK||tries>=100)throw std::runtime_error("LOCKED file has another owner");::usleep(10000);}}catch(...){close(f);throw;}io_emit(IoOpen,io_h(f),0,nullptr,0,false,&p);return f;
}
inline size_t read_some(File f,void*p,size_t n,uint64_t off){for(;;){auto got=pread(f,p,n,off_t(off));if(got>=0)return size_t(got);if(errno!=EINTR)error("pread");}}
inline size_t write_some(File f,const void*p,size_t n,uint64_t off){for(;;){auto got=pwrite(f,p,n,off_t(off));if(got>=0)return size_t(got);if(errno!=EINTR)error("pwrite");}}
inline uint64_t size(File f){struct stat st{};if(fstat(f,&st))error("fstat");require(st.st_size>=0,"CORRUPT negative size");return uint64_t(st.st_size);}
inline void truncate(File f,uint64_t n){require(n<=uint64_t(std::numeric_limits<off_t>::max()),"LIMIT file size");if(ftruncate(f,off_t(n)))error("ftruncate");io_emit(IoTruncate,io_h(f),n);}
inline void sync(File f){
#ifdef __APPLE__
 while(fcntl(f,F_FULLFSYNC)!=0){if(errno!=EINTR)error("F_FULLFSYNC");}
#else
 // fdatasync flushes the data and the metadata needed to read it back (including a
 // changed file size), but not timestamps. Commits overwrite preallocated space, so
 // this skips an inode journal write per commit; durability is unchanged.
 while(fdatasync(f)!=0){if(errno!=EINTR)error("fdatasync");}
#endif
io_emit(IoSync,io_h(f));}
inline void sync_parent(const std::string&p){auto parent=std::filesystem::absolute(std::filesystem::u8path(p)).parent_path();int f=::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(f<0)error("open parent");int rc;do{rc=fsync(f);}while(rc&&errno==EINTR);int saved=errno;close(f);if(rc){errno=saved;error("fsync parent");}}
inline void remove(const std::string&p){if(unlink(p.c_str()))error("unlink");}
inline void remove_noexcept(const std::string&p)noexcept{unlink(p.c_str());}
inline void remove_directory(const std::string&p){if(::rmdir(p.c_str()))error("rmdir");}
inline void hardlink(const std::string&source,const std::string&dest){if(link(source.c_str(),dest.c_str()))error("link");}
inline void publish(const std::string&temp,const std::string&dest){hardlink(temp,dest);sync_parent(dest);remove(temp);}
// keep becomes a second name of file (whose handles stay open), then temp atomically
// replaces file: file always exists.
inline void replace_keeping(const std::string&file,const std::string&temp,const std::string&keep){
 ::unlink(keep.c_str());if(::link(file.c_str(),keep.c_str()))error("link");
 if(::rename(temp.c_str(),file.c_str())){int e=errno;::unlink(keep.c_str());errno=e;error("rename");}
 sync_parent(file);
}
inline void random_bytes(void*p,size_t n){int f=::open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(f<0)error("open random");auto*b=static_cast<unsigned char*>(p);while(n){auto got=::read(f,b,n);if(got<0){if(errno==EINTR)continue;auto code=errno;close(f);errno=code;error("read random");}if(!got){close(f);throw std::runtime_error("IO random source ended");}b+=got;n-=size_t(got);}close(f);}
inline uint64_t process_id(){return uint64_t(getpid());}
[[noreturn]] inline void exit_now(int code){_exit(code);}
inline void binary_stdio(){}
inline void write_cached(File f,const void*p,size_t n,uint64_t off);  // defined after write_exact
inline void prefetch(const void*p,size_t n)noexcept{madvise(const_cast<void*>(p),n,MADV_WILLNEED);}
inline constexpr bool mapped_supported=true;
// POSIX may map a fixed window past end of file; only committed bytes are ever read.
inline constexpr bool mapping_beyond_eof=true;
#ifdef __APPLE__
inline constexpr const char*name="macos";
inline constexpr bool write_through=false;
inline constexpr const char*sync_name="F_FULLFSYNC";
#else
inline constexpr const char*name="linux";
inline constexpr bool write_through=false;
inline constexpr const char*sync_name="fdatasync";
#endif
inline constexpr const char*namespace_sync="directory-fsync";
class ReadWriteMutex{pthread_rwlock_t lock_{};static void check(int rc){if(rc)throw std::system_error(rc,std::generic_category());}public:ReadWriteMutex(){check(pthread_rwlock_init(&lock_,nullptr));}~ReadWriteMutex(){pthread_rwlock_destroy(&lock_);}ReadWriteMutex(const ReadWriteMutex&)=delete;ReadWriteMutex&operator=(const ReadWriteMutex&)=delete;void lock(){check(pthread_rwlock_wrlock(&lock_));}void unlock(){check(pthread_rwlock_unlock(&lock_));}void lock_shared(){check(pthread_rwlock_rdlock(&lock_));}void unlock_shared(){check(pthread_rwlock_unlock(&lock_));}};
struct Mapping{void*address=nullptr;size_t length=0;Mapping(File f,uint64_t off,size_t n):length(n){auto p=mmap(nullptr,n,PROT_READ,MAP_SHARED,f,off_t(off));if(p!=MAP_FAILED)address=p;}~Mapping(){if(address)munmap(address,length);}Mapping(const Mapping&)=delete;Mapping&operator=(const Mapping&)=delete;};
#endif

struct FD{File n=invalid_file();explicit FD(File f=invalid_file()):n(f){}~FD(){close(n);}FD(const FD&)=delete;FD&operator=(const FD&)=delete;};
inline void read_exact(File f,void*p,size_t n,uint64_t off){auto*b=static_cast<unsigned char*>(p);while(n){auto got=read_some(f,b,n,off);require(got>0,"TRUNCATED committed bytes");b+=got;n-=got;off+=got;}}
inline void write_exact(File f,const void*p,size_t n,uint64_t off){auto io=io_emit(IoWrite,io_h(f),off,p,n);auto*b=static_cast<const unsigned char*>(p);while(n){auto got=write_some(f,b,n,off);require(got>0,"IO short write");b+=got;n-=got;off+=got;}io_emit(IoComplete,io_h(f),0,nullptr,0,false,nullptr,io);}
#ifndef _WIN32
inline void write_cached(File f,const void*p,size_t n,uint64_t off){write_exact(f,p,n,off);}
#endif
// A write started now and finished by write_end (other work can run in between). Windows:
// overlapped I/O, so a write-through write proceeds while the caller computes. Elsewhere
// the write happens in write_begin.
#ifdef _WIN32
struct PendingWrite{OVERLAPPED op{};Event ev;File f=invalid_file();const unsigned char*p=nullptr;size_t n=0,amount=0;uint64_t off=0,io=0;bool started=false;
 PendingWrite()=default;PendingWrite(const PendingWrite&)=delete;PendingWrite&operator=(const PendingWrite&)=delete;
 ~PendingWrite(){if(started){DWORD d=0;GetOverlappedResult(f,&op,&d,TRUE);io_emit(IoComplete,io_h(f),0,nullptr,0,false,nullptr,io);}}  // never leave the kernel writing into a freed OVERLAPPED
};
inline bool write_done(PendingWrite&w){return !w.started||HasOverlappedIoCompleted(&w.op);}
inline void write_begin(PendingWrite&w,File f,const void*p,size_t n,uint64_t off){
 require(off<(uint64_t(1)<<63)&&n<=(uint64_t(1)<<63)-off,"LIMIT file offset");
 w.f=f;w.p=static_cast<const unsigned char*>(p);w.n=n;w.off=off;w.started=false;if(!n)return;
 w.amount=std::min<size_t>(n,64*1024*1024);w.op=OVERLAPPED{};w.op.Offset=DWORD(off);w.op.OffsetHigh=DWORD(off>>32);w.op.hEvent=w.ev.handle;
 w.io=io_emit(IoWrite,io_h(f),off,p,n);
 if(!WriteFile(f,p,DWORD(w.amount),nullptr,&w.op)){auto code=GetLastError();if(code!=ERROR_IO_PENDING)error("WriteFile",code);}
 w.started=true;
}
// How long a committing thread spins on its disk write before sleeping (Windows).
#ifndef KESTRO_COMMIT_SPIN_US
#define KESTRO_COMMIT_SPIN_US 200
#endif
inline void write_end(PendingWrite&w){
 if(!w.started)return;w.started=false;DWORD done=0;
 // A commit write takes tens of microseconds: spin briefly instead of sleeping at once, so the
 // committing thread (and its core) is not put to sleep and woken again for every commit.
 {auto until=std::chrono::steady_clock::now()+std::chrono::microseconds(KESTRO_COMMIT_SPIN_US);while(!HasOverlappedIoCompleted(&w.op)&&std::chrono::steady_clock::now()<until)YieldProcessor();}
 if(!GetOverlappedResult(w.f,&w.op,&done,TRUE))error("GetOverlappedResult",GetLastError());
 if(done<w.n)write_exact(w.f,w.p+done,w.n-done,w.off+done);
 io_emit(IoComplete,io_h(w.f),0,nullptr,0,false,nullptr,w.io);
}
#else
struct PendingWrite{};
inline bool write_done(PendingWrite&){return true;}
inline void write_begin(PendingWrite&,File f,const void*p,size_t n,uint64_t off){write_exact(f,p,n,off);}
inline void write_end(PendingWrite&){}
#endif
} // platform
} // kestro

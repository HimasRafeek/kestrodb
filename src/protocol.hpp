#pragma once
// Request/response encodings shared by the worker process and the in-process addon.
#include "kestro.hpp"
namespace kestro::protocol {
// b points at the command byte ('B'), followed by a uint32 count and the operations.
inline std::vector<Op> parse_batch(const unsigned char*b,size_t n){
 require(n>=5,"INPUT batch header");auto count=get32(b+1);require(count>0&&count<=65536,"INPUT operation count");size_t p=5;std::vector<Op> ops;ops.reserve(count);
 for(uint32_t i=0;i<count;i++){require(p+17<=n,"INPUT short operation");auto kind=b[p];uint64_t expected=get64(b+p+1);auto kn=get32(b+p+9),vn=get32(b+p+13);p+=17;require(kind<=1&&kn&&kn<=1024&&vn<=16*1024*1024&&uint64_t(p)+kn+vn<=n&&(kind||!vn),"INPUT operation bounds");Op o;o.erase=!kind;o.expected=expected;o.key.assign(reinterpret_cast<const char*>(b+p),kn);p+=kn;o.value.assign(reinterpret_cast<const char*>(b+p),vn);p+=vn;ops.push_back(std::move(o));}
 require(p==n,"INPUT trailing bytes");return ops;
}
// b points at the command byte ('M'), followed by a uint32 count and length-prefixed keys.
inline std::vector<std::string> parse_keys(const unsigned char*b,size_t n){
 require(n>=5,"INPUT getMany header");auto count=get32(b+1);require(count<=4096,"LIMIT getMany count");size_t p=5;std::vector<std::string> keys;keys.reserve(count);
 for(uint32_t i=0;i<count;i++){require(p+4<=n,"INPUT getMany key header");auto len=get32(b+p);p+=4;require(len&&len<=1024&&p+len<=n,"INPUT getMany key bounds");keys.emplace_back(reinterpret_cast<const char*>(b+p),len);p+=len;}
 require(p==n,"INPUT getMany trailing bytes");return keys;
}
// Status byte 0, uint32 count, then per key: found byte [, uint64 version, uint32 length, value].
inline Bytes encode_values(const std::vector<Value>&values){
 size_t bytes=5;for(const auto&v:values)bytes+=v.found?13+v.value.size():1;require(bytes<=MAX_GROUP,"LIMIT response");
 Bytes out(bytes);out[0]=0;put32(out.data()+1,uint32_t(values.size()));size_t p=5;
 for(const auto&v:values){out[p++]=v.found;if(v.found){put64(out.data()+p,v.version);put32(out.data()+p+8,uint32_t(v.value.size()));p+=12;memcpy(out.data()+p,v.value.data(),v.value.size());p+=v.value.size();}}
 return out;
}
}

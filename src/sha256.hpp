#pragma once
#include "platform.hpp"
#include <array>
#include <memory>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#elif !defined(_WIN32)
#include <openssl/evp.h>
#endif

namespace kestro {
using Hash=std::array<unsigned char,32>;
namespace hashing {
#ifdef _WIN32
inline void check(NTSTATUS status){if(status<0)throw std::runtime_error("SHA256 CNG operation failed");}
struct Algorithm{BCRYPT_ALG_HANDLE handle=nullptr;Algorithm(){check(BCryptOpenAlgorithmProvider(&handle,BCRYPT_SHA256_ALGORITHM,nullptr,0));}~Algorithm(){if(handle)BCryptCloseAlgorithmProvider(handle,0);}};
inline BCRYPT_ALG_HANDLE algorithm(){static Algorithm provider;return provider.handle;}
class Sha256{
 BCRYPT_HASH_HANDLE hash_=nullptr;
public:
 Sha256(){check(BCryptCreateHash(algorithm(),&hash_,nullptr,0,nullptr,0,BCRYPT_HASH_REUSABLE_FLAG));}
 ~Sha256(){if(hash_)BCryptDestroyHash(hash_);}
 Sha256(const Sha256&)=delete;Sha256&operator=(const Sha256&)=delete;
 void update(const void*p,size_t n){auto*b=static_cast<const unsigned char*>(p);while(n){auto k=ULONG(std::min<size_t>(n,64*1024*1024));check(BCryptHashData(hash_,const_cast<PUCHAR>(b),k,0));b+=k;n-=k;}}
 Hash finish(){Hash out{};check(BCryptFinishHash(hash_,out.data(),ULONG(out.size()),0));return out;}
};
inline constexpr const char*backend="Windows CNG";
#elif defined(__APPLE__)
class Sha256{
 CC_SHA256_CTX context_{};
public:
 Sha256(){require(CC_SHA256_Init(&context_)==1,"SHA256 init");}
 Sha256(const Sha256&)=delete;Sha256&operator=(const Sha256&)=delete;
 void update(const void*p,size_t n){auto*b=static_cast<const unsigned char*>(p);while(n){auto k=CC_LONG(std::min<size_t>(n,64*1024*1024));require(CC_SHA256_Update(&context_,b,k)==1,"SHA256 update");b+=k;n-=k;}}
 Hash finish(){Hash out{};require(CC_SHA256_Final(out.data(),&context_)==1,"SHA256 final");require(CC_SHA256_Init(&context_)==1,"SHA256 reset");return out;}
};
inline constexpr const char*backend="Apple CommonCrypto";
#else
class Sha256{
 std::unique_ptr<EVP_MD,decltype(&EVP_MD_free)>md_{EVP_MD_fetch(nullptr,"SHA256",nullptr),EVP_MD_free};
 std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>ctx_{EVP_MD_CTX_new(),EVP_MD_CTX_free};
public:
 Sha256(){require(md_&&ctx_&&EVP_DigestInit_ex2(ctx_.get(),md_.get(),nullptr)==1,"SHA256 init");}
 Sha256(const Sha256&)=delete;Sha256&operator=(const Sha256&)=delete;
 void update(const void*p,size_t n){require(EVP_DigestUpdate(ctx_.get(),p,n)==1,"SHA256 update");}
 Hash finish(){Hash out{};unsigned n=0;require(EVP_DigestFinal_ex(ctx_.get(),out.data(),&n)==1&&n==out.size(),"SHA256 final");require(EVP_DigestInit_ex2(ctx_.get(),nullptr,nullptr)==1,"SHA256 reset");return out;}
};
inline constexpr const char*backend="OpenSSL 3 EVP";
#endif
} // hashing
using StreamHash=hashing::Sha256;
inline Hash digest(const void*p,size_t n){thread_local std::unique_ptr<hashing::Sha256>state;if(!state)state=std::make_unique<hashing::Sha256>();try{state->update(p,n);return state->finish();}catch(...){state.reset();throw;}}
} // kestro

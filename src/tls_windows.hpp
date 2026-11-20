#pragma once
// TLS for kestro-server on Windows, through Schannel (SSPI): no third-party library.
//   Credentials  a certificate chain and private key, loaded from PEM files (or a PKCS#12
//                .pfx): the server's (inbound), or a follower's client certificate (outbound,
//                --replica-cert); see attach_key for where the key is kept. The server's may
//                also hold the CAs client certificates must lead to (--tls-client-ca).
//   Stream       one TLS connection over a connected socket: accept() for the server's
//                clients, connect() for a follower's link to its leader; read/write
//                plaintext; shutdown() sends close_notify
// TLS 1.2 is the minimum (1.3 where Windows offers it); a peer's renegotiation request ends
// the connection. The client checks the server's chain and host name against the Windows
// trust store, or against only the CA certificates of a PEM file (--replica-ca). With
// --tls-client-ca the server asks every client for a certificate; the server's caller checks
// it (client_certificate, chain_error) against only those CAs, after the handshake.
#ifdef _WIN32
#include "net.hpp"
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#ifndef SCHANNEL_USE_BLACKLISTS
#define SCHANNEL_USE_BLACKLISTS  // SCH_CREDENTIALS (Windows 10 1809+), with a fallback below
#endif
#include <subauth.h>
#include <wincrypt.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>
#include <ncrypt.h>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>

namespace kestro::wintls {
inline std::string hex_status(long s) { char b[16]; std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(s)); return b; }
inline void wipe(std::string& s) { if (!s.empty()) SecureZeroMemory(&s[0], s.size()); s.clear(); }
inline void wipe(std::vector<unsigned char>& v) { if (!v.empty()) SecureZeroMemory(v.data(), v.size()); v.clear(); }

inline std::string read_file(const std::string& path, const std::string& what) {
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("INPUT cannot read " + what + " " + path);
  std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
// The blocks of a PEM file: label ("CERTIFICATE", "PRIVATE KEY", ...) and DER bytes.
struct Pem { std::string label; std::vector<unsigned char> der; };
inline std::vector<Pem> parse_pem(const std::string& text, const std::string& what) {
  std::vector<Pem> out; size_t pos = 0;
  for (;;) {
    auto b = text.find("-----BEGIN ", pos); if (b == std::string::npos) break;
    auto le = text.find("-----", b + 11); require(le != std::string::npos, "INPUT " + what + ": malformed PEM");
    Pem p; p.label = text.substr(b + 11, le - b - 11);
    std::string end = "-----END " + p.label + "-----";
    auto e = text.find(end, le + 5); require(e != std::string::npos, "INPUT " + what + ": PEM block " + p.label + " has no END line");
    std::string body = text.substr(le + 5, e - le - 5);
    // Legacy encrypted PEM ("Proc-Type: 4,ENCRYPTED" headers).
    require(body.find(':') == std::string::npos, "INPUT " + what + ": encrypted PEM is not supported; decrypt it (openssl pkey -in key.pem -out plain.pem) or use --tls-pfx");
    DWORD n = 0;
    require(CryptStringToBinaryA(body.data(), DWORD(body.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr) && n > 0, "INPUT " + what + ": bad base64 in PEM block " + p.label);
    p.der.resize(n);
    require(CryptStringToBinaryA(body.data(), DWORD(body.size()), CRYPT_STRING_BASE64, p.der.data(), &n, nullptr, nullptr), "INPUT " + what + ": bad base64 in PEM block " + p.label);
    p.der.resize(n); wipe(body);
    out.push_back(std::move(p)); pos = e + end.size();
  }
  return out;
}

// DER pieces for wrapping a PKCS#1 RSA or SEC1 EC private key into PKCS#8, the form CNG imports.
inline void der_put(std::vector<unsigned char>& o, unsigned char tag, const unsigned char* p, size_t n) {
  o.push_back(tag);
  if (n < 128) o.push_back(static_cast<unsigned char>(n));
  else { unsigned char t[8]; int k = 0; for (size_t m = n; m; m >>= 8) t[k++] = static_cast<unsigned char>(m); o.push_back(static_cast<unsigned char>(0x80 | k)); while (k) o.push_back(t[--k]); }
  o.insert(o.end(), p, p + n);
}
inline std::vector<unsigned char> pkcs8_wrap(const std::vector<unsigned char>& algorithm, const std::vector<unsigned char>& key) {
  std::vector<unsigned char> body; body.reserve(algorithm.size() + key.size() + 32);  // no reallocation: no stray copies of the key
  body = {0x02, 0x01, 0x00};  // version 0
  der_put(body, 0x30, algorithm.data(), algorithm.size());
  der_put(body, 0x04, key.data(), key.size());
  std::vector<unsigned char> out; out.reserve(body.size() + 8); der_put(out, 0x30, body.data(), body.size()); wipe(body);
  return out;
}

// Schannel signs with the server's key inside the system's security service (LSASS), which
// opens keys by name from a key store: a key held only in this process is refused
// (SEC_E_NO_CREDENTIALS). So the key is imported, non-exportable, under a random name into the
// current user's Microsoft Software Key Storage Provider (protected by DPAPI), and deleted when
// the server stops; keys left behind by a server that did not stop (killed, power loss) are
// deleted at the next start.
inline const wchar_t* const key_prefix = L"KestroDB-TLS-";
inline void delete_stale_keys(NCRYPT_PROV_HANDLE prov) {
  std::vector<std::wstring> stale; NCryptKeyName* name = nullptr; PVOID state = nullptr;
  const size_t plen = wcslen(key_prefix);
  while (NCryptEnumKeys(prov, nullptr, &name, &state, NCRYPT_SILENT_FLAG) == ERROR_SUCCESS) {
    std::wstring n = name->pszName; NCryptFreeBuffer(name); name = nullptr;
    if (n.compare(0, plen, key_prefix) != 0) continue;
    DWORD pid = static_cast<DWORD>(wcstoul(n.c_str() + plen, nullptr, 10));
    HANDLE h = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (h) { DWORD code = 0; bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE; CloseHandle(h); if (alive) continue; }
    else if (GetLastError() != ERROR_INVALID_PARAMETER) continue;  // exists, but not ours to open
    stale.push_back(n);
  }
  if (state) NCryptFreeBuffer(state);
  for (const auto& n : stale) { NCRYPT_KEY_HANDLE k = 0; if (NCryptOpenKey(prov, &k, n.c_str(), 0, NCRYPT_SILENT_FLAG) == ERROR_SUCCESS && NCryptDeleteKey(k, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS) NCryptFreeObject(k); }
}

struct Credentials {
  CredHandle cred{}; bool have_cred = false;
  HCERTSTORE store = nullptr;        // the certificate chain, in memory
  PCCERT_CONTEXT cert = nullptr;     // the server's certificate (in store)
  NCRYPT_PROV_HANDLE provider = 0;
  NCRYPT_KEY_HANDLE key = 0;         // its private key (named key_name, deleted here)
  std::wstring key_name;
  HCERTSTORE user_ca = nullptr;      // the current user's intermediate CA store, and the
  std::vector<PCCERT_CONTEXT> published;  // intermediates added there (removed here)
  HCERTSTORE client_roots = nullptr; HCERTCHAINENGINE client_engine = nullptr;  // --tls-client-ca
  Credentials() = default;
  Credentials(const Credentials&) = delete;
  Credentials& operator=(const Credentials&) = delete;
  ~Credentials() {
    if (have_cred) FreeCredentialsHandle(&cred);
    if (client_engine) CertFreeCertificateChainEngine(client_engine);
    if (client_roots) CertCloseStore(client_roots, 0);
    if (cert) CertFreeCertificateContext(cert);
    if (key && NCryptDeleteKey(key, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS) NCryptFreeObject(key);
    if (provider) NCryptFreeObject(provider);
    if (store) CertCloseStore(store, 0);
    for (auto p : published) CertDeleteCertificateFromStore(p);
    if (user_ca) CertCloseStore(user_ca, 0);
  }
};

// TLS 1.2 and later only, strong cipher suites.
inline SECURITY_STATUS acquire(bool server, PCCERT_CONTEXT cert, CredHandle& cred) {
  DWORD flags = SCH_USE_STRONG_CRYPTO | (server ? SCH_CRED_NO_SYSTEM_MAPPER : SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION);
  TimeStamp expiry{}; SECURITY_STATUS st = SEC_E_UNSUPPORTED_FUNCTION;
#ifdef SCH_CREDENTIALS_VERSION
  TLS_PARAMETERS tp{}; tp.grbitDisabledProtocols = SP_PROT_SSL2 | SP_PROT_SSL3 | SP_PROT_TLS1_0 | SP_PROT_TLS1_1 | SP_PROT_PCT1;
  SCH_CREDENTIALS sc{}; sc.dwVersion = SCH_CREDENTIALS_VERSION; sc.dwFlags = flags; sc.cTlsParameters = 1; sc.pTlsParameters = &tp;
  if (cert) { sc.cCreds = 1; sc.paCred = &cert; }
  st = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr, nullptr, &cred, &expiry);
  if (st == SEC_E_OK) return st;
#endif
  // Windows before 10 1809: the older structure, TLS 1.2 only.
  SCHANNEL_CRED old{}; old.dwVersion = SCHANNEL_CRED_VERSION; old.dwFlags = flags; old.grbitEnabledProtocols = server ? SP_PROT_TLS1_2_SERVER : SP_PROT_TLS1_2_CLIENT;
  if (cert) { old.cCreds = 1; old.paCred = &cert; }
  SECURITY_STATUS st2 = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND, nullptr, &old, nullptr, nullptr, &cred, &expiry);
  return st2 == SEC_E_OK ? st2 : st == SEC_E_UNSUPPORTED_FUNCTION ? st2 : st;
}

// Schannel builds the chain it sends from the system's certificate stores, not from this
// process's memory: the intermediate CAs of the certificate file are added to the current
// user's "Intermediate Certification Authorities" store (public certificates; this store
// confers no trust) while the server runs, unless they are there already.
inline void publish_intermediates(Credentials& c) {
  for (PCCERT_CONTEXT it = nullptr; (it = CertEnumCertificatesInStore(c.store, it)) != nullptr;) {
    if (CertCompareCertificate(X509_ASN_ENCODING, it->pCertInfo, c.cert->pCertInfo)) continue;
    if (CertCompareCertificateName(X509_ASN_ENCODING, &it->pCertInfo->Subject, &it->pCertInfo->Issuer)) continue;  // a self-signed root is not sent
    if (!c.user_ca) c.user_ca = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"CA");
    if (!c.user_ca) { CertFreeCertificateContext(it); throw std::runtime_error("IO cannot open the user's intermediate CA store (Windows error " + std::to_string(GetLastError()) + ")"); }
    PCCERT_CONTEXT added = nullptr;
    if (CertAddEncodedCertificateToStore(c.user_ca, X509_ASN_ENCODING, it->pbCertEncoded, it->cbCertEncoded, CERT_STORE_ADD_NEW, &added)) c.published.push_back(added);
    else if (GetLastError() != DWORD(CRYPT_E_EXISTS)) { CertFreeCertificateContext(it); throw std::runtime_error("IO cannot add an intermediate CA to the user's store (Windows error " + std::to_string(GetLastError()) + ")"); }
  }
}
// The private key (an unencrypted PKCS#8 blob, wiped here) for c.cert: imported as described
// above, checked against the certificate, then the Schannel credentials (server: inbound).
inline void attach_key(Credentials& c, std::vector<unsigned char>& pkcs8, const std::string& what, bool server) {
  SECURITY_STATUS st = NCryptOpenStorageProvider(&c.provider, MS_KEY_STORAGE_PROVIDER, 0);
  if (st != ERROR_SUCCESS) { wipe(pkcs8); throw std::runtime_error("IO cannot open the Microsoft key storage provider " + hex_status(st)); }
  delete_stale_keys(c.provider);
  unsigned char rnd[12]; BCryptGenRandom(nullptr, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  wchar_t hex[32]; for (int i = 0; i < 12; i++) swprintf(hex + 2 * i, 3, L"%02x", rnd[i]);
  c.key_name = std::wstring(key_prefix) + std::to_wstring(GetCurrentProcessId()) + L"-" + hex;
  NCryptBuffer nb{DWORD((c.key_name.size() + 1) * sizeof(wchar_t)), NCRYPTBUFFER_PKCS_KEY_NAME, c.key_name.data()};
  NCryptBufferDesc params{NCRYPTBUFFER_VERSION, 1, &nb};
  NCRYPT_KEY_HANDLE k = 0;
  st = NCryptImportKey(c.provider, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, &params, &k, pkcs8.data(), DWORD(pkcs8.size()), NCRYPT_DO_NOT_FINALIZE_FLAG | NCRYPT_SILENT_FLAG);
  wipe(pkcs8);
  if (st != ERROR_SUCCESS) throw std::runtime_error("INPUT " + what + " cannot be imported (" + hex_status(st) + "): an unencrypted RSA or EC private key is expected");
  DWORD policy = 0;  // not exportable
  NCryptSetProperty(k, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&policy), sizeof policy, NCRYPT_PERSIST_FLAG | NCRYPT_SILENT_FLAG);
  st = NCryptFinalizeKey(k, NCRYPT_SILENT_FLAG);
  if (st != ERROR_SUCCESS) { if (NCryptDeleteKey(k, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS) NCryptFreeObject(k); throw std::runtime_error("IO cannot store the TLS key for Schannel (" + hex_status(st) + ")"); }
  c.key = k;
  // The key must belong to the certificate: sign with it, check with the certificate.
  {
    BCRYPT_KEY_HANDLE pub = nullptr;
    require(CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING, &c.cert->pCertInfo->SubjectPublicKeyInfo, 0, nullptr, &pub), "INPUT the certificate's public key type is not supported");
    unsigned char hash[32]; for (int i = 0; i < 32; i++) hash[i] = static_cast<unsigned char>(i * 7 + 1);
    bool rsa = std::string(c.cert->pCertInfo->SubjectPublicKeyInfo.Algorithm.pszObjId) == szOID_RSA_RSA;
    BCRYPT_PKCS1_PADDING_INFO pad{BCRYPT_SHA256_ALGORITHM};
    std::vector<unsigned char> sig(1024); DWORD n = 0;
    st = NCryptSignHash(c.key, rsa ? &pad : nullptr, hash, 32, sig.data(), DWORD(sig.size()), &n, NCRYPT_SILENT_FLAG | (rsa ? NCRYPT_PAD_PKCS1_FLAG : 0));
    bool ok = st == ERROR_SUCCESS && BCryptVerifySignature(pub, rsa ? &pad : nullptr, hash, 32, sig.data(), n, rsa ? BCRYPT_PAD_PKCS1 : 0) == 0;
    BCryptDestroyKey(pub);
    require(ok, "INPUT " + what + " does not match the certificate");
  }
  publish_intermediates(c);
  CRYPT_KEY_PROV_INFO info{}; info.pwszContainerName = c.key_name.data(); info.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);  // dwKeySpec 0: a CNG key (AT_KEYEXCHANGE fails for EC keys)
  require(CertSetCertificateContextProperty(c.cert, CERT_KEY_PROV_INFO_PROP_ID, 0, &info), "IO cannot attach the private key to the certificate");
  st = acquire(server, c.cert, c.cred);
  require(st == SEC_E_OK, "IO Schannel refused the certificate and key " + what + " (" + hex_status(st) + ")");
  c.have_cred = true;
}
inline void add_certificate(Credentials& c, const unsigned char* der, size_t n, const std::string& what) {
  PCCERT_CONTEXT added = nullptr;
  require(CertAddEncodedCertificateToStore(c.store, X509_ASN_ENCODING, der, DWORD(n), CERT_STORE_ADD_ALWAYS, c.cert ? nullptr : &added),
          "INPUT " + what + ": a certificate cannot be parsed (Windows error " + std::to_string(GetLastError()) + ")");
  if (!c.cert) c.cert = added;
}

// A certificate (first CERTIFICATE block of cert_path, followed by its chain) and private key
// (key_path: PKCS#8 "PRIVATE KEY", PKCS#1 "RSA PRIVATE KEY" or SEC1 "EC PRIVATE KEY"); the
// server's (--tls-cert/--tls-key), or a follower's client certificate (server false).
inline std::unique_ptr<Credentials> load_pem(const std::string& cert_path, const std::string& key_path, bool server = true,
                                             const std::string& cert_opt = "--tls-cert", const std::string& key_opt = "--tls-key") {
  auto c = std::make_unique<Credentials>();
  c->store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
  require(c->store != nullptr, "IO cannot create a certificate store");
  for (const auto& p : parse_pem(read_file(cert_path, cert_opt), cert_opt))
    if (p.label == "CERTIFICATE" || p.label == "TRUSTED CERTIFICATE") add_certificate(*c, p.der.data(), p.der.size(), cert_opt);
  require(c->cert != nullptr, "INPUT " + cert_opt + " " + cert_path + " holds no PEM CERTIFICATE");

  std::string key_text = read_file(key_path, key_opt);
  auto blocks = parse_pem(key_text, key_opt); wipe(key_text);
  std::vector<unsigned char> pkcs8;
  for (auto& p : blocks) {
    if (pkcs8.empty()) {
      if (p.label == "PRIVATE KEY") pkcs8.swap(p.der);
      else if (p.label == "RSA PRIVATE KEY") {
        static const std::vector<unsigned char> rsa = {0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01, 0x05, 0x00};  // rsaEncryption, NULL
        pkcs8 = pkcs8_wrap(rsa, p.der);
      } else if (p.label == "EC PRIVATE KEY") {
        // ecPublicKey with the curve the certificate names (its public key parameters).
        const auto& params = c->cert->pCertInfo->SubjectPublicKeyInfo.Algorithm.Parameters;
        if (std::string(c->cert->pCertInfo->SubjectPublicKeyInfo.Algorithm.pszObjId) == szOID_ECC_PUBLIC_KEY && params.cbData > 0) {
          std::vector<unsigned char> ec = {0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
          ec.insert(ec.end(), params.pbData, params.pbData + params.cbData);
          pkcs8 = pkcs8_wrap(ec, p.der);
        } else { for (auto& q : blocks) wipe(q.der); throw std::runtime_error("INPUT " + key_opt + " is an EC key but the certificate's key is not"); }
      } else if (p.label == "ENCRYPTED PRIVATE KEY") { for (auto& q : blocks) wipe(q.der); throw std::runtime_error("INPUT " + key_opt + " is encrypted; decrypt it (openssl pkey -in key.pem -out plain.pem)" + std::string(server ? " or use --tls-pfx with --tls-pfx-password-file" : "")); }
    }
    wipe(p.der);
  }
  require(!pkcs8.empty(), "INPUT " + key_opt + " " + key_path + " holds no PEM PRIVATE KEY, RSA PRIVATE KEY or EC PRIVATE KEY");
  attach_key(*c, pkcs8, key_opt, server);
  return c;
}

// A PKCS#12 file (certificate, chain and key). Its key is first opened in memory only, then
// handled like a PEM key.
inline std::unique_ptr<Credentials> load_pfx(const std::string& pfx_path, const std::string& password_file) {
  auto c = std::make_unique<Credentials>();
  std::string data = read_file(pfx_path, "--tls-pfx"), pw;
  if (!password_file.empty()) { pw = read_file(password_file, "--tls-pfx-password-file"); while (!pw.empty() && (pw.back() == '\n' || pw.back() == '\r')) pw.pop_back(); }
  std::wstring wpw = pw.empty() ? std::wstring() : platform::wide(pw); wipe(pw);
  CRYPT_DATA_BLOB blob{DWORD(data.size()), reinterpret_cast<BYTE*>(data.data())};
  const DWORD flags = PKCS12_NO_PERSIST_KEY | PKCS12_ALWAYS_CNG_KSP | CRYPT_EXPORTABLE;
  HCERTSTORE pfx = PFXImportCertStore(&blob, wpw.c_str(), flags);
  if (!pfx && wpw.empty()) pfx = PFXImportCertStore(&blob, nullptr, flags);  // no password: empty or absent
  DWORD err = pfx ? 0 : GetLastError();
  if (!wpw.empty()) SecureZeroMemory(&wpw[0], wpw.size() * sizeof(wchar_t));
  wipe(data);
  require(pfx != nullptr, "INPUT --tls-pfx cannot be opened (Windows error " + std::to_string(err) + "; wrong password?)");
  struct Close { HCERTSTORE s; ~Close() { CertCloseStore(s, 0); } } close_pfx{pfx};
  c->store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
  require(c->store != nullptr, "IO cannot create a certificate store");
  // The certificate with a private key first, then the rest of the chain.
  std::vector<unsigned char> pkcs8;
  for (PCCERT_CONTEXT it = nullptr; (it = CertEnumCertificatesInStore(pfx, it)) != nullptr;) {
    // An in-memory CNG key is a handle property of the certificate (owned by it).
    NCRYPT_KEY_HANDLE k = 0; DWORD spec = 0, size = sizeof k; BOOL free_key = FALSE;
    if (!CertGetCertificateContextProperty(it, CERT_NCRYPT_KEY_HANDLE_PROP_ID, &k, &size) &&
        !CryptAcquireCertificatePrivateKey(it, CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG, nullptr, &k, &spec, &free_key)) continue;
    DWORD policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;  // an in-memory key, exported once for the import below
    NCryptSetProperty(k, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&policy), sizeof policy, NCRYPT_SILENT_FLAG);
    DWORD n = 0; SECURITY_STATUS st = NCryptExportKey(k, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, nullptr, 0, &n, NCRYPT_SILENT_FLAG);
    if (st == ERROR_SUCCESS) { pkcs8.resize(n); st = NCryptExportKey(k, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, pkcs8.data(), n, &n, NCRYPT_SILENT_FLAG); pkcs8.resize(n); }
    if (free_key) NCryptFreeObject(k);
    if (st != ERROR_SUCCESS) { wipe(pkcs8); CertFreeCertificateContext(it); throw std::runtime_error("INPUT --tls-pfx: its private key cannot be read (" + hex_status(st) + ")"); }
    add_certificate(*c, it->pbCertEncoded, it->cbCertEncoded, "--tls-pfx");
    CertFreeCertificateContext(it); break;
  }
  require(c->cert != nullptr, "INPUT --tls-pfx holds no certificate with a private key");
  for (PCCERT_CONTEXT it = nullptr; (it = CertEnumCertificatesInStore(pfx, it)) != nullptr;)
    if (!CertCompareCertificate(X509_ASN_ENCODING, it->pCertInfo, c->cert->pCertInfo)) add_certificate(*c, it->pbCertEncoded, it->cbCertEncoded, "--tls-pfx");
  attach_key(*c, pkcs8, "--tls-pfx", true);
  return c;
}

// A chain engine that trusts only the CA certificates of a PEM file (opt names it in errors);
// roots holds them (both are the caller's to free).
inline HCERTCHAINENGINE ca_engine(const std::string& file, const std::string& opt, HCERTSTORE& roots) {
  roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
  require(roots != nullptr, "IO cannot create a certificate store");
  size_t count = 0;
  for (const auto& p : parse_pem(read_file(file, opt), opt))
    if (p.label == "CERTIFICATE" || p.label == "TRUSTED CERTIFICATE") { require(CertAddEncodedCertificateToStore(roots, X509_ASN_ENCODING, p.der.data(), DWORD(p.der.size()), CERT_STORE_ADD_ALWAYS, nullptr), "INPUT " + opt + ": a certificate cannot be parsed"); count++; }
  require(count > 0, "INPUT " + opt + " " + file + " holds no PEM CERTIFICATE");
  CERT_CHAIN_ENGINE_CONFIG ec{}; ec.cbSize = sizeof ec; ec.hExclusiveRoot = roots;
  HCERTCHAINENGINE engine = nullptr;
  require(CertCreateCertificateChainEngine(&ec, &engine), "IO cannot create a certificate chain engine (Windows error " + std::to_string(GetLastError()) + ")");
  return engine;
}
// The server's CAs for client certificates (--tls-client-ca).
inline void load_client_ca(Credentials& c, const std::string& file) { c.client_engine = ca_engine(file, "--tls-client-ca", c.client_roots); }
// Checks cert's chain (with the certificates its peer sent, in cert->hCertStore) against engine
// (nullptr: the system trust store), for server or client authentication; 0 when it holds,
// else the CERT_E_* error. server_name: the name a server's certificate must carry.
inline long chain_error(HCERTCHAINENGINE engine, PCCERT_CONTEXT cert, bool server_auth, wchar_t* server_name) {
  PCCERT_CHAIN_CONTEXT chain = nullptr;
  LPSTR usage[] = {const_cast<LPSTR>(server_auth ? szOID_PKIX_KP_SERVER_AUTH : szOID_PKIX_KP_CLIENT_AUTH)};
  CERT_CHAIN_PARA cp{}; cp.cbSize = sizeof cp; cp.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND; cp.RequestedUsage.Usage.cUsageIdentifier = 1; cp.RequestedUsage.Usage.rgpszUsageIdentifier = usage;
  DWORD flags = engine ? CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL : 0;  // no network fetches for a private CA
  require(CertGetCertificateChain(engine, cert, nullptr, cert->hCertStore, &cp, flags, nullptr, &chain), "IO TLS: cannot build a certificate chain (Windows error " + std::to_string(GetLastError()) + ")");
  SSL_EXTRA_CERT_CHAIN_POLICY_PARA ex{}; ex.cbSize = sizeof ex; ex.dwAuthType = server_auth ? AUTHTYPE_SERVER : AUTHTYPE_CLIENT; ex.pwszServerName = server_name;
  CERT_CHAIN_POLICY_PARA pp{}; pp.cbSize = sizeof pp; pp.pvExtraPolicyPara = &ex;
  CERT_CHAIN_POLICY_STATUS ps{}; ps.cbSize = sizeof ps;
  BOOL ok = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &pp, &ps);
  CertFreeCertificateChain(chain);
  require(ok, "IO TLS: cannot check a certificate chain");
  return static_cast<long>(ps.dwError);
}
inline std::string chain_reason(long e) {
  return e == CERT_E_UNTRUSTEDROOT || e == CERT_E_CHAINING ? "not issued by a trusted CA" : e == CERT_E_EXPIRED ? "expired or not yet valid"
       : e == CERT_E_WRONG_USAGE ? "not for this use (extended key usage)" : "error " + hex_status(e);
}

class Stream {
public:
  ~Stream() {
    if (have_ctx_) DeleteSecurityContext(&ctx_);
    if (own_cred_) FreeCredentialsHandle(&client_cred_);
  }
  // Server side: the TLS handshake with a new client, within deadline_seconds. nullptr when
  // it fails (not TLS, a protocol the server refuses, too slow, ...).
  // With client CAs (load_client_ca) the client is asked for a certificate: see client_certificate.
  static Stream* accept(Credentials& c, net::Socket s, int deadline_seconds) {
    std::unique_ptr<Stream> t(new Stream(s, true));
    t->cred_ = &c.cred; if (c.client_engine) t->server_flags_ |= ASC_REQ_MUTUAL_AUTH;
    t->deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(deadline_seconds > 0 ? deadline_seconds : 10);
    if (!t->handshake() || !t->ready()) return nullptr;
    return t.release();
  }
  // Client side (a follower to its leader): handshake, then the server's certificate is
  // checked against host and the system trust store, or only the CAs in ca_file. Throws.
  // mine: this follower's client certificate (load_pem with server false), or nullptr.
  static Stream* connect(net::Socket s, std::string host, const std::string& ca_file, Credentials* mine = nullptr) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    std::unique_ptr<Stream> t(new Stream(s, false));
    t->target_ = platform::wide(host);
    if (mine) t->cred_ = &mine->cred;
    else {
      SECURITY_STATUS st = acquire(false, nullptr, t->client_cred_);
      if (st != SEC_E_OK) throw std::runtime_error("IO TLS client credentials " + hex_status(st));
      t->own_cred_ = true; t->cred_ = &t->client_cred_;
    }
    t->deadline_ = std::chrono::steady_clock::time_point::max();
    if (!t->handshake()) throw std::runtime_error("IO TLS handshake with the leader failed" + (t->error_.empty() ? std::string() : " (" + t->error_ + ")"));
    t->verify(ca_file);
    if (!t->ready()) throw std::runtime_error("IO TLS handshake with the leader failed (" + t->error_ + ")");
    return t.release();
  }
  // Server side: the client's certificate (the caller frees it), nullptr when it sent none.
  PCCERT_CONTEXT client_certificate() {
    PCCERT_CONTEXT remote = nullptr;
    if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote) != SEC_E_OK) return nullptr;
    return remote;
  }
  // Up to n plaintext bytes: > 0 read, 0 the peer closed, -1 an error or timeout.
  long long read(void* p, size_t n) {
    for (;;) {
      if (plain_n_) {
        size_t k = std::min(n, plain_n_); memcpy(p, in_.data() + plain_off_, k);
        plain_off_ += k; plain_n_ -= k; return static_cast<long long>(k);
      }
      if (closed_) return 0;
      if (enc_off_) { memmove(in_.data(), in_.data() + enc_off_, in_len_ - enc_off_); in_len_ -= enc_off_; enc_off_ = 0; }
      if (in_len_) {
        SecBuffer b[4] = {{DWORD(in_len_), SECBUFFER_DATA, in_.data()}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc d{SECBUFFER_VERSION, 4, b};
        SECURITY_STATUS st = DecryptMessage(&ctx_, &d, 0, nullptr);
        if (st == SEC_E_OK || st == SEC_I_RENEGOTIATE || st == SEC_I_CONTEXT_EXPIRED) {
          size_t extra_off = in_len_;
          for (auto& x : b) {
            if (x.BufferType == SECBUFFER_DATA && x.cbBuffer) { plain_off_ = size_t(static_cast<unsigned char*>(x.pvBuffer) - in_.data()); plain_n_ = x.cbBuffer; }
            if (x.BufferType == SECBUFFER_EXTRA && x.cbBuffer) extra_off = in_len_ - x.cbBuffer;
          }
          enc_off_ = extra_off;
          if (st == SEC_I_CONTEXT_EXPIRED) { closed_ = true; continue; }  // close_notify
          if (st == SEC_I_RENEGOTIATE) {
            // TLS 1.3 post-handshake messages (session tickets, key updates) come this way;
            // a TLS 1.2 renegotiation is refused: the connection ends.
            if (!tls13_) return -1;
            memmove(in_.data(), in_.data() + enc_off_, in_len_ - enc_off_); in_len_ -= enc_off_; enc_off_ = 0; plain_n_ = 0;
            if (!handshake(true)) return -1;
          }
          continue;
        }
        if (st != SEC_E_INCOMPLETE_MESSAGE) return -1;
      }
      if (!fill()) return -1;
    }
  }
  bool write(const unsigned char* p, size_t n) {
    while (n) {
      size_t used = 0;
      // Up to two records per send.
      for (int r = 0; r < 2 && n; r++) {
        size_t k = std::min<size_t>(n, sizes_.cbMaximumMessage);
        unsigned char* at = out_.data() + used;
        memcpy(at + sizes_.cbHeader, p, k);
        SecBuffer b[4] = {{sizes_.cbHeader, SECBUFFER_STREAM_HEADER, at}, {DWORD(k), SECBUFFER_DATA, at + sizes_.cbHeader},
                          {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, at + sizes_.cbHeader + k}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc d{SECBUFFER_VERSION, 4, b};
        if (EncryptMessage(&ctx_, 0, &d, 0) != SEC_E_OK) return false;
        used += b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer;
        p += k; n -= k;
      }
      if (!net::send_all(sock_, out_.data(), used)) return false;
    }
    return true;
  }
  // close_notify to the peer (best effort).
  void shutdown() {
    if (!have_ctx_ || !ready_) return;
    DWORD type = SCHANNEL_SHUTDOWN;
    SecBuffer in{sizeof type, SECBUFFER_TOKEN, &type}; SecBufferDesc ind{SECBUFFER_VERSION, 1, &in};
    if (ApplyControlToken(&ctx_, &ind) != SEC_E_OK) return;
    SecBuffer out{0, SECBUFFER_TOKEN, nullptr}; SecBufferDesc outd{SECBUFFER_VERSION, 1, &out}; ULONG attrs = 0;
    SECURITY_STATUS st = server_
      ? AcceptSecurityContext(cred_, &ctx_, nullptr, server_flags_, 0, nullptr, &outd, &attrs, nullptr)
      : InitializeSecurityContextW(cred_, &ctx_, target_.data(), client_flags, 0, 0, nullptr, 0, nullptr, &outd, &attrs, nullptr);
    if ((st == SEC_E_OK || st == SEC_I_CONTEXT_EXPIRED) && out.pvBuffer && out.cbBuffer) net::send_all(sock_, static_cast<unsigned char*>(out.pvBuffer), out.cbBuffer);
    if (out.pvBuffer) FreeContextBuffer(out.pvBuffer);
    ready_ = false;
  }

private:
  ULONG server_flags_ = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;
  static constexpr ULONG client_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY |
                                        ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION | ISC_REQ_USE_SUPPLIED_CREDS;
  static constexpr size_t max_record = 16384 + 2048 + 256;  // the largest TLS record, with room
  Stream(net::Socket s, bool server) : sock_(s), server_(server) { SecInvalidateHandle(&ctx_); in_.resize(2 * max_record); }

  // More encrypted bytes from the socket (false: closed, error, timeout or past the deadline).
  bool fill() {
    if (std::chrono::steady_clock::now() > deadline_) return false;
    if (in_len_ == in_.size()) {
      if (ready_ || in_.size() >= 256 * 1024) return false;  // a record larger than TLS allows, or a handshake flight too large
      in_.resize(in_.size() * 2);  // a long certificate chain
    }
    auto got = net::recv_some(sock_, in_.data() + in_len_, in_.size() - in_len_);
    if (got <= 0) return false;
    in_len_ += size_t(got); return true;
  }
  bool send_token(SecBuffer& b) {
    bool ok = true;
    if (b.pvBuffer && b.cbBuffer) ok = net::send_all(sock_, static_cast<unsigned char*>(b.pvBuffer), b.cbBuffer);
    if (b.pvBuffer) FreeContextBuffer(b.pvBuffer);
    b.pvBuffer = nullptr; b.cbBuffer = 0;
    return ok;
  }
  // The handshake loop (also for TLS 1.3 post-handshake messages, with in_ holding them).
  // post: TLS 1.3 post-handshake messages, given first with what in_ holds (maybe nothing).
  bool handshake(bool post = false) {
    bool first = !have_ctx_, need = server_ && !post && in_len_ == 0;
    if (!server_ && first) {  // the client speaks first
      SecBuffer out{0, SECBUFFER_TOKEN, nullptr}; SecBufferDesc outd{SECBUFFER_VERSION, 1, &out}; ULONG attrs = 0;
      SECURITY_STATUS st = InitializeSecurityContextW(cred_, nullptr, target_.data(), client_flags, 0, 0, nullptr, 0, &ctx_, &outd, &attrs, nullptr);
      have_ctx_ = SecIsValidHandle(&ctx_);
      if (st != SEC_I_CONTINUE_NEEDED) { if (out.pvBuffer) FreeContextBuffer(out.pvBuffer); error_ = "InitializeSecurityContext " + hex_status(st); return false; }
      if (!send_token(out)) { error_ = "send"; return false; }
      need = true;
    }
    for (;;) {
      if (need) { if (!fill()) { if (error_.empty()) error_ = "the connection closed or timed out during the handshake"; return false; } need = false; }
      SecBuffer in[2] = {{DWORD(in_len_), SECBUFFER_TOKEN, in_.data()}, {0, SECBUFFER_EMPTY, nullptr}};
      SecBuffer out[3] = {{0, SECBUFFER_TOKEN, nullptr}, {0, SECBUFFER_ALERT, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
      SecBufferDesc ind{SECBUFFER_VERSION, 2, in}, outd{SECBUFFER_VERSION, 3, out};
      ULONG attrs = 0; SECURITY_STATUS st;
      if (server_) {
        st = AcceptSecurityContext(cred_, have_ctx_ ? &ctx_ : nullptr, &ind, server_flags_, 0, &ctx_, &outd, &attrs, nullptr);
        if (!have_ctx_) have_ctx_ = SecIsValidHandle(&ctx_);
      } else {
        st = InitializeSecurityContextW(cred_, &ctx_, target_.data(), client_flags, 0, 0, &ind, 0, nullptr, &outd, &attrs, nullptr);
      }
      if (st == SEC_E_INCOMPLETE_MESSAGE) { for (auto& o : out) if (o.pvBuffer) FreeContextBuffer(o.pvBuffer); need = true; continue; }
      bool sent = send_token(out[0]);  // also an alert on failure (extended error)
      if (out[1].pvBuffer) FreeContextBuffer(out[1].pvBuffer);
      if (out[2].pvBuffer) FreeContextBuffer(out[2].pvBuffer);
      if (st == SEC_I_INCOMPLETE_CREDENTIALS) continue;  // a client certificate was asked for: go on without one (same input)
      if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED) { error_ = (server_ ? "AcceptSecurityContext " : "InitializeSecurityContext ") + hex_status(st); return false; }
      if (!sent) { error_ = "send"; return false; }
      // Bytes after this handshake message: the next message, or (when done) the first data.
      if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer) { memmove(in_.data(), in_.data() + (in_len_ - in[1].cbBuffer), in[1].cbBuffer); in_len_ = in[1].cbBuffer; }
      else in_len_ = 0;
      if (st == SEC_E_OK) return true;
      need = in_len_ == 0;
    }
  }
  // After the handshake: TLS 1.2 or later, and the record sizes.
  bool ready() {
    SecPkgContext_ConnectionInfo info{};
    if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_CONNECTION_INFO, &info) != SEC_E_OK) { error_ = "connection info"; return false; }
    if (!(info.dwProtocol & (SP_PROT_TLS1_2 | SP_PROT_TLS1_3))) { error_ = "protocol older than TLS 1.2"; return false; }
    tls13_ = (info.dwProtocol & SP_PROT_TLS1_3) != 0;
    if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK) { error_ = "stream sizes"; return false; }
    out_.resize(2 * (size_t(sizes_.cbHeader) + sizes_.cbMaximumMessage + sizes_.cbTrailer));
    ready_ = true; deadline_ = std::chrono::steady_clock::time_point::max();
    return true;
  }
  // The server's certificate chain and name (client side).
  void verify(const std::string& ca_file) {
    PCCERT_CONTEXT remote = nullptr;
    if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote) != SEC_E_OK || !remote) throw std::runtime_error("IO TLS: the leader sent no certificate");
    HCERTSTORE roots = nullptr; HCERTCHAINENGINE engine = nullptr;
    auto cleanup = [&] { if (engine) CertFreeCertificateChainEngine(engine); if (roots) CertCloseStore(roots, 0); CertFreeCertificateContext(remote); };
    try {
      if (!ca_file.empty()) engine = ca_engine(ca_file, "--replica-ca", roots);  // only its CAs are trusted (not the system store)
      long e = chain_error(engine, remote, true, target_.data());
      if (e) {
        bool untrusted = e == CERT_E_UNTRUSTEDROOT || e == CERT_E_CHAINING;
        std::string why = e == CERT_E_CN_NO_MATCH ? "its name does not match " + platform::utf8(target_) : chain_reason(e) + (untrusted && !ca_file.empty() ? " (--replica-ca)" : "");
        throw std::runtime_error("IO TLS handshake with the leader failed (certificate: " + why + ")");
      }
    } catch (...) { cleanup(); throw; }
    cleanup();
  }

  net::Socket sock_; bool server_;
  CredHandle* cred_ = nullptr; CredHandle client_cred_{}; bool own_cred_ = false;
  CtxtHandle ctx_{}; bool have_ctx_ = false, ready_ = false, closed_ = false, tls13_ = false;
  std::wstring target_;
  std::chrono::steady_clock::time_point deadline_;
  SecPkgContext_StreamSizes sizes_{};
  std::string error_;
  // Received bytes in_[0, in_len_): decrypted plaintext in_[plain_off_, +plain_n_), then
  // encrypted bytes not yet decrypted from enc_off_.
  std::vector<unsigned char> in_, out_;
  size_t in_len_ = 0, enc_off_ = 0, plain_off_ = 0, plain_n_ = 0;
};
}  // namespace kestro::wintls
#endif

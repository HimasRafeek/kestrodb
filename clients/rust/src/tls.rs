//! tls:// with rustls (feature `tls`): the system's CAs, a CA file, or no verification.

use crate::Options;
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{verify_tls12_signature, verify_tls13_signature, CryptoProvider};
use rustls::pki_types::pem::PemObject;
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::{
    ClientConfig, ClientConnection, DigitallySignedStruct, RootCertStore, SignatureScheme,
    StreamOwned,
};
use std::net::TcpStream;
use std::sync::Arc;

/// The client configuration the options ask for (TLS 1.2 and 1.3).
pub(crate) fn config(options: &Options) -> Result<Arc<ClientConfig>, String> {
    if let Some(c) = &options.tls_config {
        return Ok(c.clone());
    }
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let builder = ClientConfig::builder_with_provider(provider.clone())
        .with_safe_default_protocol_versions()
        .map_err(|e| format!("TLS setup: {e}"))?;
    if !options.tls_verify {
        let verifier = Arc::new(NoVerification(provider));
        return Ok(Arc::new(
            builder
                .dangerous()
                .with_custom_certificate_verifier(verifier)
                .with_no_client_auth(),
        ));
    }
    let mut roots = RootCertStore::empty();
    if let Some(path) = &options.tls_ca_file {
        let certs = CertificateDer::pem_file_iter(path)
            .and_then(|it| it.collect::<Result<Vec<_>, _>>())
            .map_err(|e| format!("cannot read the CA file {}: {e}", path.display()))?;
        if certs.is_empty() {
            return Err(format!("no certificate in {}", path.display()));
        }
        for c in certs {
            roots
                .add(c)
                .map_err(|e| format!("CA file {}: {e}", path.display()))?;
        }
    } else {
        roots.add_parsable_certificates(rustls_native_certs::load_native_certs().certs);
        if roots.is_empty() {
            return Err(
                "no CA certificates found on this system (set Options::tls_ca_file)".into(),
            );
        }
    }
    Ok(Arc::new(
        builder.with_root_certificates(roots).with_no_client_auth(),
    ))
}

/// Completes the handshake on sock (whose timeouts are already set).
pub(crate) fn handshake(
    mut sock: TcpStream,
    name: &str,
    config: Arc<ClientConfig>,
) -> Result<StreamOwned<ClientConnection, TcpStream>, String> {
    let server_name = ServerName::try_from(name.to_string())
        .map_err(|e| format!("invalid TLS server name {name}: {e}"))?;
    let mut conn =
        ClientConnection::new(config, server_name).map_err(|e| format!("TLS setup: {e}"))?;
    while conn.is_handshaking() {
        conn.complete_io(&mut sock)
            .map_err(|e| format!("TLS handshake failed: {e}"))?;
    }
    Ok(StreamOwned::new(conn, sock))
}

/// Accepts any certificate (Options::tls_verify = false; testing only). Signatures are
/// still checked, so the handshake itself is sound.
#[derive(Debug)]
struct NoVerification(Arc<CryptoProvider>);

impl ServerCertVerifier for NoVerification {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls12_signature(
            message,
            cert,
            dss,
            &self.0.signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(
            message,
            cert,
            dss,
            &self.0.signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.0.signature_verification_algorithms.supported_schemes()
    }
}

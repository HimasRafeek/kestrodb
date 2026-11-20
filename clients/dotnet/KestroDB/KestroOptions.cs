using System;
using System.Net.Security;
using System.Security.Cryptography.X509Certificates;

namespace KestroDB;

/// <summary>Connection settings for <see cref="KestroClient"/>.</summary>
public sealed class KestroOptions
{
    /// <summary>The access token (a server with --token-file or --users); null for none.</summary>
    public string? Token { get; set; }

    /// <summary>
    /// How long to wait for each reply (default 30 s; <see cref="TimeSpan.Zero"/> or
    /// <see cref="System.Threading.Timeout.InfiniteTimeSpan"/>: no limit). Also the connect timeout
    /// unless <see cref="ConnectTimeout"/> is set. A timed-out request closes the connection (IO).
    /// </summary>
    public TimeSpan Timeout { get; set; } = TimeSpan.FromSeconds(30);

    /// <summary>Time allowed to open a connection, TLS handshake included (default: <see cref="Timeout"/>).</summary>
    public TimeSpan? ConnectTimeout { get; set; }

    /// <summary>Reopen a connection the server closed while it was idle (default true). Never inside a transaction.</summary>
    public bool Reconnect { get; set; } = true;

    /// <summary>How long a cluster client keeps looking for the leader (default 15 s).</summary>
    public TimeSpan FailoverTimeout { get; set; } = TimeSpan.FromSeconds(15);

    /// <summary>tls://: trust only the CA certificates in this PEM file (default: the system's trust store).</summary>
    public string? CaFile { get; set; }

    /// <summary>tls://: your own certificate check, instead of <see cref="CaFile"/> / <see cref="Verify"/>.</summary>
    public RemoteCertificateValidationCallback? RemoteCertificateValidation { get; set; }

    /// <summary>tls://: false accepts any certificate and host name (testing only).</summary>
    public bool Verify { get; set; } = true;

    /// <summary>tls://: the name to send (SNI) and check the certificate against, instead of the address's host.</summary>
    public string? ServerName { get; set; }

    /// <summary>tls://: client certificates, for a server that asks for one.</summary>
    public X509CertificateCollection? ClientCertificates { get; set; }

    internal KestroOptions Clone() => (KestroOptions)MemberwiseClone();
}

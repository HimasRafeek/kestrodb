using System;
using System.Data.Common;

namespace KestroDB;

/// <summary>
/// An error from KestroDB or from this client. <see cref="Kind"/> is the leading error code of the
/// message: <c>CONFLICT</c>, <c>INPUT</c>, <c>LIMIT</c>, <c>AUTH</c>, <c>IO</c>, <c>CLOSED</c>,
/// <c>BUSY</c>, <c>READONLY</c>, <c>UNAVAILABLE</c>, <c>CORRUPT</c>, or another code the server sends
/// (<c>STALE</c>, <c>UNSUPPORTED</c>, ...). Each common code has its own subclass; the others are plain
/// <see cref="KestroException"/>s. It is a <see cref="DbException"/>, so ADO.NET code can catch it as one.
/// </summary>
public class KestroException : DbException
{
    public KestroException(string message) : this(message, null) { }

    public KestroException(string message, Exception? innerException) : base(message ?? "ERROR", innerException)
    {
        Kind = KindOf(message);
    }

    /// <summary>The error code: the first word of the message (CONFLICT, INPUT, IO, ...), or ERROR.</summary>
    public string Kind { get; }

    /// <summary>The same as <see cref="Kind"/> (Node calls it <c>code</c>).</summary>
    public string Code => Kind;

    /// <summary>An SQLSTATE for the kind (as the Java JDBC driver uses).</summary>
    public override string? SqlState => Kind switch
    {
        "CONFLICT" => "23000",
        "INPUT" => "42000",
        "LIMIT" => Message.Contains("timeout", StringComparison.Ordinal) ? "57014" : "22000",
        "AUTH" => "28000",
        "IO" => "08006",
        "CLOSED" => "08003",
        "BUSY" => "08004",
        "READONLY" => "25006",
        "UNAVAILABLE" => "08007",
        "CORRUPT" => "XX001",
        _ => "HY000",
    };

    /// <summary>True for errors that may go away when retried: BUSY, READONLY, UNAVAILABLE and IO.</summary>
    public override bool IsTransient => Kind is "BUSY" or "READONLY" or "UNAVAILABLE" or "IO";

    internal static string KindOf(string? message)
    {
        string m = message ?? "";
        int space = m.IndexOf(' ');
        string head = space < 0 ? m : m.Substring(0, space);
        if (head.Length == 0) return "ERROR";
        foreach (char c in head) if (!((c >= 'A' && c <= 'Z') || c == '_')) return "ERROR";
        return head;
    }

    /// <summary>The exception class for a message whose first word is its error code.</summary>
    public static KestroException FromMessage(string? message, Exception? innerException = null)
    {
        string m = string.IsNullOrEmpty(message) ? "ERROR" : message;
        return KindOf(m) switch
        {
            "CONFLICT" => new ConflictException(m, innerException),
            "INPUT" => new InputException(m, innerException),
            "LIMIT" => new LimitException(m, innerException),
            "AUTH" => new AuthException(m, innerException),
            "IO" => new KestroIOException(m, innerException),
            "CLOSED" => new ClosedException(m, innerException),
            "BUSY" => new BusyException(m, innerException),
            "READONLY" => new ReadOnlyException(m, innerException),
            "UNAVAILABLE" => new UnavailableException(m, innerException),
            "CORRUPT" => new CorruptException(m, innerException),
            _ => new KestroException(m, innerException),
        };
    }
}

/// <summary>CONFLICT: a unique constraint, a duplicate id, a failed expectedVersion, or a BEGIN CONCURRENT transaction that lost.</summary>
public class ConflictException : KestroException
{
    public ConflictException(string message, Exception? innerException = null) : base(message, innerException) { }
}

/// <summary>INPUT: invalid SQL, parameters, names or values.</summary>
public class InputException : KestroException
{
    public InputException(string message, Exception? innerException = null) : base(message, innerException) { }
}

/// <summary>LIMIT: a size or count limit (or a statement timeout).</summary>
public class LimitException : KestroException
{
    public LimitException(string message, Exception? innerException = null) : base(message, innerException) { }
}

/// <summary>AUTH: a wrong token, or the user may not do this.</summary>
public class AuthException : KestroException
{
    public AuthException(string message, Exception? innerException = null) : base(message, innerException) { }
}

/// <summary>
/// IO: the connection failed or was lost. A write sent without a reply has an unknown outcome.
/// </summary>
public class KestroIOException : KestroException
{
    public KestroIOException(string message, Exception? innerException = null) : base(message, innerException) { }
}

/// <summary>CLOSED: the client was closed (disposed) by the program.</summary>
public class ClosedException : KestroIOException
{
    public ClosedException(string message, Exception? innerException = null) : base(message, innerException) { }
}

/// <summary>BUSY: the server is at its connection limit (<c>--max-connections</c>).</summary>
public class BusyException : KestroException
{
    public BusyException(string message, Exception? innerException = null) : base(message, innerException) { }
}

/// <summary>READONLY: a follower or replica refused a write (a cluster client retries at the leader).</summary>
public class ReadOnlyException : KestroException
{
    public ReadOnlyException(string message, Exception? innerException = null) : base(message, innerException) { }
}

/// <summary>UNAVAILABLE: a cluster's majority did not confirm a write in time; its outcome is unknown.</summary>
public class UnavailableException : KestroException
{
    public UnavailableException(string message, Exception? innerException = null) : base(message, innerException) { }
}

/// <summary>CORRUPT: the database file failed a checksum.</summary>
public class CorruptException : KestroException
{
    public CorruptException(string message, Exception? innerException = null) : base(message, innerException) { }
}

package dev.kestrodb;

/** {@code IO}: the connection failed or was lost. A write that was sent without a reply has an unknown outcome. */
public class ConnectionException extends KestroException {
    private static final long serialVersionUID = 1L;

    public ConnectionException(String message) { super(message); }

    public ConnectionException(String message, Throwable cause) { super(message, cause); }
}

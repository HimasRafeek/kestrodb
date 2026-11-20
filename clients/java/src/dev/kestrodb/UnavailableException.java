package dev.kestrodb;

/** {@code UNAVAILABLE}: a cluster's majority did not confirm a write in time; its outcome is unknown, as with IO. */
public class UnavailableException extends KestroException {
    private static final long serialVersionUID = 1L;

    public UnavailableException(String message) { super(message); }

    public UnavailableException(String message, Throwable cause) { super(message, cause); }
}

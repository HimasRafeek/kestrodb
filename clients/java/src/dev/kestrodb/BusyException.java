package dev.kestrodb;

/** {@code BUSY}: the server is at its connection limit. */
public class BusyException extends KestroException {
    private static final long serialVersionUID = 1L;

    public BusyException(String message) { super(message); }

    public BusyException(String message, Throwable cause) { super(message, cause); }
}

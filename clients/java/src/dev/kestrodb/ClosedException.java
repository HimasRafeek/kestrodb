package dev.kestrodb;

/** {@code CLOSED}: the program closed the client. A {@link ConnectionException} whose {@code kind()} is CLOSED. */
public class ClosedException extends ConnectionException {
    private static final long serialVersionUID = 1L;

    public ClosedException(String message) { super(message); }

    public ClosedException(String message, Throwable cause) { super(message, cause); }
}

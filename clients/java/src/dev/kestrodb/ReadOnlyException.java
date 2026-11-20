package dev.kestrodb;

/** {@code READONLY}: a follower or replica refused a write (nothing was applied). In a cluster the client retries at the leader first. */
public class ReadOnlyException extends KestroException {
    private static final long serialVersionUID = 1L;

    public ReadOnlyException(String message) { super(message); }

    public ReadOnlyException(String message, Throwable cause) { super(message, cause); }
}

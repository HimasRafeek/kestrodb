package dev.kestrodb;

/**
 * An error from KestroDB or from this client. {@link #kind()} is the leading error code of the
 * message: {@code CONFLICT}, {@code INPUT}, {@code LIMIT}, {@code AUTH}, {@code IO},
 * {@code CLOSED}, {@code BUSY}, {@code READONLY}, {@code UNAVAILABLE}, {@code CORRUPT}, or
 * another code the server sends ({@code STALE}, {@code UNKNOWN_COMMIT}, {@code UNSUPPORTED}, ...).
 * Each common code has its own subclass; the others are plain {@code KestroException}s.
 */
public class KestroException extends RuntimeException {
    private static final long serialVersionUID = 1L;
    private final String kind;

    public KestroException(String message) { this(message, null); }

    public KestroException(String message, Throwable cause) {
        super(message, cause);
        String m = message == null ? "" : message;
        int space = m.indexOf(' ');
        String head = space < 0 ? m : m.substring(0, space);
        this.kind = head.isEmpty() || !head.chars().allMatch(c -> (c >= 'A' && c <= 'Z') || c == '_') ? "ERROR" : head;
    }

    /** The error code: the first word of the message (CONFLICT, INPUT, IO, ...). */
    public String kind() { return kind; }

    /** The same as {@link #kind()} (Node calls it {@code code}). */
    public String code() { return kind; }

    /** The exception class for a message whose first word is its error code. */
    public static KestroException of(String message) { return of(message, null); }

    public static KestroException of(String message, Throwable cause) {
        String m = message == null ? "ERROR" : message;
        int space = m.indexOf(' ');
        switch (space < 0 ? m : m.substring(0, space)) {
            case "CONFLICT": return new ConflictException(m, cause);
            case "INPUT": return new InputException(m, cause);
            case "LIMIT": return new LimitException(m, cause);
            case "AUTH": return new AuthException(m, cause);
            case "IO": return new ConnectionException(m, cause);
            case "CLOSED": return new ClosedException(m, cause);
            case "BUSY": return new BusyException(m, cause);
            case "READONLY": return new ReadOnlyException(m, cause);
            case "UNAVAILABLE": return new UnavailableException(m, cause);
            case "CORRUPT": return new CorruptException(m, cause);
            default: return new KestroException(m, cause);
        }
    }
}

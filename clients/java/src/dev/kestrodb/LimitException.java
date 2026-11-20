package dev.kestrodb;

/** {@code LIMIT}: a size, count or time limit (statement timeout, a reply over 64 MB, ...). */
public class LimitException extends KestroException {
    private static final long serialVersionUID = 1L;

    public LimitException(String message) { super(message); }

    public LimitException(String message, Throwable cause) { super(message, cause); }
}

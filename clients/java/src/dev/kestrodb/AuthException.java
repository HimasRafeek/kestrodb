package dev.kestrodb;

/** {@code AUTH}: a wrong token, or a request the user may not make. */
public class AuthException extends KestroException {
    private static final long serialVersionUID = 1L;

    public AuthException(String message) { super(message); }

    public AuthException(String message, Throwable cause) { super(message, cause); }
}

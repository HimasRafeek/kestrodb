package dev.kestrodb;

/** {@code INPUT}: invalid SQL, parameters, names or values. */
public class InputException extends KestroException {
    private static final long serialVersionUID = 1L;

    public InputException(String message) { super(message); }

    public InputException(String message, Throwable cause) { super(message, cause); }
}

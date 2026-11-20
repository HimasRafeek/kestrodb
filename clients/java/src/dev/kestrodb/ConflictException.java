package dev.kestrodb;

/** {@code CONFLICT}: a unique constraint, a duplicate id, or a failed {@code expectedVersion}; a COMMIT that fails this way rolled the transaction back. */
public class ConflictException extends KestroException {
    private static final long serialVersionUID = 1L;

    public ConflictException(String message) { super(message); }

    public ConflictException(String message, Throwable cause) { super(message, cause); }
}

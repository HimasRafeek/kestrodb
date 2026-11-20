package dev.kestrodb;

/** {@code CORRUPT}: the file failed a checksum. */
public class CorruptException extends KestroException {
    private static final long serialVersionUID = 1L;

    public CorruptException(String message) { super(message); }

    public CorruptException(String message, Throwable cause) { super(message, cause); }
}

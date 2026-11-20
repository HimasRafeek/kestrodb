package dev.kestrodb;

import java.util.Objects;

/** A stored key/value: its version (the transaction that wrote it) and its JSON value. */
public final class Versioned {
    private final long version;
    private final Object value;

    public Versioned(long version, Object value) {
        this.version = version;
        this.value = value;
    }

    /** The transaction id that last wrote the key; pass it as expectedVersion to update only if unchanged. */
    public long version() { return version; }

    /** The decoded JSON value (Map, List, String, Long, Double, Boolean or null). */
    public Object value() { return value; }

    @Override public boolean equals(Object o) {
        if (!(o instanceof Versioned)) return false;
        Versioned v = (Versioned) o;
        return v.version == version && Objects.equals(v.value, value);
    }

    @Override public int hashCode() { return Long.hashCode(version) * 31 + Objects.hashCode(value); }

    @Override public String toString() { return "{\"version\":" + version + ",\"value\":" + Json.stringify(value) + "}"; }
}

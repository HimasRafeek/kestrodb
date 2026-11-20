package dev.kestrodb;

/**
 * One operation of an all-or-nothing {@link KestroClient#batch} transaction.
 * An expected version makes it conditional: the whole batch fails with
 * {@link ConflictException} unless the key is at that version (0: the key must not exist).
 */
public final class Op {
    /** No condition. */
    public static final long ANY_VERSION = -1;

    final boolean set;
    final String collection;
    final String key;
    final Object value;
    final long expectedVersion;

    private Op(boolean set, String collection, String key, Object value, long expectedVersion) {
        if (expectedVersion < -1) throw new InputException("INPUT expectedVersion must be 0 or more");
        this.set = set;
        this.collection = collection;
        this.key = key;
        this.value = value;
        this.expectedVersion = expectedVersion;
    }

    public static Op set(String collection, String key, Object value) { return new Op(true, collection, key, value, ANY_VERSION); }

    public static Op set(String collection, String key, Object value, long expectedVersion) { return new Op(true, collection, key, value, expectedVersion); }

    public static Op delete(String collection, String key) { return new Op(false, collection, key, null, ANY_VERSION); }

    public static Op delete(String collection, String key, long expectedVersion) { return new Op(false, collection, key, null, expectedVersion); }

    public boolean isSet() { return set; }

    public String collection() { return collection; }

    public String key() { return key; }

    public Object value() { return value; }

    /** The required version, or {@link #ANY_VERSION}. */
    public long expectedVersion() { return expectedVersion; }
}

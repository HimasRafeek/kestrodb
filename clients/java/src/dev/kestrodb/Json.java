package dev.kestrodb;

import java.io.ByteArrayOutputStream;
import java.lang.reflect.Array;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;

/**
 * A small, strict JSON reader and writer (RFC 8259), with no dependencies.
 *
 * <p>Reading: objects become {@link LinkedHashMap} (key order kept), arrays {@link ArrayList},
 * strings {@link String}, {@code true/false} {@link Boolean}, {@code null} null, and numbers
 * {@link Long} when integral (no fraction or exponent; {@link BigInteger} beyond the long range),
 * otherwise {@link Double}.
 *
 * <p>Writing: null, Boolean, String/CharSequence/Character, Number (integral types exactly;
 * float/double must be finite; BigDecimal as written), Map (keys as strings), Iterable, arrays
 * (except byte[]), Optional, Enum (its name), and anything else whose {@code toString()} is the
 * intended text value: java.time values, UUID. Strings are written as UTF-8; lone surrogates are
 * refused because they are not valid Unicode.
 */
public final class Json {
    private Json() {}

    /** Nesting limit for reading and writing (also catches cyclic structures). */
    public static final int MAX_DEPTH = 1000;

    // ---------------------------------------------------------------- reading

    /** Parses one JSON value from UTF-8 bytes. */
    public static Object parse(byte[] utf8, int offset, int length) {
        return parse(new String(utf8, offset, length, StandardCharsets.UTF_8));
    }

    /** Parses one JSON value; trailing non-whitespace is an error. */
    public static Object parse(String text) {
        Reader r = new Reader(text);
        r.skipSpace();
        Object v = r.value(0);
        r.skipSpace();
        if (r.pos != text.length()) throw r.error("unexpected text after the value");
        return v;
    }

    private static final class Reader {
        final String s;
        int pos;

        Reader(String s) { this.s = s; }

        InputException error(String what) {
            return new InputException("INPUT invalid JSON at offset " + pos + ": " + what);
        }

        void skipSpace() {
            while (pos < s.length()) {
                char c = s.charAt(pos);
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos++;
                else break;
            }
        }

        Object value(int depth) {
            if (depth > MAX_DEPTH) throw error("nesting deeper than " + MAX_DEPTH);
            if (pos >= s.length()) throw error("unexpected end");
            char c = s.charAt(pos);
            switch (c) {
                case '{': return object(depth);
                case '[': return array(depth);
                case '"': return string();
                case 't': literal("true"); return Boolean.TRUE;
                case 'f': literal("false"); return Boolean.FALSE;
                case 'n': literal("null"); return null;
                default:
                    if (c == '-' || (c >= '0' && c <= '9')) return number();
                    throw error("unexpected character '" + c + "'");
            }
        }

        void literal(String word) {
            if (!s.startsWith(word, pos)) throw error("expected " + word);
            pos += word.length();
        }

        Map<String, Object> object(int depth) {
            pos++; // {
            Map<String, Object> map = new LinkedHashMap<>();
            skipSpace();
            if (pos < s.length() && s.charAt(pos) == '}') { pos++; return map; }
            for (;;) {
                skipSpace();
                if (pos >= s.length() || s.charAt(pos) != '"') throw error("expected a string key");
                String key = string();
                skipSpace();
                if (pos >= s.length() || s.charAt(pos) != ':') throw error("expected ':'");
                pos++;
                skipSpace();
                map.put(key, value(depth + 1));
                skipSpace();
                if (pos >= s.length()) throw error("unexpected end in an object");
                char c = s.charAt(pos++);
                if (c == '}') return map;
                if (c != ',') { pos--; throw error("expected ',' or '}'"); }
            }
        }

        List<Object> array(int depth) {
            pos++; // [
            List<Object> list = new ArrayList<>();
            skipSpace();
            if (pos < s.length() && s.charAt(pos) == ']') { pos++; return list; }
            for (;;) {
                skipSpace();
                list.add(value(depth + 1));
                skipSpace();
                if (pos >= s.length()) throw error("unexpected end in an array");
                char c = s.charAt(pos++);
                if (c == ']') return list;
                if (c != ',') { pos--; throw error("expected ',' or ']'"); }
            }
        }

        String string() {
            pos++; // opening quote
            int start = pos;
            // Fast path: no escapes.
            while (pos < s.length()) {
                char c = s.charAt(pos);
                if (c == '"') { String out = s.substring(start, pos); pos++; return out; }
                if (c == '\\') break;
                if (c < 0x20) throw error("control character in a string");
                pos++;
            }
            StringBuilder b = new StringBuilder(s.length() - start);
            b.append(s, start, pos);
            while (pos < s.length()) {
                char c = s.charAt(pos++);
                if (c == '"') return b.toString();
                if (c < 0x20) { pos--; throw error("control character in a string"); }
                if (c != '\\') { b.append(c); continue; }
                if (pos >= s.length()) break;
                char e = s.charAt(pos++);
                switch (e) {
                    case '"': b.append('"'); break;
                    case '\\': b.append('\\'); break;
                    case '/': b.append('/'); break;
                    case 'b': b.append('\b'); break;
                    case 'f': b.append('\f'); break;
                    case 'n': b.append('\n'); break;
                    case 'r': b.append('\r'); break;
                    case 't': b.append('\t'); break;
                    case 'u': {
                        // \\uD83D\\uDE00 pairs become one code point naturally: Java strings are UTF-16.
                        if (pos + 4 > s.length()) throw error("short \\u escape");
                        int v = 0;
                        for (int i = 0; i < 4; i++) {
                            int d = Character.digit(s.charAt(pos + i), 16);
                            if (d < 0) throw error("bad \\u escape");
                            v = v * 16 + d;
                        }
                        pos += 4;
                        b.append((char) v);
                        break;
                    }
                    default: pos--; throw error("bad escape \\" + e);
                }
            }
            throw error("unterminated string");
        }

        Object number() {
            int start = pos;
            boolean integral = true;
            if (s.charAt(pos) == '-') pos++;
            if (pos >= s.length()) throw error("bad number");
            if (s.charAt(pos) == '0') pos++;
            else if (s.charAt(pos) >= '1' && s.charAt(pos) <= '9') { while (pos < s.length() && isDigit(s.charAt(pos))) pos++; }
            else throw error("bad number");
            if (pos < s.length() && s.charAt(pos) == '.') {
                integral = false;
                pos++;
                if (pos >= s.length() || !isDigit(s.charAt(pos))) throw error("bad number fraction");
                while (pos < s.length() && isDigit(s.charAt(pos))) pos++;
            }
            if (pos < s.length() && (s.charAt(pos) == 'e' || s.charAt(pos) == 'E')) {
                integral = false;
                pos++;
                if (pos < s.length() && (s.charAt(pos) == '+' || s.charAt(pos) == '-')) pos++;
                if (pos >= s.length() || !isDigit(s.charAt(pos))) throw error("bad number exponent");
                while (pos < s.length() && isDigit(s.charAt(pos))) pos++;
            }
            String t = s.substring(start, pos);
            if (integral) {
                if (t.length() < 19) return Long.parseLong(t);
                BigInteger big = new BigInteger(t);
                return big.bitLength() < 64 ? (Object) big.longValue() : big;
            }
            return Double.parseDouble(t);
        }

        static boolean isDigit(char c) { return c >= '0' && c <= '9'; }
    }

    // ---------------------------------------------------------------- writing

    /** Compact JSON text for a value. */
    public static String stringify(Object value) {
        StringBuilder b = new StringBuilder();
        write(b, value, 0);
        return b.toString();
    }

    /** Compact JSON as UTF-8 bytes. */
    public static byte[] toBytes(Object value) {
        String text = stringify(value);
        return utf8(text);
    }

    /** UTF-8 of a string; a lone surrogate is an {@link InputException} (not valid Unicode). */
    static byte[] utf8(String text) {
        // Strict: String.getBytes would silently turn a lone surrogate into '?'.
        int n = text.length();
        ByteArrayOutputStream out = new ByteArrayOutputStream(n + 16);
        for (int i = 0; i < n; i++) {
            char c = text.charAt(i);
            if (c < 0x80) out.write(c);
            else if (c < 0x800) { out.write(0xC0 | (c >> 6)); out.write(0x80 | (c & 0x3F)); }
            else if (Character.isHighSurrogate(c)) {
                if (i + 1 >= n || !Character.isLowSurrogate(text.charAt(i + 1))) throw new InputException("INPUT text is not valid Unicode (a lone surrogate)");
                int cp = Character.toCodePoint(c, text.charAt(++i));
                out.write(0xF0 | (cp >> 18)); out.write(0x80 | ((cp >> 12) & 0x3F)); out.write(0x80 | ((cp >> 6) & 0x3F)); out.write(0x80 | (cp & 0x3F));
            } else if (Character.isLowSurrogate(c)) throw new InputException("INPUT text is not valid Unicode (a lone surrogate)");
            else { out.write(0xE0 | (c >> 12)); out.write(0x80 | ((c >> 6) & 0x3F)); out.write(0x80 | (c & 0x3F)); }
        }
        return out.toByteArray();
    }

    private static void write(StringBuilder b, Object v, int depth) {
        if (depth > MAX_DEPTH) throw new InputException("INPUT JSON nesting deeper than " + MAX_DEPTH + " (a cycle?)");
        if (v == null) { b.append("null"); return; }
        if (v instanceof String) { quote(b, (String) v); return; }
        if (v instanceof Boolean) { b.append(((Boolean) v) ? "true" : "false"); return; }
        if (v instanceof Integer || v instanceof Long || v instanceof Short || v instanceof Byte
                || v instanceof BigInteger || v instanceof java.util.concurrent.atomic.AtomicInteger
                || v instanceof java.util.concurrent.atomic.AtomicLong) { b.append(v.toString()); return; }
        if (v instanceof Double || v instanceof Float) {
            double d = ((Number) v).doubleValue();
            if (Double.isNaN(d) || Double.isInfinite(d)) throw new InputException("INPUT " + d + " is not a JSON number");
            // Float.toString keeps a float's shortest text (0.1f -> 0.1, not 0.10000000149011612).
            b.append(v instanceof Float ? Float.toString((Float) v) : Double.toString(d));
            return;
        }
        if (v instanceof BigDecimal) { b.append(((BigDecimal) v).toString()); return; }
        if (v instanceof Number) {
            double d = ((Number) v).doubleValue();
            if (Double.isNaN(d) || Double.isInfinite(d)) throw new InputException("INPUT " + d + " is not a JSON number");
            b.append(new BigDecimal(v.toString()).toString());
            return;
        }
        if (v instanceof CharSequence || v instanceof Character) { quote(b, v.toString()); return; }
        if (v instanceof Map) {
            b.append('{');
            boolean first = true;
            for (Map.Entry<?, ?> e : ((Map<?, ?>) v).entrySet()) {
                if (e.getKey() == null) throw new InputException("INPUT a JSON object key cannot be null");
                if (!first) b.append(',');
                first = false;
                quote(b, e.getKey().toString());
                b.append(':');
                write(b, e.getValue(), depth + 1);
            }
            b.append('}');
            return;
        }
        if (v instanceof Iterable) {
            b.append('[');
            boolean first = true;
            for (Object item : (Iterable<?>) v) {
                if (!first) b.append(',');
                first = false;
                write(b, item, depth + 1);
            }
            b.append(']');
            return;
        }
        if (v instanceof byte[]) throw new InputException("INPUT byte[] is not a JSON value; encode it first (for example Base64 text)");
        if (v.getClass().isArray()) {
            b.append('[');
            int n = Array.getLength(v);
            for (int i = 0; i < n; i++) {
                if (i > 0) b.append(',');
                write(b, Array.get(v, i), depth + 1);
            }
            b.append(']');
            return;
        }
        if (v instanceof Optional) { write(b, ((Optional<?>) v).orElse(null), depth + 1); return; }
        if (v instanceof Enum) { quote(b, ((Enum<?>) v).name()); return; }
        if (v instanceof java.time.temporal.TemporalAccessor || v instanceof java.util.UUID
                || v instanceof java.time.Duration || v instanceof java.time.Period) { quote(b, v.toString()); return; }
        // java.sql types as SQL text: DATE 2026-11-20, TIME 10:15:30, TIMESTAMP 2026-11-20T10:15:30.5
        if (v instanceof java.sql.Date) { quote(b, ((java.sql.Date) v).toLocalDate().toString()); return; }
        if (v instanceof java.sql.Time) { quote(b, ((java.sql.Time) v).toLocalTime().toString()); return; }
        if (v instanceof java.sql.Timestamp) { quote(b, ((java.sql.Timestamp) v).toLocalDateTime().toString()); return; }
        if (v instanceof java.util.Date) { quote(b, ((java.util.Date) v).toInstant().toString()); return; }
        throw new InputException("INPUT " + v.getClass().getName() + " is not a JSON value");
    }

    private static final char[] HEX = "0123456789abcdef".toCharArray();

    /** Appends s as a JSON string. Non-ASCII stays as it is (the text is sent as UTF-8). */
    static void quote(StringBuilder b, String s) {
        b.append('"');
        int n = s.length();
        for (int i = 0; i < n; i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"': b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\t': b.append("\\t"); break;
                case '\b': b.append("\\b"); break;
                case '\f': b.append("\\f"); break;
                default:
                    if (c < 0x20) {
                        b.append("\\u00").append(HEX[c >> 4]).append(HEX[c & 15]);
                    } else if (Character.isSurrogate(c)) {
                        if (Character.isHighSurrogate(c) && i + 1 < n && Character.isLowSurrogate(s.charAt(i + 1))) {
                            b.append(c).append(s.charAt(++i));
                        } else {
                            throw new InputException("INPUT text is not valid Unicode (a lone surrogate)");
                        }
                    } else {
                        b.append(c);
                    }
            }
        }
        b.append('"');
    }

}

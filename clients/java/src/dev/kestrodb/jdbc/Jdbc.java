package dev.kestrodb.jdbc;

import dev.kestrodb.KestroException;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.sql.SQLDataException;
import java.sql.SQLException;
import java.sql.SQLFeatureNotSupportedException;
import java.sql.SQLIntegrityConstraintViolationException;
import java.sql.SQLInvalidAuthorizationSpecException;
import java.sql.SQLNonTransientConnectionException;
import java.sql.SQLNonTransientException;
import java.sql.SQLRecoverableException;
import java.sql.SQLSyntaxErrorException;
import java.sql.SQLTimeoutException;
import java.sql.SQLTransientConnectionException;
import java.sql.SQLTransientException;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;

/**
 * The JDBC objects are proxies of a JDBC interface onto a small implementation class: a method
 * the class has (same name and parameter types) is called, every other method fails with
 * {@link SQLFeatureNotSupportedException}. KestroDB errors become SQLExceptions with a SQLState
 * and the {@link KestroException} as cause.
 */
final class Jdbc {
    private Jdbc() {}

    private static final ClassValue<Map<Method, Optional<Method>>> TARGETS = new ClassValue<>() {
        @Override protected Map<Method, Optional<Method>> computeValue(Class<?> type) { return new ConcurrentHashMap<>(); }
    };

    @SuppressWarnings("unchecked")
    static <T> T wrap(Class<T> iface, Object impl) {
        return (T) Proxy.newProxyInstance(Jdbc.class.getClassLoader(), new Class<?>[] {iface}, new Handler(iface, impl));
    }

    private static final class Handler implements InvocationHandler {
        final Class<?> iface;
        final Object impl;

        Handler(Class<?> iface, Object impl) { this.iface = iface; this.impl = impl; }

        @Override
        public Object invoke(Object proxy, Method m, Object[] args) throws Throwable {
            String name = m.getName();
            int n = args == null ? 0 : args.length;
            if (m.getDeclaringClass() == Object.class) {
                switch (name) {
                    case "equals": return proxy == args[0];
                    case "hashCode": return System.identityHashCode(proxy);
                    case "toString": return iface.getSimpleName() + "(" + impl + ")";
                    default: break;
                }
            }
            if (name.equals("isWrapperFor") && n == 1) {
                Class<?> want = (Class<?>) args[0];
                return want.isInstance(proxy) || want.isInstance(impl) || unwrapTarget(want) != null;
            }
            if (name.equals("unwrap") && n == 1) {
                Class<?> want = (Class<?>) args[0];
                if (want.isInstance(proxy)) return proxy;
                if (want.isInstance(impl)) return impl;
                Object inner = unwrapTarget(want);
                if (inner != null) return inner;
                throw new SQLException("not a wrapper for " + want.getName());
            }
            Optional<Method> target = TARGETS.get(impl.getClass()).computeIfAbsent(m, k -> {
                try {
                    return Optional.of(impl.getClass().getMethod(k.getName(), k.getParameterTypes()));
                } catch (NoSuchMethodException e) {
                    return Optional.empty();
                }
            });
            if (target.isEmpty()) throw new SQLFeatureNotSupportedException(iface.getSimpleName() + "." + name + " is not supported by the KestroDB driver");
            try {
                return target.get().invoke(impl, args);
            } catch (InvocationTargetException e) {
                Throwable cause = e.getCause();
                if (cause instanceof KestroException) throw sql((KestroException) cause);
                throw cause;
            }
        }

        private Object unwrapTarget(Class<?> want) {
            if (impl instanceof Unwrappable) {
                Object inner = ((Unwrappable) impl).inner();
                if (want.isInstance(inner)) return inner;
            }
            return null;
        }
    }

    /** An implementation that can hand out the KestroDB object behind it (unwrap). */
    interface Unwrappable { Object inner(); }

    /** A SQLException for a KestroDB error: class and SQLState by its code. */
    static SQLException sql(KestroException e) {
        String m = e.getMessage();
        String kind = e.kind();
        switch (kind) {
            case "CONFLICT": return new SQLIntegrityConstraintViolationException(m, "23000", 0, e);
            case "INPUT": return new SQLSyntaxErrorException(m, "42000", 0, e);
            case "LIMIT":
                if (m != null && m.contains("timeout")) return new SQLTimeoutException(m, "57014", 0, e);
                return new SQLDataException(m, "22000", 0, e);
            case "AUTH": return new SQLInvalidAuthorizationSpecException(m, "28000", 0, e);
            case "IO": return new SQLRecoverableException(m, "08006", 0, e);
            case "CLOSED": return new SQLNonTransientConnectionException(m, "08003", 0, e);
            case "BUSY": return new SQLTransientConnectionException(m, "08004", 0, e);
            case "READONLY": return new SQLNonTransientException(m, "25006", 0, e);
            case "UNAVAILABLE": return new SQLTransientException(m, "08007", 0, e);
            case "CORRUPT": return new SQLNonTransientException(m, "XX001", 0, e);
            default: return new SQLException(m, "HY000", 0, e);
        }
    }
}

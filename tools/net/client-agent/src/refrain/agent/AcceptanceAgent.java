// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

package refrain.agent;

import java.awt.Robot;
import java.awt.event.KeyEvent;
import java.io.IOException;
import java.lang.instrument.Instrumentation;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Watchdog loaded into the vanilla client through {@code -javaagent}.
 *
 * <p>The client is obfuscated, so the agent does not reflect into its classes.
 * It watches the client log for the join sequence and drives the client
 * through {@link Robot}, then writes a report and halts the JVM with an exit
 * code the launcher can assert on.
 *
 * <p>Agent arguments are comma separated {@code key=value} pairs:
 *
 * <ul>
 *   <li>{@code log} - client {@code logs/latest.log} to watch (required)
 *   <li>{@code report} - JSON report path (required); the launcher reads it
 *   <li>{@code timeout} - seconds until the join is considered stalled
 *   <li>{@code settle} - seconds to wait after the level loads
 *   <li>{@code keys} - comma separated {@link KeyEvent} names to press after
 *       the settle delay, for example {@code E} or {@code E+ESCAPE}
 * </ul>
 */
public final class AcceptanceAgent {
    private static final String CONNECTING = "Connecting to";
    private static final String LEVEL_LOADED = "Loaded ";
    private static final List<String> FAILURES =
            List.of("Failed to connect", "Connection Lost", "Disconnected",
                    "Internal Exception", "io.netty.channel",
                    "io.netty.handler", "Connection refused");

    private AcceptanceAgent() {
    }

    public static void premain(String arguments, Instrumentation ignored) {
        final Options options;
        try {
            options = Options.parse(arguments);
        } catch (RuntimeException error) {
            System.err.println("refrain-agent: bad arguments: "
                    + error.getMessage());
            Runtime.getRuntime().halt(2);
            return;
        }
        final Thread watchdog = new Thread(() -> run(options), "refrain-agent");
        watchdog.setDaemon(true);
        watchdog.start();
    }

    private static void run(Options options) {
        try {
            final Outcome outcome = awaitJoin(options);
            if (!outcome.joined) {
                report(options, outcome.status, outcome.detail, 0);
                Runtime.getRuntime().halt(1);
                return;
            }
            final long joinedAt = System.nanoTime();
            Thread.sleep(options.settleSeconds * 1000L);
            final String input = pressKeys(options);
            final long settledMs = (System.nanoTime() - joinedAt) / 1_000_000L;
            report(options, "joined", input, settledMs);
            // Hold the client alive until the launcher has taken its
            // screenshot, then leave without running the client shutdown
            // hooks: the world is not this run's output.
            final Path release = Path.of(options.report + ".release");
            final long deadline = System.nanoTime() + 30_000_000_000L;
            while (System.nanoTime() < deadline && !Files.exists(release)) {
                Thread.sleep(100);
            }
            Runtime.getRuntime().halt(0);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            Runtime.getRuntime().halt(1);
        } catch (RuntimeException | IOException error) {
            try {
                report(options, "error", String.valueOf(error), 0);
            } catch (IOException ignored) {
                // The launcher also treats a missing report as a failure.
            }
            Runtime.getRuntime().halt(1);
        }
    }

    private static Outcome awaitJoin(Options options) throws IOException {
        final Path log = Path.of(options.log);
        final long deadline =
                System.nanoTime() + options.timeoutSeconds * 1_000_000_000L;
        boolean connecting = false;
        // The run directory keeps the log of earlier launches; only lines
        // written after the agent starts belong to this run.
        long offset = Files.exists(log) ? Files.size(log) : 0;
        while (System.nanoTime() < deadline) {
            if (Files.exists(log)) {
                final String text = read(log);
                if (text.length() < offset) {
                    offset = 0;
                }
                final String fresh = text.substring((int) offset);
                offset = text.length();
                for (final String failure : FAILURES) {
                    final int at = fresh.indexOf(failure);
                    if (at >= 0) {
                        return new Outcome(false, "failed",
                                firstLine(fresh, at));
                    }
                }
                if (!connecting && fresh.contains(CONNECTING)) {
                    connecting = true;
                }
                if (connecting && fresh.contains(LEVEL_LOADED)
                        && fresh.contains(" advancements")) {
                    return new Outcome(true, "joined", "");
                }
            }
            try {
                Thread.sleep(200);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return new Outcome(false, "failed", "interrupted");
            }
        }
        return new Outcome(false, "timeout", connecting
                ? "the level never loaded"
                : "the client never started connecting");
    }

    private static String read(Path log) throws IOException {
        return Files.readString(log, StandardCharsets.UTF_8);
    }

    private static String firstLine(String text, int at) {
        int end = text.indexOf('\n', at);
        if (end < 0) {
            end = text.length();
        }
        return text.substring(at, end).trim();
    }

    /**
     * Press the configured keys with {@link Robot}. Input is best effort: a
     * display without AWT support reports the keys as unavailable instead of
     * failing the join.
     */
    private static String pressKeys(Options options) {
        if (options.keys.isEmpty()) {
            return "none";
        }
        try {
            final Robot robot = new Robot();
            robot.setAutoDelay(60);
            for (final String name : options.keys) {
                final int code = keyCode(name);
                if (code < 0) {
                    return "unknown:" + name;
                }
                robot.keyPress(code);
                robot.keyRelease(code);
                robot.delay(400);
            }
            return "pressed:" + String.join(",", options.keys);
        } catch (RuntimeException | java.awt.AWTError | java.awt.AWTException error) {
            return "unavailable";
        }
    }

    private static int keyCode(String name) {
        try {
            return KeyEvent.class.getField("VK_" + name.toUpperCase(Locale.ROOT))
                    .getInt(null);
        } catch (ReflectiveOperationException missing) {
            return -1;
        }
    }

    private static void report(Options options, String status, String detail,
                               long settledMs) throws IOException {
        final Path path = Path.of(options.report);
        final Path parent = path.getParent();
        if (parent != null) {
            Files.createDirectories(parent);
        }
        final String json = String.format(Locale.ROOT, """
                {
                  "status": "%s",
                  "detail": "%s",
                  "settled_ms": %d,
                  "server": "%s",
                  "finished_at": "%s"
                }
                """, escape(status), escape(detail), settledMs,
                escape(options.server), Instant.now());
        Files.writeString(path, json, StandardCharsets.UTF_8,
                StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
    }

    private static String escape(String text) {
        return text.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    private record Outcome(boolean joined, String status, String detail) {
    }

    private record Options(String log, String report, String server,
                           int timeoutSeconds, int settleSeconds,
                           List<String> keys) {
        static Options parse(String arguments) {
            final List<String> parts = new ArrayList<>();
            if (arguments != null) {
                for (final String part : arguments.split(",")) {
                    if (!part.isBlank()) {
                        parts.add(part.trim());
                    }
                }
            }
            String log = null;
            String report = null;
            String server = "";
            int timeout = 180;
            int settle = 8;
            final List<String> keys = new ArrayList<>();
            for (final String part : parts) {
                final int equals = part.indexOf('=');
                if (equals < 0) {
                    throw new IllegalArgumentException("expected key=value: " + part);
                }
                final String key = part.substring(0, equals);
                final String value = part.substring(equals + 1);
                switch (key) {
                    case "log" -> log = value;
                    case "report" -> report = value;
                    case "server" -> server = value;
                    case "timeout" -> timeout = Integer.parseInt(value);
                    case "settle" -> settle = Integer.parseInt(value);
                    case "keys" -> {
                        for (final String name : value.split("\\+")) {
                            if (!name.isBlank()) {
                                keys.add(name.trim());
                            }
                        }
                    }
                    default -> throw new IllegalArgumentException("unknown key: " + key);
                }
            }
            if (log == null || report == null) {
                throw new IllegalArgumentException("log and report are required");
            }
            return new Options(log, report, server, timeout, settle, keys);
        }
    }
}

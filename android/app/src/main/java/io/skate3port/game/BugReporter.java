package io.skate3port.game;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.ApplicationExitInfo;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;
import android.os.StatFs;
import android.provider.MediaStore;
import android.system.Os;
import android.system.OsConstants;
import android.view.InputDevice;
import android.widget.Toast;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.TreeSet;
import java.util.zip.GZIPInputStream;

final class BugReporter {
    private static final String ISSUE_URL =
        "https://github.com/darchap/Skate3-Port/issues/new";
    // GitHub answers 500 above ~7000 characters of prefill URL. Measured, not documented.
    private static final int MAX_URL_LENGTH = 6500;
    private static final String SHORTENED_NOTE =
        "\n[Shortened for the browser. The full report is on the clipboard and the full log " +
        "is in Downloads.]";
    private static final int MAX_IMPORTANT_LINES = 72;
    private static final int LATEST_LINES = 12;

    private BugReporter() {}

    static void show(Activity activity, boolean gameInstalled) {
        // Reads up to 640 KB of log, parses tombstones and copies a file: off the UI thread.
        new Thread(() -> {
            Diagnostic diagnostic;
            try {
                diagnostic = collect(activity, gameInstalled);
            } catch (Exception exception) {
                // An unchecked throw here (StatFs on ejected storage) would kill the process.
                activity.runOnUiThread(() -> Toast.makeText(activity,
                    "Could not build the report: " + clean(exception.getClass().getSimpleName()),
                    Toast.LENGTH_LONG).show());
                return;
            }
            activity.runOnUiThread(() -> {
                if (activity.isFinishing() || activity.isDestroyed()) return;
                new AlertDialog.Builder(activity)
                    .setTitle("Report a problem")
                    .setMessage("GitHub will open with this device's technical details already filled in. If the form opens empty, paste into the Diagnostics box: the same details are on your clipboard. The newest game log is saved to Downloads so you can attach it.\n\nNo ISO, game file, save, account name, or private path is included.")
                    .setNegativeButton("Cancel", null)
                    .setNeutralButton("Copy only", (dialog, which) -> copy(activity, diagnostic.report))
                    .setPositiveButton("Open GitHub", (dialog, which) -> {
                        copy(activity, diagnostic.report);
                        open(activity, diagnostic);
                    })
                    .show();
            });
        }).start();
    }

    private static Diagnostic collect(Context context, boolean gameInstalled) {
        String version = "unknown";
        try {
            PackageInfo info = context.getPackageManager()
                .getPackageInfo(context.getPackageName(), 0);
            version = "v" + info.versionName + " (" + info.getLongVersionCode() + ")";
        } catch (Exception ignored) {
        }

        String device = clean(Build.MANUFACTURER + " " + Build.MODEL);
        String android = "Android " + Build.VERSION.RELEASE + " (API " +
                         Build.VERSION.SDK_INT + ")";
        String soc = clean(Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL);
        if (soc.isEmpty()) soc = clean(Build.HARDWARE);
        GpuDriverInfo imported = GpuDriver.installedDriver(context);
        String gpuDriver = imported == null ? "System"
            : imported.isEnabled() ? imported.label()
            : "System (imported, not selected: " + imported.label() + ")";
        String input = inputMethod();
        String exits = recentExits(context);
        File log = newestLog(context);
        List<String> evidence = runtimeEvidence(context, log);
        long pageSize = Os.sysconf(OsConstants._SC_PAGESIZE);
        String vulkan = vulkanVersion(context);
        long availableMb = Runtime.getRuntime().maxMemory() / (1024 * 1024);
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        context.getSystemService(ActivityManager.class).getMemoryInfo(memory);
        File storageRoot = context.getExternalFilesDir(null);
        if (storageRoot == null) storageRoot = context.getFilesDir();
        File userData = new File(context.getFilesDir(), "skate3");
        String settings = readSmallFile(new File(userData, "settings.toml"));
        String bundleName = "skate3-report-" +
            new SimpleDateFormat("yyyyMMdd-HHmm", Locale.US).format(new Date()) + ".txt";

        String header =
            "Skate 3 diagnostics\n" +
            "App: " + version + "\n" +
            "Package: " + context.getPackageName() + "\n" +
            "Device: " + device + "\n" +
            "Android: " + android + "\n" +
            "SoC: " + soc + "\n" +
            "Hardware: " + clean(Build.HARDWARE) + "\n" +
            "Build: " + clean(Build.FINGERPRINT) + "\n" +
            "CPU: " + cpuLayout() + "\n" +
            "Game folder: " + mountType(storageRoot) + "\n" +
            "ABI: " + String.join(", ", Build.SUPPORTED_ABIS) + "\n" +
            "Memory page: " + pageSize + " bytes\n" +
            "Vulkan feature: " + vulkan + "\n" +
            "Java heap limit: " + availableMb + " MiB\n" +
            "RAM: " + memory.totalMem / (1024 * 1024) + " MiB\n" +
            "Free storage: " + GameFiles.humanBytes(
                new StatFs(storageRoot.getAbsolutePath()).getAvailableBytes()) + "\n" +
            "GPU driver: " + gpuDriver + "\n" +
            "Input: " + input + "\n" +
            "Game installed: " + (gameInstalled ? "yes" : "no") + "\n" +
            "Reached gameplay before: " +
            (new File(userData, ".reached-gameplay").isFile() ? "yes" : "no") + "\n" +
            "Full report with settings and log: Downloads/" + bundleName +
            " (attach it to this issue)\n" +
            "Recent process exits:\n" + exits + "\n\n" +
            (gameInstalled ? "" : "Setup log (last lines):\n" +
                lastLines(context, readSmallFile(new File(context.getFilesDir(), "phone-setup.log")), 5) +
                "\n\n") +
            "Settings (rendering):\n" + settingsSummary(settings) + "\n\n" +
            "Log (" + (log == null ? "none" : log.getName() + ", " + log.length() / 1024 + " KB") +
            "):\n" + (log == null ? "" : firstLines(context, log) + "\n") +
            "Warnings and errors, then the latest lines:\n";
        Diagnostic diagnostic =
            new Diagnostic(version, device, android, soc, gpuDriver, input, header, evidence);
        saveBundle(context, bundleName, diagnostic.report, settings, log);
        return diagnostic;
    }

    private static String readSmallFile(File file) {
        try {
            if (!file.isFile() || file.length() > 64 * 1024) return "";
            return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
        } catch (Exception ignored) {
            return "";
        }
    }

    private static String lastLines(Context context, String text, int count) {
        String[] lines = text.trim().split("\\R");
        StringBuilder out = new StringBuilder();
        for (int i = Math.max(0, lines.length - count); i < lines.length; i++) {
            out.append(sanitizePath(context, lines[i])).append('\n');
        }
        return out.toString().trim();
    }

    // The first lines name the native build and its git hash; they stay in the header
    // so the browser budget can never drop them.
    private static String firstLines(Context context, File log) {
        try (RandomAccessFile input = new RandomAccessFile(log, "r")) {
            byte[] head = new byte[(int)Math.min(log.length(), 4096)];
            input.readFully(head);
            String[] lines = new String(head, StandardCharsets.UTF_8).split("\\R");
            StringBuilder out = new StringBuilder();
            for (int i = 0; i < Math.min(3, lines.length); i++) {
                if (!lines[i].isEmpty()) out.append(sanitizePath(context, lines[i])).append('\n');
            }
            return out.toString().trim();
        } catch (Exception exception) {
            return "";
        }
    }

    // The keys a renderer report turns on first; the whole file rides in the bundle.
    private static String settingsSummary(String settings) {
        if (settings.isEmpty()) return "- no settings file";
        StringBuilder out = new StringBuilder();
        for (String line : settings.split("\\R")) {
            String key = line.trim();
            if (key.startsWith("resolution_scale") || key.startsWith("draw_resolution") ||
                key.contains("scene_width") || key.contains("scene_height") ||
                key.startsWith("skate3_native_render_scene =") ||
                key.contains("fps_cap") || key.contains("scene_msaa") || key.contains("hdr") ||
                key.contains("shadow") || key.contains("ssao") || key.contains("bloom") ||
                key.contains("distance_scale") || key.startsWith("skate3_demo_path") ||
                key.contains("vegetation") || key.contains("ambient_npcs") ||
                key.contains("movable_props")) {
                out.append(key).append('\n');
            }
        }
        return out.length() == 0 ? "- defaults" : out.toString().trim();
    }

    private static String inputMethod() {
        boolean controller = false;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device == null || id < 0) continue;
            int sources = device.getSources();
            controller |= (sources & (InputDevice.SOURCE_GAMEPAD |
                                      InputDevice.SOURCE_JOYSTICK |
                                      InputDevice.SOURCE_DPAD)) != 0;
        }
        if (!controller) return "Touch controls";
        return "Multiple input methods";
    }

    private static String vulkanVersion(Context context) {
        for (FeatureInfo feature : context.getPackageManager().getSystemAvailableFeatures()) {
            if (!"android.hardware.vulkan.version".equals(feature.name)) continue;
            int version = feature.version;
            return ((version >> 22) & 0x3ff) + "." +
                   ((version >> 12) & 0x3ff) + "." + (version & 0xfff);
        }
        return "not reported";
    }

    private static String recentExits(Context context) {
        try {
            ActivityManager manager = context.getSystemService(ActivityManager.class);
            List<ApplicationExitInfo> exits = manager.getHistoricalProcessExitReasons(
                context.getPackageName(), 0, 3);
            if (exits.isEmpty()) return "- none recorded";
            // Local time, like the log's own timestamps, so the two line up.
            SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US);
            StringBuilder text = new StringBuilder();
            boolean traceIncluded = false;
            for (ApplicationExitInfo exit : exits) {
                text.append("- ").append(format.format(new Date(exit.getTimestamp())))
                    .append(": ").append(reasonName(exit.getReason()))
                    .append(", status=").append(exit.getStatus())
                    .append(", pss=").append(exit.getPss()).append(" KiB")
                    .append(", rss=").append(exit.getRss()).append(" KiB");
                text.append('\n');
                if (!traceIncluded &&
                    exit.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE) {
                    String trace = nativeTrace(exit);
                    if (!trace.isEmpty()) {
                        text.append("  Native tombstone:\n");
                        for (String line : trace.split("\\R")) {
                            text.append("  ").append(line).append('\n');
                        }
                        traceIncluded = true;
                    }
                }
            }
            return text.toString().trim();
        } catch (Exception exception) {
            return "- unavailable: " + clean(exception.getClass().getSimpleName());
        }
    }

    private static String nativeTrace(ApplicationExitInfo exit) {
        try (InputStream raw = exit.getTraceInputStream()) {
            if (raw == null) return "";
            byte[] encoded = readLimited(raw, 4 * 1024 * 1024);
            if (encoded.length >= 2 && (encoded[0] & 0xff) == 0x1f &&
                (encoded[1] & 0xff) == 0x8b) {
                try (GZIPInputStream gzip = new GZIPInputStream(
                         new ByteArrayInputStream(encoded))) {
                    encoded = readLimited(gzip, 4 * 1024 * 1024);
                }
            }
            return parseTombstone(encoded);
        } catch (Exception exception) {
            return "trace unavailable: " + clean(exception.getClass().getSimpleName());
        }
    }

    // Native exit traces are debuggerd Tombstone protobufs; this reads only the
    // crash fields so the APK needs no protobuf runtime.
    private static String parseTombstone(byte[] data) {
        ProtoReader root = new ProtoReader(data);
        long crashingTid = -1;
        String signal = "";
        String abort = "";
        List<String> causes = new ArrayList<>();
        List<ThreadTrace> threads = new ArrayList<>();
        while (root.hasRemaining()) {
            int tag = root.readTag();
            if (tag == 0) break;
            int field = tag >>> 3;
            int wire = tag & 7;
            if (field == 6 && wire == 0) {
                crashingTid = root.readVarint();
            } else if (field == 10 && wire == 2) {
                signal = parseSignal(root.readBytes());
            } else if (field == 14 && wire == 2) {
                abort = clean(root.readString());
            } else if (field == 15 && wire == 2) {
                String cause = parseCause(root.readBytes());
                if (!cause.isEmpty()) causes.add(cause);
            } else if (field == 16 && wire == 2) {
                ThreadTrace thread = parseThreadEntry(root.readBytes());
                if (thread != null) threads.add(thread);
            } else {
                root.skip(wire);
            }
        }
        StringBuilder out = new StringBuilder();
        if (!signal.isEmpty()) out.append("Signal: ").append(signal).append('\n');
        if (!abort.isEmpty()) out.append("Abort: ").append(abort).append('\n');
        for (String cause : causes) out.append("Cause: ").append(cause).append('\n');
        ThreadTrace crashing = null;
        for (ThreadTrace thread : threads) {
            if (thread.tid == crashingTid) {
                crashing = thread;
                break;
            }
        }
        if (crashing == null && !threads.isEmpty()) crashing = threads.get(0);
        if (crashing != null) {
            out.append("Thread: ").append(crashing.tid);
            if (!crashing.name.isEmpty()) out.append(" (").append(crashing.name).append(')');
            out.append('\n');
            for (String frame : crashing.frames) out.append(frame).append('\n');
            for (String note : crashing.notes) out.append("Note: ").append(note).append('\n');
        }
        return out.toString().trim();
    }

    private static String parseSignal(byte[] data) {
        ProtoReader reader = new ProtoReader(data);
        long number = -1;
        String name = "";
        String code = "";
        long address = -1;
        while (reader.hasRemaining()) {
            int tag = reader.readTag();
            if (tag == 0) break;
            int field = tag >>> 3;
            int wire = tag & 7;
            if (field == 1 && wire == 0) number = reader.readVarint();
            else if (field == 2 && wire == 2) name = clean(reader.readString());
            else if (field == 4 && wire == 2) code = clean(reader.readString());
            else if (field == 9 && wire == 0) address = reader.readVarint();
            else reader.skip(wire);
        }
        StringBuilder out = new StringBuilder();
        if (!name.isEmpty()) out.append(name);
        else if (number >= 0) out.append(number);
        if (!code.isEmpty()) out.append(" / ").append(code);
        if (address >= 0) out.append(" at 0x").append(Long.toHexString(address));
        return out.toString();
    }

    private static String parseCause(byte[] data) {
        ProtoReader reader = new ProtoReader(data);
        while (reader.hasRemaining()) {
            int tag = reader.readTag();
            if (tag == 0) break;
            int field = tag >>> 3;
            int wire = tag & 7;
            if (field == 1 && wire == 2) return clean(reader.readString());
            reader.skip(wire);
        }
        return "";
    }

    private static ThreadTrace parseThreadEntry(byte[] data) {
        ProtoReader entry = new ProtoReader(data);
        long key = -1;
        byte[] value = null;
        while (entry.hasRemaining()) {
            int tag = entry.readTag();
            if (tag == 0) break;
            int field = tag >>> 3;
            int wire = tag & 7;
            if (field == 1 && wire == 0) key = entry.readVarint();
            else if (field == 2 && wire == 2) value = entry.readBytes();
            else entry.skip(wire);
        }
        if (value == null) return null;
        ThreadTrace thread = parseThread(value);
        if (thread.tid < 0) thread.tid = key;
        return thread;
    }

    private static ThreadTrace parseThread(byte[] data) {
        ThreadTrace thread = new ThreadTrace();
        ProtoReader reader = new ProtoReader(data);
        while (reader.hasRemaining()) {
            int tag = reader.readTag();
            if (tag == 0) break;
            int field = tag >>> 3;
            int wire = tag & 7;
            if (field == 1 && wire == 0) thread.tid = reader.readVarint();
            else if (field == 2 && wire == 2) thread.name = clean(reader.readString());
            else if (field == 4 && wire == 2 && thread.frames.size() < 16) {
                String frame = parseFrame(reader.readBytes(), thread.frames.size());
                if (!frame.isEmpty()) thread.frames.add(frame);
            } else if (field == 7 && wire == 2 && thread.notes.size() < 4) {
                thread.notes.add(clean(reader.readString()));
            } else reader.skip(wire);
        }
        return thread;
    }

    private static String parseFrame(byte[] data, int index) {
        ProtoReader reader = new ProtoReader(data);
        long relativePc = -1;
        long functionOffset = -1;
        String function = "";
        String file = "";
        while (reader.hasRemaining()) {
            int tag = reader.readTag();
            if (tag == 0) break;
            int field = tag >>> 3;
            int wire = tag & 7;
            if (field == 1 && wire == 0) relativePc = reader.readVarint();
            else if (field == 4 && wire == 2) function = clean(reader.readString());
            else if (field == 5 && wire == 0) functionOffset = reader.readVarint();
            else if (field == 6 && wire == 2) file = new File(reader.readString()).getName();
            else reader.skip(wire);
        }
        if (file.isEmpty() && function.isEmpty() && relativePc < 0) return "";
        StringBuilder out = new StringBuilder(String.format(Locale.US, "#%02d ", index));
        out.append(file.isEmpty() ? "<unknown>" : file);
        if (relativePc >= 0) out.append("+0x").append(Long.toHexString(relativePc));
        if (!function.isEmpty()) out.append(" ").append(function);
        if (functionOffset > 0) out.append("+0x").append(Long.toHexString(functionOffset));
        return out.toString();
    }

    private static byte[] readLimited(InputStream input, int limit) throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[16 * 1024];
        while (output.size() < limit) {
            int read = input.read(buffer, 0, Math.min(buffer.length, limit - output.size()));
            if (read < 0) break;
            output.write(buffer, 0, read);
        }
        return output.toByteArray();
    }

    private static File newestLog(Context context) {
        File[] logs = new File(context.getFilesDir(), "logs").listFiles((dir, name) ->
            name.startsWith("skate3_") && name.endsWith(".log"));
        if (logs == null || logs.length == 0) return null;
        File latest = logs[0];
        for (File log : logs) if (log.lastModified() > latest.lastModified()) latest = log;
        return latest;
    }

    // Warnings and errors from the whole window, then the last lines: the browser
    // budget in open() drops from the front, so the newest lines survive longest.
    private static List<String> runtimeEvidence(Context context, File log) {
        List<String> out = new ArrayList<>();
        if (log == null) {
            out.add("- no runtime log found");
            return out;
        }
        try {
            String[] lines = new String(readLogWindow(log), StandardCharsets.UTF_8).split("\\R");
            List<String> important = new ArrayList<>();
            for (String line : lines) {
                String lower = line.toLowerCase(Locale.US);
                if (lower.contains("[warning]") || lower.contains("[error]") ||
                    lower.contains("[critical]") || lower.contains("fatal") ||
                    lower.contains("abort")) {
                    important.add(sanitizePath(context, line));
                }
            }
            if (important.size() > MAX_IMPORTANT_LINES) {
                important = important.subList(important.size() - MAX_IMPORTANT_LINES,
                                              important.size());
            }
            out.addAll(important);
            out.add("[latest lines]");
            for (int i = Math.max(0, lines.length - LATEST_LINES); i < lines.length; i++) {
                if (!lines[i].isEmpty()) out.add(sanitizePath(context, lines[i]));
            }
        } catch (Exception exception) {
            out.add("- runtime log unavailable: " + clean(exception.getClass().getSimpleName()));
        }
        return out;
    }

    /** One file in Downloads, where any picker can reach it: report, settings, whole log. */
    private static void saveBundle(Context context, String name, String report,
                                   String settings, File log) {
        ContentValues values = new ContentValues();
        values.put(MediaStore.Downloads.DISPLAY_NAME, name);
        values.put(MediaStore.Downloads.MIME_TYPE, "text/plain");
        values.put(MediaStore.Downloads.IS_PENDING, 1);
        ContentResolver resolver = context.getContentResolver();
        Uri uri = null;
        try {
            uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
            if (uri == null) return;
            try (OutputStream out = resolver.openOutputStream(uri)) {
                if (out == null) throw new IOException("no output stream");
                out.write((report + "\n\n=== settings.toml ===\n" + settings + "\n\n=== " +
                           (log == null ? "no log" : log.getName()) + " ===\n")
                          .getBytes(StandardCharsets.UTF_8));
                if (log != null) {
                    try (InputStream in = new FileInputStream(log)) {
                        byte[] buffer = new byte[64 * 1024];
                        for (int read; (read = in.read(buffer)) > 0; ) out.write(buffer, 0, read);
                    }
                }
            }
            values.clear();
            values.put(MediaStore.Downloads.IS_PENDING, 0);
            resolver.update(uri, values, null, null);
        } catch (Exception exception) {
            // A half-written pending row would sit hidden in Downloads for a week.
            if (uri != null) resolver.delete(uri, null, null);
        }
    }

    private static byte[] readLogWindow(File file) throws Exception {
        final int headLimit = 128 * 1024;
        final int tailLimit = 512 * 1024;
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] head = new byte[(int)Math.min(file.length(), headLimit)];
            int read = input.read(head);
            if (read > 0) output.write(head, 0, read);
        }
        if (file.length() > headLimit) {
            // Start the tail after the head, or a mid-sized log is read twice.
            long start = Math.max(headLimit, file.length() - tailLimit);
            if (start > headLimit) {
                output.write("\n[tail of runtime log]\n".getBytes(StandardCharsets.UTF_8));
            }
            try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
                input.seek(start);
                byte[] tail = new byte[(int)(file.length() - start)];
                input.readFully(tail);
                output.write(tail);
            }
        }
        return output.toByteArray();
    }

    private static String sanitizePath(Context context, String value) {
        String safe = value.replace(context.getFilesDir().getAbsolutePath(), "<app-files>");
        File external = context.getExternalFilesDir(null);
        if (external != null) {
            safe = safe.replace(external.getAbsolutePath(), "<app-external-files>");
        }
        return clean(safe);
    }

    /**
     * How the last session ended, when that is worth telling the player: Android
     * reclaiming the process, or a crash. Returns null for an ordinary exit, which
     * is what almost every launch follows.
     */
    static String lastSessionEnding(Context context) {
        try {
            List<ApplicationExitInfo> exits = context.getSystemService(ActivityManager.class)
                .getHistoricalProcessExitReasons(context.getPackageName(), 0, 1);
            if (exits.isEmpty()) return null;
            switch (exits.get(0).getReason()) {
                case ApplicationExitInfo.REASON_LOW_MEMORY:
                    return "Android closed the last session to free memory.";
                case ApplicationExitInfo.REASON_CRASH:
                case ApplicationExitInfo.REASON_CRASH_NATIVE:
                    return "The last session ended in a crash.";
                case ApplicationExitInfo.REASON_ANR:
                    return "The last session stopped responding and was closed.";
                case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE:
                    return "Android closed the last session for using too much memory or power.";
                default:
                    return null;
            }
        } catch (RuntimeException ignored) {
            return null;
        }
    }

    private static String reasonName(int reason) {
        switch (reason) {
            case ApplicationExitInfo.REASON_EXIT_SELF: return "normal self-exit";
            case ApplicationExitInfo.REASON_SIGNALED: return "signal";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "low memory";
            case ApplicationExitInfo.REASON_CRASH: return "Java crash";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "native crash";
            case ApplicationExitInfo.REASON_ANR: return "ANR";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "initialization failure";
            case ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "permission change";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "excessive resources";
            case ApplicationExitInfo.REASON_USER_REQUESTED: return "user requested";
            case ApplicationExitInfo.REASON_USER_STOPPED: return "user stopped";
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "dependency died";
            case ApplicationExitInfo.REASON_OTHER: return "other";
            case ApplicationExitInfo.REASON_FREEZER: return "app freezer";
            case ApplicationExitInfo.REASON_PACKAGE_STATE_CHANGE: return "package state change";
            case ApplicationExitInfo.REASON_PACKAGE_UPDATED: return "package updated";
            default: return "unknown (" + reason + ")";
        }
    }

    private static void copy(Context context, String report) {
        ClipboardManager clipboard = context.getSystemService(ClipboardManager.class);
        clipboard.setPrimaryClip(ClipData.newPlainText("Skate 3 diagnostics", fenced(report)));
        Toast.makeText(context, "Device diagnostics copied.", Toast.LENGTH_SHORT).show();
    }

    private static void open(Activity activity, Diagnostic diagnostic) {
        List<String> lines = diagnostic.evidence;
        Uri uri;
        while (true) {
            String evidence = diagnostic.header + String.join("\n", lines);
            if (lines.size() < diagnostic.evidence.size()) evidence += SHORTENED_NOTE;
            uri = issueUri(diagnostic, evidence);
            if (uri.toString().length() <= MAX_URL_LENGTH || lines.isEmpty()) break;
            lines = lines.subList(1, lines.size());
        }
        if (uri.toString().length() > MAX_URL_LENGTH) {
            // A long native backtrace can overflow the header alone; the clipboard keeps it.
            int cut = diagnostic.header.indexOf("Recent process exits:");
            String evidence = (cut > 0 ? diagnostic.header.substring(0, cut) : "") + SHORTENED_NOTE;
            uri = issueUri(diagnostic, evidence);
        }
        try {
            activity.startActivity(new Intent(Intent.ACTION_VIEW, uri));
        } catch (ActivityNotFoundException exception) {
            Toast.makeText(activity, "No browser is available. The diagnostics are copied.",
                           Toast.LENGTH_LONG).show();
        }
    }

    private static Uri issueUri(Diagnostic diagnostic, String evidence) {
        return Uri.parse(ISSUE_URL).buildUpon()
            .appendQueryParameter("template", "bug_report.yml")
            .appendQueryParameter("title", "[BUG] " + diagnostic.device)
            .appendQueryParameter("version", diagnostic.version)
            .appendQueryParameter("device", diagnostic.device)
            .appendQueryParameter("android", diagnostic.android)
            .appendQueryParameter("chipset", diagnostic.soc)
            .appendQueryParameter("driver", diagnostic.driver)
            .appendQueryParameter("input", diagnostic.input)
            .appendQueryParameter("evidence", fenced(evidence))
            .build();
    }

    // Rendered as markdown, "#01" frames autolink to issues 1-10 and "<app-files>"
    // vanishes as an HTML tag. A code fence shows the text as written.
    private static String fenced(String text) {
        return "```text\n" + text + "\n```";
    }

    /**
     * Cluster layout as "4x1.80 + 3x2.42 + 1x2.84 GHz". Thread placement and every
     * performance reading depend on which cores a device actually has.
     */
    private static String cpuLayout() {
        List<Integer> speeds = new ArrayList<>();
        for (int cpu = 0; cpu < 32; cpu++) {
            String khz = readSmallFile(new File(
                "/sys/devices/system/cpu/cpu" + cpu + "/cpufreq/cpuinfo_max_freq"));
            if (khz == null || khz.trim().isEmpty()) break;
            try {
                speeds.add(Integer.parseInt(khz.trim()));
            } catch (NumberFormatException ignored) {
                break;
            }
        }
        if (speeds.isEmpty()) return "unknown";
        StringBuilder out = new StringBuilder();
        for (int speed : new TreeSet<>(speeds)) {
            if (out.length() > 0) out.append(" + ");
            out.append(Collections.frequency(speeds, speed)).append('x')
                .append(String.format(Locale.US, "%.2f", speed / 1000000.0));
        }
        return out.append(" GHz").toString();
    }

    /**
     * Filesystem the game folder sits on. A FUSE-mounted Android/data behaves
     * differently enough during extraction to be worth knowing from a report.
     */
    private static String mountType(File path) {
        String mounts = readSmallFile(new File("/proc/mounts"));
        if (mounts == null || path == null) return "unknown";
        String longest = "";
        String type = "unknown";
        for (String line : mounts.split("\n")) {
            String[] parts = line.split(" ");
            if (parts.length < 3) continue;
            if (path.getAbsolutePath().startsWith(parts[1]) && parts[1].length() > longest.length()) {
                longest = parts[1];
                type = parts[2];
            }
        }
        return type;
    }

    private static String clean(String value) {
        if (value == null) return "";
        return value.replace('\n', ' ').replace('\r', ' ').trim();
    }

    private static final class Diagnostic {
        final String version;
        final String device;
        final String android;
        final String soc;
        final String driver;
        final String input;
        final String header;
        final List<String> evidence;
        /** Everything, unbudgeted: what the clipboard gets. */
        final String report;

        Diagnostic(String version, String device, String android, String soc,
                   String driver, String input, String header, List<String> evidence) {
            this.version = version;
            this.device = device;
            this.android = android;
            this.soc = soc;
            this.driver = driver;
            this.input = input;
            this.header = header;
            this.evidence = evidence;
            this.report = header + String.join("\n", evidence);
        }
    }

    private static final class ThreadTrace {
        long tid = -1;
        String name = "";
        final List<String> frames = new ArrayList<>();
        final List<String> notes = new ArrayList<>();
    }

    private static final class ProtoReader {
        final byte[] data;
        int position;

        ProtoReader(byte[] data) {
            this.data = data != null ? data : new byte[0];
        }

        boolean hasRemaining() { return position < data.length; }

        int readTag() {
            long value = readVarint();
            return value > 0 && value <= Integer.MAX_VALUE ? (int)value : 0;
        }

        long readVarint() {
            long value = 0;
            for (int shift = 0; shift < 64 && position < data.length; shift += 7) {
                int next = data[position++] & 0xff;
                value |= (long)(next & 0x7f) << shift;
                if ((next & 0x80) == 0) return value;
            }
            throw new IllegalArgumentException("invalid protobuf varint");
        }

        byte[] readBytes() {
            long encodedLength = readVarint();
            if (encodedLength < 0 || encodedLength > Integer.MAX_VALUE ||
                position + encodedLength > data.length) {
                throw new IllegalArgumentException("invalid protobuf length");
            }
            int length = (int)encodedLength;
            byte[] result = new byte[length];
            System.arraycopy(data, position, result, 0, length);
            position += length;
            return result;
        }

        String readString() {
            return new String(readBytes(), StandardCharsets.UTF_8);
        }

        void skip(int wire) {
            if (wire == 0) {
                readVarint();
            } else if (wire == 1) {
                advance(8);
            } else if (wire == 2) {
                long length = readVarint();
                if (length > Integer.MAX_VALUE) throw new IllegalArgumentException("field too big");
                advance((int)length);
            } else if (wire == 5) {
                advance(4);
            } else {
                throw new IllegalArgumentException("unsupported protobuf wire type");
            }
        }

        void advance(int count) {
            if (count < 0 || position + count > data.length) {
                throw new IllegalArgumentException("truncated protobuf");
            }
            position += count;
        }
    }
}

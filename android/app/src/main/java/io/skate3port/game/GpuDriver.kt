@file:JvmName("GpuDriver")
package io.skate3port.game

import android.content.Context
import android.os.Build
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.IOException
import java.io.InputStream
import java.nio.charset.StandardCharsets
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.util.Locale
import java.util.stream.Collectors
import java.util.zip.ZipInputStream

/**
 * Imports a driver package into app-private storage and remembers whether the
 * game loads it. The ZIP is user-picked, so nothing in it is trusted.
 */

private const val PREFERENCES = "gpu_driver"
private const val ENABLED_PREFERENCE = "custom_gpu_driver_enabled"
private const val ACTIVE_DIRECTORY = "active"
private const val INSTALL_METADATA = ".skate3-driver.json"
private const val MAX_FILES = 96
private const val MAX_TOTAL_SIZE = 384L * 1024 * 1024
private const val MAX_METADATA_SIZE = 128L * 1024

/** An imported driver. [isEnabled] is false while the system driver is selected. */
class GpuDriverInfo(
    val name: String,
    val vendor: String,
    val version: String,
    val author: String,
    val libraryName: String,
    val directory: Path,
    val isEnabled: Boolean,
) {
    fun label(): String = "$name $version"
}

fun isLikelyAdrenoDevice(): Boolean {
    val identity = (
        Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL + " " + Build.HARDWARE + " " + Build.BOARD
        ).lowercase(Locale.US)
    return listOf("qualcomm", "qti", "qcom", "snapdragon", "adreno", "sm8")
        .any { identity.contains(it) }
}

/** The imported driver, or null when none is installed or it no longer verifies. */
fun installedDriver(context: Context): GpuDriverInfo? {
    val active = driverRoot(context).resolve(ACTIVE_DIRECTORY)
    val marker = active.resolve(INSTALL_METADATA)
    return try {
        if (!Files.isRegularFile(marker) || Files.size(marker) > MAX_METADATA_SIZE) return null
        val metadata = JSONObject(String(Files.readAllBytes(marker), StandardCharsets.UTF_8))
        val libraryName = metadata.getString("libraryName")
        if (!safeFileName(libraryName)) return null
        val directory = active.resolve(metadata.getString("directory")).normalize()
        if (!directory.startsWith(active) ||
            !Files.isRegularFile(directory.resolve(libraryName))
        ) {
            return null
        }
        GpuDriverInfo(
            metadata.getString("name"),
            metadata.getString("vendor"),
            metadata.getString("driverVersion"),
            metadata.getString("author"),
            libraryName,
            directory,
            enabledPreference(context) && isLikelyAdrenoDevice(),
        )
    } catch (ignored: Exception) {
        null
    }
}

fun selectedDriverLabel(context: Context): String {
    val driver = installedDriver(context)
    return if (driver != null && driver.isEnabled) driver.label() else "System"
}

@Throws(IOException::class)
fun importDriverPackage(context: Context, rawInput: InputStream): GpuDriverInfo {
    if (!isLikelyAdrenoDevice()) {
        throw IOException("Custom drivers only load on Snapdragon / Adreno devices.")
    }
    val root = driverRoot(context)
    Files.createDirectories(root)
    val staging = root.resolve("importing-" + System.nanoTime())
    Files.createDirectory(staging)
    try {
        extractPackage(rawInput, staging)
        val packageMetadata = findPackageMetadata(staging)
        val metadata = readPackageMetadata(packageMetadata)
        validateMetadata(metadata)

        val packageDirectory = packageMetadata.parent
        val libraryName = metadata.getString("libraryName")
        val mainLibrary = packageDirectory.resolve(libraryName).normalize()
        if (mainLibrary.parent != packageDirectory || !Files.isRegularFile(mainLibrary)) {
            throw IOException("The driver ZIP does not contain $libraryName beside meta.json.")
        }
        validateLibraries(staging)

        val installMetadata = JSONObject()
        installMetadata.put("name", bounded(metadata.getString("name"), 80, "name"))
        installMetadata.put("vendor", bounded(metadata.getString("vendor"), 32, "vendor"))
        installMetadata.put(
            "driverVersion", bounded(metadata.getString("driverVersion"), 64, "driverVersion")
        )
        installMetadata.put("author", bounded(metadata.getString("author"), 80, "author"))
        installMetadata.put("libraryName", libraryName)
        val relativeDirectory = staging.relativize(packageDirectory).toString()
        installMetadata.put("directory", relativeDirectory.ifEmpty { "." })
        Files.write(
            staging.resolve(INSTALL_METADATA),
            installMetadata.toString(2).toByteArray(StandardCharsets.UTF_8),
            StandardOpenOption.CREATE_NEW,
        )

        val active = root.resolve(ACTIVE_DIRECTORY)
        val backup = root.resolve("previous")
        deleteTree(backup)
        if (Files.exists(active)) move(active, backup)
        try {
            move(staging, active)
        } catch (exception: IOException) {
            // A failed swap must not leave the previous driver half-removed.
            if (Files.exists(backup) && !Files.exists(active)) move(backup, active)
            throw exception
        }
        // Past this point the new driver is live; a stale backup is not a failure.
        try {
            deleteTree(backup)
        } catch (ignored: IOException) {
        }
        setEnabledPreference(context, true)
        return installedDriver(context)
            ?: throw IOException("The imported driver could not be activated.")
    } catch (exception: Exception) {
        deleteTree(staging)
        throw exception as? IOException
            ?: IOException("The driver package is invalid: " + cleanMessage(exception), exception)
    }
}

fun useSystemDriver(context: Context) = setEnabledPreference(context, false)

@Throws(IOException::class)
fun useCustomDriver(context: Context) {
    if (!isLikelyAdrenoDevice()) {
        throw IOException("A custom driver is not available on this non-Adreno device.")
    }
    if (installedDriver(context) == null) throw IOException("Import a driver ZIP first.")
    setEnabledPreference(context, true)
}

@Throws(IOException::class)
fun removeDriver(context: Context) {
    useSystemDriver(context)
    deleteTree(driverRoot(context).resolve(ACTIVE_DIRECTORY))
}

private fun driverRoot(context: Context): Path =
    context.filesDir.toPath().resolve("gpu-drivers")

private fun enabledPreference(context: Context): Boolean =
    context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
        .getBoolean(ENABLED_PREFERENCE, false)

private fun setEnabledPreference(context: Context, enabled: Boolean) {
    context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
        .putBoolean(ENABLED_PREFERENCE, enabled).apply()
}

@Throws(IOException::class)
private fun extractPackage(rawInput: InputStream, staging: Path) {
    var total = 0L
    var files = 0
    val buffer = ByteArray(256 * 1024)
    ZipInputStream(BufferedInputStream(rawInput)).use { zip ->
        while (true) {
            val entry = zip.nextEntry ?: break
            val name = entry.name
            if (name.isNullOrBlank() || name.indexOf('\u0000') >= 0 || name.contains("\\") ||
                name.startsWith("/") || name.length > 240
            ) {
                throw IOException("The driver ZIP contains an unsafe path.")
            }
            val target = staging.resolve(name).normalize()
            if (!target.startsWith(staging)) {
                throw IOException("The driver ZIP contains a path outside the package.")
            }
            if (++files > MAX_FILES) throw IOException("The driver ZIP contains too many files.")
            if (entry.isDirectory) {
                Files.createDirectories(target)
                zip.closeEntry()
                continue
            }
            if (Files.exists(target)) throw IOException("The driver ZIP contains duplicate files.")
            Files.createDirectories(target.parent)
            var fileSize = 0L
            Files.newOutputStream(target, StandardOpenOption.CREATE_NEW).use { output ->
                while (true) {
                    val read = zip.read(buffer)
                    if (read < 0) break
                    fileSize += read
                    total += read
                    // The declared entry size is attacker-controlled; count what arrives.
                    if (total > MAX_TOTAL_SIZE) {
                        throw IOException("The driver ZIP is unexpectedly large.")
                    }
                    output.write(buffer, 0, read)
                }
            }
            if (fileSize == 0L) throw IOException("The driver ZIP contains an empty file.")
            zip.closeEntry()
        }
    }
    if (files == 0) throw IOException("The selected file is not a driver ZIP.")
}

@Throws(IOException::class)
private fun findPackageMetadata(staging: Path): Path {
    val matches = walk(staging).filter { it.fileName?.toString() == "meta.json" }
    if (matches.size != 1) {
        throw IOException("The driver ZIP must contain exactly one meta.json.")
    }
    return matches[0]
}

@Throws(IOException::class)
private fun readPackageMetadata(path: Path): JSONObject {
    if (Files.size(path) > MAX_METADATA_SIZE) {
        throw IOException("The driver metadata is unexpectedly large.")
    }
    return try {
        JSONObject(String(Files.readAllBytes(path), StandardCharsets.UTF_8))
    } catch (exception: Exception) {
        throw IOException("meta.json is not valid JSON.", exception)
    }
}

@Throws(IOException::class)
private fun validateMetadata(metadata: JSONObject) {
    try {
        if (metadata.getInt("schemaVersion") != 1) {
            throw IOException("This driver package schema is not supported.")
        }
        val library = metadata.getString("libraryName")
        if (!safeFileName(library) || !library.endsWith(".so")) {
            throw IOException("meta.json has an invalid libraryName.")
        }
        val minimumApi = metadata.getInt("minApi")
        if (minimumApi < 21 || minimumApi > Build.VERSION.SDK_INT) {
            throw IOException(
                "This driver requires Android API $minimumApi, but this device is API " +
                    Build.VERSION.SDK_INT + "."
            )
        }
    } catch (exception: IOException) {
        throw exception
    } catch (exception: Exception) {
        throw IOException("meta.json is missing required driver package fields.", exception)
    }
}

@Throws(IOException::class)
private fun validateLibraries(staging: Path) {
    var libraries = 0
    for (path in walk(staging)) {
        if (!Files.isRegularFile(path) || !path.fileName.toString().endsWith(".so")) continue
        ++libraries
        validateArm64Elf(path)
        path.toFile().setReadable(true, true)
        path.toFile().setExecutable(true, true)
    }
    if (libraries == 0) throw IOException("The driver ZIP contains no shared libraries.")
}

// The loader dlopens this file, so a wrong-architecture blob kills the process
// with nothing to report. Magic, 64-bit little-endian, e_type 3, e_machine 183.
@Throws(IOException::class)
private fun validateArm64Elf(path: Path) {
    val h = Files.newInputStream(path).use { it.readNBytes(20) }
    val valid = h.size == 20 && h[0] == 0x7f.toByte() && h[1] == 'E'.code.toByte() &&
        h[2] == 'L'.code.toByte() && h[3] == 'F'.code.toByte() &&
        h[4].toInt() == 2 && h[5].toInt() == 1 &&
        h[16].toInt() == 3 && h[17].toInt() == 0 &&
        (h[18].toInt() and 0xff) == 183 && h[19].toInt() == 0
    if (!valid) throw IOException("${path.fileName} is not an ARM64 Android library.")
}

@Throws(IOException::class)
private fun bounded(value: String, maximum: Int, field: String): String {
    if (value.isBlank() || value.length > maximum ||
        value.indexOf('\n') >= 0 || value.indexOf('\r') >= 0
    ) {
        throw IOException("meta.json has an invalid $field.")
    }
    return value.trim()
}

private fun safeFileName(value: String?): Boolean =
    value != null && value.length in 4..120 && value != "." && value != ".." &&
        value.indexOf('/') < 0 && value.indexOf('\\') < 0 && value.indexOf('\u0000') < 0

@Throws(IOException::class)
private fun walk(root: Path): List<Path> =
    Files.walk(root).use { paths -> paths.collect(Collectors.toList()) }

@Throws(IOException::class)
private fun move(source: Path, destination: Path) {
    try {
        Files.move(source, destination, StandardCopyOption.ATOMIC_MOVE)
    } catch (exception: AtomicMoveNotSupportedException) {
        Files.move(source, destination)
    }
}

@Throws(IOException::class)
private fun deleteTree(target: Path) = deleteRecursively(target.toFile())

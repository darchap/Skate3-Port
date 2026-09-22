@file:JvmName("GameFiles")
package io.skate3port.game

import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.nio.file.Path
import java.security.MessageDigest
import java.util.Locale

/** Written into the staging directory once every ISO entry has been extracted. */
const val COMPLETE_MARKER: String = ".iso-extraction-complete"

private const val EXPECTED_DEFAULT_XEX =
    "1db39496585c521d17a2137804f42cf73ebed2b32cac166ec42dbf772f4dcf7f"
private const val EXPECTED_WEBKIT_XEX =
    "0ee66b9558c888147f4ffdfd748cfc28950e80e46a97321da2de2afdb068523f"

/** Pre-scoped-storage install location, still honoured so old installs keep working. */
private val LEGACY_ROOT = File("/storage/emulated/0/skate3")

fun isGameReady(root: Path): Boolean =
    Files.isRegularFile(root.resolve("default.xex")) &&
        Files.isRegularFile(root.resolve("data/webkit/EAWebkit.xex")) &&
        TitleUpdateInstaller.isInstalled(root)

fun legacyGameReady(): Boolean = isGameReady(LEGACY_ROOT.toPath())

fun isExtractionComplete(partialDirectory: File): Boolean =
    File(partialDirectory, COMPLETE_MARKER).isFile

@Throws(IOException::class)
fun verifyRetailGame(root: Path) {
    verifyFile(root.resolve("default.xex"), EXPECTED_DEFAULT_XEX, "default.xex")
    verifyFile(root.resolve("data/webkit/EAWebkit.xex"), EXPECTED_WEBKIT_XEX,
        "data/webkit/EAWebkit.xex")
}

@Throws(IOException::class)
private fun verifyFile(path: Path, expectedHash: String, label: String) {
    if (!Files.isRegularFile(path)) {
        throw IOException("The ISO did not provide $label.")
    }
    val digest = MessageDigest.getInstance("SHA-256")
    Files.newInputStream(path).use { input ->
        val buffer = ByteArray(1024 * 1024)
        while (true) {
            val read = input.read(buffer)
            if (read < 0) break
            digest.update(buffer, 0, read)
        }
    }
    if (hex(digest.digest()) != expectedHash) {
        throw IOException("$label is not the supported USA/Europe retail version.")
    }
}

// Deletes depth-first: a directory only goes once its children are gone.
@Throws(IOException::class)
fun deleteRecursively(target: File) {
    if (!target.exists()) return
    target.listFiles()?.forEach { deleteRecursively(it) }
    if (!target.delete() && target.exists()) {
        throw IOException("Could not remove ${target.absolutePath}")
    }
}

/** Human-readable size, matching the launcher's previous formatting exactly. */
fun humanBytes(bytes: Long): String {
    if (bytes < 1024) return "$bytes B"
    val units = arrayOf("B", "KB", "MB", "GB", "TB")
    var value = bytes.toDouble()
    var unit = 0
    while (value >= 1024 && unit < units.size - 1) {
        value /= 1024
        unit++
    }
    val format = if (value >= 10) "%.1f %s" else "%.2f %s"
    return String.format(Locale.US, format, value, units[unit])
}

private fun hex(bytes: ByteArray): String {
    val digits = "0123456789abcdef"
    val result = CharArray(bytes.size * 2)
    for (i in bytes.indices) {
        val value = bytes[i].toInt() and 0xFF
        result[i * 2] = digits[value ushr 4]
        result[i * 2 + 1] = digits[value and 15]
    }
    return String(result)
}

/**
 * Decide what the launcher should show, from the filesystem alone.
 *
 * [unsupportedReason], [freeBytes] and [gpuDriver] are passed in because they
 * need Android APIs; everything else this reads is a plain file check.
 */
fun currentState(
    gameDirectory: File,
    partialDirectory: File,
    freeBytes: Long,
    setupLogExists: Boolean,
    unsupportedReason: String?,
    gpuDriver: String,
    updateChannel: String,
): InstallState {
    if (unsupportedReason != null) return InstallState.Unsupported(unsupportedReason)

    if (isGameReady(gameDirectory.toPath())) {
        return InstallState.Ready(gameDirectory, canRepair = true, gpuDriver = gpuDriver,
            updateChannel = updateChannel)
    }
    if (legacyGameReady()) {
        return InstallState.Ready(LEGACY_ROOT, canRepair = false, gpuDriver = gpuDriver,
            updateChannel = updateChannel)
    }
    if (isExtractionComplete(partialDirectory)) return InstallState.AwaitingTitleUpdate

    return InstallState.NeedsIso(freeBytes, setupLogExists)
}

/**
 * What the launcher screen is showing. Derived from the filesystem by
 * [currentState]; nothing here knows about views.
 */
sealed interface InstallState {

    /** The device cannot run this build at all. [reason] is shown to the user. */
    data class Unsupported(val reason: String) : InstallState

    /** A playable install exists at [gameRoot]. Legacy installs cannot be repaired. */
    data class Ready(
        val gameRoot: File,
        val canRepair: Boolean,
        val gpuDriver: String,
        val updateChannel: String,
    ) : InstallState

    /** The ISO is extracted; only Title Update 3 is missing. */
    data object AwaitingTitleUpdate : InstallState

    /** Nothing installed yet. [freeBytes] is shown so the user can judge space. */
    data class NeedsIso(val freeBytes: Long, val hasSetupLog: Boolean) : InstallState

    /**
     * Work is running and the buttons are inert. [progress] runs 0f..1f, or is
     * null when there is nothing measurable to show.
     */
    data class Busy(val status: String, val detail: String, val progress: Float?) : InstallState
}

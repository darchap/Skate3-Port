package io.skate3port.game

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.os.StatFs
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.concurrent.Executors

private const val INSTALL_HEADROOM = 512L * 1024 * 1024
private const val ISO_UNCHANGED = "\n\nYour original ISO was not changed."

/**
 * Everything that writes to disk: extract the ISO, install Title Update 3, then
 * swap the staging directory into place. Runs on one worker thread and reports
 * back through [publish]; it never touches a view.
 */
class InstallFlow(
    private val resolver: ContentResolver,
    private val storageRoot: File,
    private val gameDirectory: File,
    private val partialDirectory: File,
    private val setupLog: File,
    private val publish: (InstallState) -> Unit,
    private val onFinished: () -> Unit,
    private val onFailed: (String, Exception) -> Unit,
) {
    private val worker = Executors.newSingleThreadExecutor()
    private var status = ""
    private var lastProgressUpdate = 0L

    fun shutdown() {
        worker.shutdownNow()
    }

    /** Announce the step about to start. [progress] null means no bar. */
    fun begin(status: String, detail: String, showProgress: Boolean) {
        this.status = status
        publish(InstallState.Busy(status, detail, if (showProgress) 0f else null))
    }

    fun installFromIso(uri: Uri) = worker.execute {
        var extractionComplete = false
        try {
            deleteRecursively(partialDirectory)
            Files.createDirectories(partialDirectory.toPath())
            val descriptor: ParcelFileDescriptor = resolver.openFileDescriptor(uri, "r")
                ?: throw IOException("Android could not open the selected ISO.")
            descriptor.use { fd ->
                FileInputStream(fd.fileDescriptor).use { input ->
                    val channel = input.channel
                    val inspection = XboxIsoExtractor.inspect(channel)
                    val available = StatFs(storageRoot.absolutePath).availableBytes
                    if (available < inspection.totalBytes + INSTALL_HEADROOM) {
                        throw IOException(
                            "Not enough free space. This install needs " +
                                humanBytes(inspection.totalBytes + INSTALL_HEADROOM) +
                                ", but only " + humanBytes(available) + " is available."
                        )
                    }
                    status = "Extracting game"
                    publish(InstallState.Busy(status, "", 0f))
                    XboxIsoExtractor.extract(
                        channel, inspection, partialDirectory.toPath(), ::onExtractionProgress
                    )
                }
            }
            verifyRetailGame(partialDirectory.toPath())
            Files.write(
                partialDirectory.toPath().resolve(COMPLETE_MARKER),
                "Skate 3 retail files verified\n".toByteArray(),
            )
            extractionComplete = true
            appendLog(setupLog, "ISO extraction and retail-file verification completed.")
            downloadTitleUpdateAndFinalize()
        } catch (exception: Exception) {
            // A half-extracted staging directory is worse than none; only keep it
            // once the marker says every file landed and verified.
            if (!extractionComplete) {
                try {
                    deleteRecursively(partialDirectory)
                } catch (cleanupError: IOException) {
                    exception.addSuppressed(cleanupError)
                }
            }
            onFailed("Setup stopped: " + cleanMessage(exception) + ISO_UNCHANGED, exception)
        }
    }

    fun finishOnline() = worker.execute {
        try {
            verifyRetailGame(partialDirectory.toPath())
            downloadTitleUpdateAndFinalize()
        } catch (exception: Exception) {
            onFailed("Title Update setup stopped: " + cleanMessage(exception) + ISO_UNCHANGED,
                exception)
        }
    }

    fun installFromFile(uri: Uri) = worker.execute {
        try {
            val stream = resolver.openInputStream(uri)
                ?: throw IOException("Android could not open the selected Title Update file.")
            stream.use { input ->
                verifyRetailGame(partialDirectory.toPath())
                TitleUpdateInstaller.installPackage(input, partialDirectory.toPath())
            }
            promoteStagingDirectory()
        } catch (exception: Exception) {
            onFailed("Title Update setup stopped: " + cleanMessage(exception) + ISO_UNCHANGED,
                exception)
        }
    }

    fun importGpuDriver(context: Context, uri: Uri) = worker.execute {
        try {
            val stream = resolver.openInputStream(uri)
                ?: throw IOException("Android could not open the selected ZIP.")
            val driver = stream.use { input -> importDriverPackage(context, input) }
            appendLog(setupLog, "Imported GPU driver " + driver.label() + ".")
            onFinished()
        } catch (exception: Exception) {
            onFailed(
                "Could not import the GPU driver: " + cleanMessage(exception) +
                    "\n\nThe current driver selection was not changed.",
                exception,
            )
        }
    }

    fun wipe() = worker.execute {
        try {
            deleteRecursively(partialDirectory)
            deleteRecursively(gameDirectory)
            onFinished()
        } catch (exception: Exception) {
            onFailed("Cleanup stopped: " + cleanMessage(exception) + ISO_UNCHANGED, exception)
        }
    }

    private fun downloadTitleUpdateAndFinalize() {
        status = "Installing title update 3"
        publish(InstallState.Busy(status, "Downloading and verifying 1.7 MB...", 0f))
        TitleUpdateInstaller.downloadAndInstall(partialDirectory.toPath(), ::onDownloadProgress)
        promoteStagingDirectory()
    }

    private fun promoteStagingDirectory() {
        if (!TitleUpdateInstaller.isInstalled(partialDirectory.toPath())) {
            throw IOException("Title Update 3 did not pass final verification.")
        }
        Files.deleteIfExists(partialDirectory.toPath().resolve(COMPLETE_MARKER))
        if (gameDirectory.exists()) deleteRecursively(gameDirectory)
        try {
            Files.move(
                partialDirectory.toPath(), gameDirectory.toPath(), StandardCopyOption.ATOMIC_MOVE
            )
        } catch (exception: AtomicMoveNotSupportedException) {
            Files.move(partialDirectory.toPath(), gameDirectory.toPath())
        }
        appendLog(setupLog, "Phone-only installation completed at " + gameDirectory.absolutePath)
        onFinished()
    }

    // Throttled: the extractor reports per file, which is far faster than a screen.
    private fun onExtractionProgress(copied: Long, total: Long, currentFile: String) {
        val now = System.nanoTime()
        if (copied < total && now - lastProgressUpdate < 150_000_000L) return
        lastProgressUpdate = now
        publish(
            InstallState.Busy(
                status,
                humanBytes(copied) + " of " + humanBytes(total) + "\n" + currentFile,
                fraction(copied, total),
            )
        )
    }

    private fun onDownloadProgress(copied: Long, total: Long, message: String) {
        val amount = humanBytes(copied) + if (total > 0) " of " + humanBytes(total) else ""
        publish(InstallState.Busy(status, message + "\n" + amount, fraction(copied, total)))
    }

    private fun fraction(copied: Long, total: Long): Float =
        if (total > 0) minOf(1f, copied.toFloat() / total) else 0f
}

package io.skate3port.game

import android.app.Activity
import android.app.AlertDialog
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.StatFs
import android.provider.OpenableColumns
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts.StartActivityForResult
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import java.io.File
import java.io.FileWriter
import java.io.IOException
import java.io.PrintWriter
import java.io.StringWriter
import java.util.Locale

class LauncherActivity : ComponentActivity() {

    private lateinit var storageRoot: File
    private lateinit var gameDirectory: File
    private lateinit var partialDirectory: File
    private lateinit var setupLog: File
    private lateinit var install: InstallFlow

    private val uiState = mutableStateOf<InstallState>(
        InstallState.Busy("Checking installation", "", null)
    )

    // Busy says what is happening, not where in the flow it is, so the rail
    // remembers the last step it was told about.
    private val step = mutableIntStateOf(1)
    private var busy = false

    private val isoPicker = registerForActivityResult(StartActivityForResult()) { result ->
        // A cancelled pick can still carry a URI; without this the next line starts an
        // 8 GB extraction on a file the user never confirmed.
        if (result.resultCode != Activity.RESULT_OK) return@registerForActivityResult
        val uri = result.data?.data ?: return@registerForActivityResult
        try {
            contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
        } catch (ignored: SecurityException) {
        }
        setBusy("Inspecting ISO", "Checking " + displayName(this, uri), showProgress = false)
        install.installFromIso(uri)
    }

    private val titleUpdatePicker = registerForActivityResult(StartActivityForResult()) { result ->
        if (result.resultCode != Activity.RESULT_OK) return@registerForActivityResult
        val uri = result.data?.data ?: return@registerForActivityResult
        setBusy("Verifying title update 3", "Reading " + displayName(this, uri), true)
        install.installFromFile(uri)
    }

    private val gpuDriverPicker = registerForActivityResult(StartActivityForResult()) { result ->
        if (result.resultCode != Activity.RESULT_OK) return@registerForActivityResult
        val uri = result.data?.data ?: return@registerForActivityResult
        setBusy("Importing GPU driver", "Checking " + displayName(this, uri), showProgress = false)
        install.importGpuDriver(this, uri)
    }

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        // Opening the launcher icon over a live SDL session used to leave its
        // SurfaceView abandoned. Remove this duplicate launcher immediately and
        // reveal the game that is already underneath it.
        if (Skate3Activity.isSessionActive()) {
            finish()
            return
        }
        storageRoot = getExternalFilesDir(null) ?: filesDir
        gameDirectory = File(storageRoot, "game")
        partialDirectory = File(storageRoot, "game-installing")
        setupLog = File(filesDir, "phone-setup.log")
        install = InstallFlow(
            resolver = contentResolver,
            storageRoot = storageRoot,
            gameDirectory = gameDirectory,
            partialDirectory = partialDirectory,
            setupLog = setupLog,
            publish = ::show,
            onFinished = ::clearBusy,
            onFailed = ::showFailure,
        )

        val actions = LauncherActions(
            ::pickIso, ::launchGame, ::confirmReinstall, ::showGpuDriverMenu, ::showLog,
            ::finishSetupOnline, ::pickTitleUpdate, ::confirmStartOver, ::finish,
        )
        setContent { LauncherScreen(uiState.value, step.intValue, actions) }
        refreshInterface()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        // Shared task with the game, which hides them; ask explicitly.
        if (hasFocus) showSystemBars(this)
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing && ::install.isInitialized) install.shutdown()
    }

    /** Safe to call from the install worker; hops to the UI thread itself. */
    private fun show(next: InstallState) = runOnUiThread {
        when (next) {
            is InstallState.NeedsIso -> step.intValue = 1
            InstallState.AwaitingTitleUpdate -> step.intValue = 2
            is InstallState.Ready -> step.intValue = 3
            else -> Unit
        }
        uiState.value = next
    }

    private fun refreshInterface() {
        if (busy) return
        show(
            currentState(
                gameDirectory,
                partialDirectory,
                StatFs(storageRoot.absolutePath).availableBytes,
                setupLog.isFile,
                compatibilityProblem(this),
                selectedDriverLabel(this),
            )
        )
    }

    private fun openDocument(launch: (Intent) -> Unit, persistable: Boolean) {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType("*/*")
            .addFlags(
                if (persistable) {
                    Intent.FLAG_GRANT_READ_URI_PERMISSION or
                        Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                } else {
                    Intent.FLAG_GRANT_READ_URI_PERMISSION
                }
            )
        try {
            launch(intent)
        } catch (exception: ActivityNotFoundException) {
            showFailure("Android could not open its file picker.", exception)
        }
    }

    private fun pickIso() = openDocument(isoPicker::launch, persistable = true)

    private fun pickTitleUpdate() = openDocument(titleUpdatePicker::launch, persistable = false)

    private fun pickGpuDriver() = openDocument(gpuDriverPicker::launch, persistable = false)

    private fun showGpuDriverMenu() {
        if (!isLikelyAdrenoDevice()) {
            AlertDialog.Builder(this)
                .setTitle("GPU driver")
                .setMessage(
                    "This device does not report a Snapdragon / Adreno GPU, so a custom " +
                        "driver cannot load here. Skate 3 stays on the system driver."
                )
                .setPositiveButton("OK", null)
                .show()
            return
        }
        val driver = installedDriver(this)
        if (driver == null) {
            AlertDialog.Builder(this)
                .setTitle("GPU driver")
                .setMessage(
                    "The system driver is selected. You can import a driver package built " +
                        "for this device. Drivers are device-specific, and an incompatible " +
                        "one may crash the game at launch."
                )
                .setNegativeButton("Close", null)
                .setPositiveButton("Import driver ZIP") { _, _ -> pickGpuDriver() }
                .show()
            return
        }
        val marker = "  ·  selected"
        // A message is not shown beside a list, so the title carries the identity.
        AlertDialog.Builder(this)
            .setTitle("GPU driver · " + driver.label())
            .setItems(
                arrayOf(
                    "Use system driver" + if (driver.isEnabled) "" else marker,
                    "Use " + driver.label() + if (driver.isEnabled) marker else "",
                    "Import another driver ZIP",
                    "Remove imported driver",
                )
            ) { _, which ->
                when (which) {
                    0 -> changeDriver("Could not select the system driver") {
                        useSystemDriver(this)
                    }
                    1 -> confirmCustomGpuDriver(driver)
                    2 -> pickGpuDriver()
                    else -> confirmRemoveGpuDriver(driver)
                }
            }
            .setNegativeButton("Close", null)
            .show()
    }

    private fun confirmCustomGpuDriver(driver: GpuDriverInfo) = confirm(
        "Use " + driver.label() + "?",
        driver.vendor + " · " + driver.author +
            "\n\nOnly Skate 3 uses this driver. If the game crashes at launch, reopen the " +
            "launcher and select the system driver.",
        "Use custom driver",
    ) { changeDriver("Could not select the custom driver") { useCustomDriver(this) } }

    private fun confirmRemoveGpuDriver(driver: GpuDriverInfo) = confirm(
        "Remove " + driver.label() + "?",
        "The imported files are deleted and the system driver is selected.",
        "Remove",
    ) { changeDriver("Could not remove the imported driver") { removeDriver(this) } }

    // The refreshed row label is the confirmation; nothing else reports success.
    private fun changeDriver(failure: String, change: () -> Unit) {
        try {
            change()
            refreshInterface()
        } catch (exception: IOException) {
            showFailure(failure + ": " + cleanMessage(exception), exception)
        }
    }

    private fun finishSetupOnline() {
        setBusy("Installing title update 3", "Downloading and verifying 1.7 MB...", true)
        install.finishOnline()
    }

    private fun launchGame() {
        if (!isGameReady(gameDirectory.toPath()) && !legacyGameReady()) {
            Toast.makeText(this, "Game installation needs repair.", Toast.LENGTH_LONG).show()
            refreshInterface()
            return
        }
        startActivity(Intent(this, Skate3Activity::class.java))
        finish()
    }

    private fun confirmReinstall() = confirm(
        "Repair or reinstall?",
        "This removes the extracted game files from this app, then lets you select your ISO " +
            "again. Your original ISO is not changed.",
        "Continue",
    )

    private fun confirmStartOver() = confirm(
        "Discard partial setup?",
        "Only the incomplete copy created by this installer will be removed. Your original ISO " +
            "is not changed.",
        "Start over",
    )

    private fun confirm(
        title: String,
        message: String,
        confirmLabel: String,
        action: () -> Unit = ::startOver,
    ) {
        AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage(message)
            .setNegativeButton("Cancel", null)
            .setPositiveButton(confirmLabel) { _, _ -> action() }
            .show()
    }

    private fun startOver() {
        setBusy("Cleaning up", "Removing this app's installed copy...", showProgress = false)
        install.wipe()
    }

    private fun showLog() {
        AlertDialog.Builder(this)
            .setTitle("Setup log")
            .setMessage(readLog(setupLog))
            .setPositiveButton("OK", null)
            .show()
    }

    private fun setBusy(status: String, detail: String, showProgress: Boolean) {
        busy = true
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        install.begin(status, detail, showProgress)
    }

    private fun clearBusy() = runOnUiThread {
        busy = false
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        refreshInterface()
    }

    private fun showFailure(message: String, exception: Exception) {
        appendLog(setupLog, message + "\n" + stackTrace(exception))
        runOnUiThread {
            busy = false
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            AlertDialog.Builder(this)
                .setTitle("Setup needs attention")
                .setMessage(message)
                .setPositiveButton("OK") { _, _ -> refreshInterface() }
                .show()
        }
    }
}

/** Why this device cannot run the build, or null if it can. */
fun compatibilityProblem(context: Context): String? {
    if (Build.SUPPORTED_ABIS.none { it == "arm64-v8a" }) {
        return "This build requires a 64-bit ARM Android device."
    }
    if (Build.VERSION.SDK_INT < 33) {
        return "This build requires Android 13 or newer."
    }
    if (!context.packageManager.hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL)) {
        return "This device does not report the required Vulkan support."
    }
    return null
}

/** The picker's own name for a file, for status text. Never fails the install. */
fun displayName(context: Context, uri: Uri): String {
    try {
        context.contentResolver.query(
            uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null
        )?.use { cursor ->
            if (cursor.moveToFirst()) {
                val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (index >= 0) return cursor.getString(index)
            }
        }
    } catch (ignored: Exception) {
    }
    return "selected file"
}

/** Appends one timestamped line. A failed log must never break setup. */
@Synchronized
fun appendLog(logFile: File, text: String) {
    try {
        FileWriter(logFile, true).use { writer ->
            writer.write(
                String.format(Locale.US, "%n[%tF %<tT] %s%n", System.currentTimeMillis(), text)
            )
        }
    } catch (ignored: IOException) {
    }
}

fun readLog(logFile: File): String = try {
    if (logFile.isFile) logFile.readText() else "No setup log has been written yet."
} catch (exception: IOException) {
    cleanMessage(exception)
}

/** Exception message, or the class name when the message is blank. */
fun cleanMessage(throwable: Throwable): String {
    val message = throwable.message
    return if (message.isNullOrBlank()) throwable.javaClass.simpleName else message
}

fun stackTrace(throwable: Throwable): String {
    val text = StringWriter()
    throwable.printStackTrace(PrintWriter(text))
    return text.toString()
}

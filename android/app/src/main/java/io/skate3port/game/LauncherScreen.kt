package io.skate3port.game

import android.os.Build
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Callbacks back into the activity. */
class LauncherActions(
    @JvmField val pickIso: Runnable,
    @JvmField val play: Runnable,
    @JvmField val repair: Runnable,
    @JvmField val gpuDriver: Runnable,
    @JvmField val showLog: Runnable,
    @JvmField val finishSetup: Runnable,
    @JvmField val pickTitleUpdate: Runnable,
    @JvmField val startOver: Runnable,
    @JvmField val close: Runnable,
)

@Composable
fun LauncherScreen(state: InstallState, step: Int, actions: LauncherActions) {
    Skate3Theme {
        Surface(modifier = Modifier.fillMaxSize(), color = Skate3.Background) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .safeDrawingPadding()
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = Skate3.GapL, vertical = Skate3.GapXl),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Column(
                    modifier = Modifier.widthIn(max = Skate3.MaxContentWidth),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    BrandHeader()

                    if (state !is InstallState.Unsupported && state !is InstallState.Ready) {
                        StepRail(
                            current = step,
                            modifier = Modifier.padding(top = Skate3.GapXl),
                        )
                    }

                    StatusCard(
                        headline = headline(state),
                        detail = detail(state),
                        progress = (state as? InstallState.Busy)?.progress,
                        modifier = Modifier.padding(top = Skate3.GapXl),
                    )

                    Actions(
                        state = state,
                        a = actions,
                        modifier = Modifier.padding(top = Skate3.GapL),
                    )

                    Text(
                        "Android 13+ · ARM64 · Vulkan · about 8 GB free",
                        color = Skate3.TextFaint,
                        fontSize = 12.sp,
                        textAlign = TextAlign.Center,
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(top = Skate3.GapXl),
                    )
                }
            }
        }
    }
}

@Composable
private fun Actions(state: InstallState, a: LauncherActions, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(Skate3.GapS),
    ) {
        when (state) {
            is InstallState.Unsupported ->
                Primary("Close", null, a.close)

            is InstallState.Ready -> {
                Primary("Play Skate 3", Icons.Filled.PlayArrow, a.play)
                Secondary("GPU driver · " + state.gpuDriver, Icons.Filled.Settings, a.gpuDriver)
                if (state.canRepair) Secondary("Repair or reinstall", Icons.Filled.Refresh, a.repair)
                Tertiary("View setup log", a.showLog)
            }

            InstallState.AwaitingTitleUpdate -> {
                Primary("Finish setup automatically", null, a.finishSetup)
                Secondary("Select title update file", null, a.pickTitleUpdate)
                Tertiary("Start over", a.startOver)
            }

            is InstallState.NeedsIso -> {
                Primary("Select my Skate 3 ISO", null, a.pickIso)
                if (state.hasSetupLog) Tertiary("View last setup log", a.showLog)
            }

            is InstallState.Busy -> Unit // work in flight: nothing to offer
        }
    }
}

@Composable
private fun Primary(label: String, icon: ImageVector?, onClick: Runnable) {
    Button(
        onClick = { onClick.run() },
        shape = RoundedCornerShape(Skate3.CornerButton),
        colors = ButtonDefaults.buttonColors(
            containerColor = Skate3.Orange,
            contentColor = Color.Black,
        ),
        modifier = Modifier
            .fillMaxWidth()
            .height(56.dp),
    ) {
        if (icon != null) {
            Icon(icon, contentDescription = null, modifier = Modifier.height(20.dp))
        }
        Text(
            label,
            fontSize = 16.sp,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.padding(start = if (icon != null) Skate3.GapS else 0.dp),
        )
    }
}

@Composable
private fun Secondary(label: String, icon: ImageVector?, onClick: Runnable) {
    OutlinedButton(
        onClick = { onClick.run() },
        shape = RoundedCornerShape(Skate3.CornerButton),
        modifier = Modifier
            .fillMaxWidth()
            .height(52.dp),
    ) {
        if (icon != null) {
            Icon(icon, contentDescription = null, tint = Skate3.TextSecondary,
                modifier = Modifier.height(18.dp))
        }
        Text(
            label,
            color = Skate3.TextPrimary,
            fontSize = 15.sp,
            // A driver's own name can be 45 characters; it must not wrap the row.
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(start = if (icon != null) Skate3.GapS else 0.dp),
        )
    }
}

@Composable
private fun Tertiary(label: String, onClick: Runnable) {
    TextButton(
        onClick = { onClick.run() },
        modifier = Modifier
            .fillMaxWidth()
            .height(44.dp),
    ) {
        Text(label, color = Skate3.TextSecondary, fontSize = 14.sp)
    }
}

private fun headline(state: InstallState): String = when (state) {
    is InstallState.Unsupported -> "Device not supported"
    is InstallState.Ready -> "Ready to skate"
    InstallState.AwaitingTitleUpdate -> "Game extracted"
    is InstallState.NeedsIso -> "One file needed"
    is InstallState.Busy -> state.status
}

private fun detail(state: InstallState): String = when (state) {
    is InstallState.Unsupported -> state.reason
    is InstallState.Ready ->
        Build.MODEL + "\n" + state.gameRoot.absolutePath
    InstallState.AwaitingTitleUpdate ->
        "Download the verified 1.7 MB Title Update 3, or pick the package yourself."
    is InstallState.NeedsIso ->
        "Select the Xbox 360 ISO you dumped from your own copy. It can be in Downloads, on " +
            "an SD card, or on a USB drive. The game stays on this device.\n\n" +
            humanBytes(state.freeBytes) + " free"
    is InstallState.Busy -> state.detail
}

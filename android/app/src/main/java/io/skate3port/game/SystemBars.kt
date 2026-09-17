@file:JvmName("SystemBars")

package io.skate3port.game

import android.app.Activity
import android.view.WindowInsets
import android.view.WindowInsetsController

// SDL's setSystemUiVisibility and FLAG_FULLSCREEN are ignored once the app targets
// SDK 35 on Android 15, so WindowInsetsController is the only path left. Call these
// from onWindowFocusChanged: the bars return after dialogs and task switches.

fun hideSystemBars(activity: Activity) {
    val controller = activity.window.insetsController ?: return
    controller.hide(WindowInsets.Type.systemBars())
    controller.systemBarsBehavior =
        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
}

// The launcher shares a task with the game and inherits its hidden bars, so it has
// to ask for them back. Not hiding is not the same as showing.
fun showSystemBars(activity: Activity) {
    val controller = activity.window.insetsController ?: return
    controller.show(WindowInsets.Type.systemBars())
    controller.systemBarsBehavior = WindowInsetsController.BEHAVIOR_DEFAULT
}

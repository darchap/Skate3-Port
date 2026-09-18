package io.skate3port.game;

import android.content.Context;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ViewGroup;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;
import org.libsdl.app.SDLSurface;

import java.io.File;


public class Skate3Activity extends SDLActivity {
    private static final String INPUT_TAG = "Skate3Input";
    private static final String LIFECYCLE_TAG = "Skate3Lifecycle";
    private static volatile boolean sessionActive;
    private TouchControllerView touchController;

    // Set the instant the Activity pauses, before the surface can be destroyed.
    // The runtime clears it on foreground.
    private static native void nativeSetBackgrounded(boolean backgrounded);
    // Raised once a resume has a surface; covers the pause+resume pair SDL drops
    // when both land before its first event pump.
    private static native void nativeNotifyResumed();
    // Only a reported pause may raise the request: the first surfaceChanged at
    // launch (which starts the SDL thread) must not.
    private static boolean resumePending;

    private static void raiseForegroundRequest() {
        if (!resumePending) return;
        resumePending = false;
        notifyResumedSafe();
    }

    private static void setBackgroundedSafe(boolean backgrounded) {
        try {
            nativeSetBackgrounded(backgrounded);
        } catch (UnsatisfiedLinkError e) {
        }
    }

    private static void notifyResumedSafe() {
        try {
            nativeNotifyResumed();
        } catch (UnsatisfiedLinkError e) {
        }
    }

    // Raise the request only once the new ANativeWindow exists, like SDL's own
    // resume; earlier would rebuild the surface onto a released window.
    static final class Skate3Surface extends SDLSurface {
        Skate3Surface(Context context) {
            super(context);
        }

        boolean isReady() {
            return mIsSurfaceReady;
        }

        @Override
        public void surfaceChanged(android.view.SurfaceHolder holder,
                                   int format, int width, int height) {
            super.surfaceChanged(holder, format, width, height);
            if (mIsResumedCalled && mIsSurfaceReady) {
                raiseForegroundRequest();
            }
        }
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new Skate3Surface(context);
    }

    static boolean isSessionActive() {
        return sessionActive;
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "c++_shared", "rexruntime", "skate3" };
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        sessionActive = true;
        String files = getFilesDir().getAbsolutePath();
        nativeSetenv("XDG_DATA_HOME", files);
        nativeSetenv("HOME", files);
        nativeSetenv("SKATE3_INTERNAL_FILES_DIR", files);
        nativeSetenv("SKATE3_NATIVE_LIBRARY_DIR", getApplicationInfo().nativeLibraryDir);
        GpuDriverInfo driver = GpuDriver.installedDriver(this);
        if (driver != null && driver.isEnabled()) {
            // The loader expects a directory path that already ends in a separator.
            nativeSetenv("SKATE3_VULKAN_DRIVER_DIR",
                         driver.getDirectory().toAbsolutePath() + File.separator);
            nativeSetenv("SKATE3_VULKAN_DRIVER_NAME", driver.getLibraryName());
            Log.i("Skate3GpuDriver", "Selected custom driver: " + driver.label());
        } else {
            nativeSetenv("SKATE3_VULKAN_DRIVER_DIR", "");
            nativeSetenv("SKATE3_VULKAN_DRIVER_NAME", "");
            Log.i("Skate3GpuDriver", "Selected system driver");
        }
        GameKeepAliveService.start(this);
        touchController = new TouchControllerView(this);
        mLayout.addView(touchController, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            SystemBars.hideSystemBars(this);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Surface survived the pause: no surfaceChanged will follow.
        if (mSurface instanceof Skate3Surface && ((Skate3Surface) mSurface).isReady()) {
            raiseForegroundRequest();
        }
    }

    @Override
    protected void onPause() {
        // Before the SDL thread exists SDL sends no pause/resume; a flag set here
        // would never be cleared.
        if (mSDLThread != null) {
            setBackgroundedSafe(true);
            resumePending = true;
        }
        if (touchController != null) touchController.clearInput();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        sessionActive = false;
        GameKeepAliveService.stop(this);
        if (touchController != null) touchController.disconnect();
        // Read before super: the flag is what separates a real close from a
        // low-memory destroy, and backgrounding never reaches onDestroy at all.
        boolean finishing = isFinishing();
        Log.i(LIFECYCLE_TAG, "onDestroy finishing=" + finishing);
        super.onDestroy();
        if (finishing) {
            // Guest threads (Job Manager, dlc_enumerator, presence_thread) are
            // recompiled 360 code with no shutdown path, so the process would keep
            // running with audio until Android reclaimed it.
            Log.i(LIFECYCLE_TAG, "exiting process to stop guest threads");
            System.exit(0);
        }
    }

    private static int getAllSources(int deviceId, int eventSource) {
        InputDevice device = InputDevice.getDevice(deviceId);
        return device == null ? eventSource : eventSource | device.getSources();
    }

    private static boolean isControllerEvent(int deviceId, int eventSource) {
        int sources = getAllSources(deviceId, eventSource);
        return (sources & (InputDevice.SOURCE_GAMEPAD |
                           InputDevice.SOURCE_JOYSTICK |
                           InputDevice.SOURCE_DPAD)) != 0;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int deviceId = event.getDeviceId();
        if (isControllerEvent(deviceId, event.getSource())) {
            boolean handled = false;
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                handled = SDLControllerManager.onNativePadDown(deviceId, event.getKeyCode());
            } else if (event.getAction() == KeyEvent.ACTION_UP) {
                handled = SDLControllerManager.onNativePadUp(deviceId, event.getKeyCode());
            }
            Log.i(INPUT_TAG, "key action=" + event.getAction() +
                    " code=" + event.getKeyCode() + " device=" + deviceId +
                    " eventSources=0x" + Integer.toHexString(event.getSource()) +
                    " allSources=0x" + Integer.toHexString(getAllSources(deviceId, event.getSource())) +
                    " handled=" + handled);
            if (handled) {
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (isControllerEvent(event.getDeviceId(), event.getSource()) &&
                SDLControllerManager.handleJoystickMotionEvent(event)) {
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

}

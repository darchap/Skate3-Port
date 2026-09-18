# Local patches to vendored code

Every edit made to `third_party/rexglue-sdk` and the build files, kept exact so the
SDK can be re-vendored and the patches replayed.

1. `third_party/rexglue-sdk/thirdparty/CMakeLists.txt`: submodule check accepts a
   populated directory (tree is vendored without `.git` markers). Two lines, commented.
2. `CMakePresets.json`: `android-base` condition allows Darwin, Linux, Windows hosts;
   new `windows-host` configure + build preset.
3. `third_party/rexglue-sdk/src/input/sdl/sdl_input_driver.cpp`: the JNI export is
   `Java_io_skate3port_game_TouchControllerBridge_setState` (was `Java_chat_buku_skate3_...`).
   It must match the Java package of `TouchControllerBridge` exactly or touch input
   silently stops (the Java side catches `UnsatisfiedLinkError`). Lives in
   `librexruntime.so`, so a package rename needs a native rebuild.
4. `third_party/rexglue-sdk/src/audio/audio_system.cpp`: guest audio-frame credits are
   dispatched on an even 5.33 ms beat (256 samples at 48 kHz) instead of device-callback
   bursts. Cvar `audio_paced_credit_dispatch` (default on, hot-reload) turns it off for
   A/B tests. Fixes rhythmic crackle: guest content duty 27-35% -> 99-100%.
5. `third_party/rexglue-sdk/src/ui/overlay/simple_settings_overlay.{cpp,h}`: "Auto-Boot to
   Gameplay" row in the System tab (member `auto_boot_`) toggling `skate3_demo_path`,
   which is added to `kOptionalSimpleSettingsCvars` (27 -> 28) so the choice survives
   restarts. The defaults live in our `src/skate3_demo_path.cpp`: `skate3_demo_path` and
   `skate3_demo_path_signed_in` both true on Android, false elsewhere. `signed_in` is not
   user-facing and not persisted; it keeps the player's own profile and save. Hooks
   install at boot only, so a mid-session toggle applies at the next launch; switching off
   mid-game stops the button presses at once.
6. `simple_settings_overlay.{cpp,h}` (settings own the graphics): the "Android Device
   Profile" picker stays with a single option, `kAndroidQualityProfileLabels = {"Custom"}`
   (member `android_quality_profile_index_`), as the slot for future profiles; "3D Scene
   Resolution" is a real selector (`kAndroidSceneRes*` tables, `AndroidSceneResIndexFromCvar`,
   member `android_scene_res_index_`); under a "Low-End Devices" header after
   Volumetrics: Draw Batching, Hair Detail, Water Effects (cvars
   `skate3_native_render_scene_{merge_draws,hair_single_pass,water_effects}`, members
   `merge_draws_`, `hair_full_`, `water_effects_`), then Vegetation & Foliage, Pedestrians &
   Traffic, Movable Props, Clutter Detail (cvars `skate3_native_render_scene_{vegetation,
   ambient_npcs,movable_props,clutter_detail}`) and World Texture Layers (drives the
   existing `lightmaps/macro/decals`); members `vegetation_`, `ambient_npcs_`,
   `movable_props_`, `clutter_detail_`, `world_texture_layers_`;
   `kOptionalSimpleSettingsCvars` 28 -> 42 (the three cvars above, the four content cvars, the
   three texture-layer cvars, the two scene caps, `lw_update_refresh`,
   `guest_static_refresh`; the profile cvar was already listed). NPC Update Rate and
   World Update Rate rows (`kNpcUpdateRates` 1/2/3/4, `kWorldRefreshRates` 1/2/4/8,
   `NearestRateIndex`, members `npc_update_rate_index_`, `world_refresh_index_`); both
   cvars default to 1 on every platform and their gates in `skate3_native_render.cpp` no
   longer require `handheld_potato`; `kAndroidBaseline` no longer pins them. App side, ours: the two preset tables and the renderer's
   `Install()` force-off are gone; `skate3_android_quality_profile` has only value 0.
   `kAndroidBaseline` in `skate3_app_common.cpp` pins ~20 internal cvars with no menu row
   every boot; `kFirstRunAndroid` writes lean values into the settings file on first launch
   only (potato not among them; `lightmaps/macro/decals` keep their code default, on,
   until they get a row: baked lighting carries the game's own shadows); the
   `handheld_potato` cvar, the lean PSO family (`g_r.lean_pipelines`) and every
   `HandheldPotato*` helper are deleted, outline/spline/shadow PSOs are always built
   (`skate3_native_scene*.cpp`); `EnsureOutputSizedTargets` reads
   `skate3_android_scene_{width,height}_cap` unconditionally (0 = 1280x720). Future
   presets: an "apply once" menu action, never a boot-time table.

7b. `simple_settings_overlay.{cpp,h}`: `kFrameCapRates` 6 -> 10 and `kFrameCapLabels` 7 -> 11,
   sub-60 caps 30/40/45/50 (40 divides 120 Hz, 45 divides 90 Hz). The cap applies live via
   `ApplyFrameCap()` (called from `SaveVideo`, the row's `on_enum_change` and both reset
   branches) and no longer counts toward the pending-restart check.
7. Android app lifecycle (merged to `main` as 92372fdd), ported from the earlier build's verified
   design. SDK: `ui/app_lifecycle_listener.h` (new interface), `ui/android_background_state.{h,cpp}`
   (new; the ONE background flag plus a foreground-request flag, defined in librexruntime
   with default visibility, never inline; the request lets the UI loop synthesise a
   foreground when SDL coalesced a pause+resume pair before its first pump), `ui/windowed_app_context.h` + `ui/windowed_app_context_sdl.{h,cpp}` (an SDL_AddEventWatch
   installed in the context constructor records lifecycle events, SDL never queues them;
   they are handled in RunMainLoop after the wait, never inside the watch: SDL holds its
   watcher lock there and other threads' SDL_PushEvent would block), `ui/windowed_app_main_sdl.cpp` (SDL_HINT_ANDROID_BLOCK_ON_PAUSE
   off), `ui/rex_app.{h,cpp}` (background = audio Pause + vblank gate; foreground = rebuild
   surface, clear flag, kick paint, ungate, Resume; never tear down Vulkan or suspend threads),
   `ui/presenter.cpp` (no present while backgrounded, clear the paint latch, ignore GPU loss,
   33 ms swap sleep), `ui/window.h` + `ui/window_sdl.cpp` (NotifySurfaceChanged, rebuild on
   the resize that follows a new ANativeWindow), `audio/audio_system.{h,cpp}` (pause check at
   the top of the worker loop), `graphics/graphics_system.{h,cpp}` + `system/interfaces/graphics.h`
   (SetBackgroundPaused vblank gate), `system/interfaces/audio.h` (virtual Pause/Resume),
   `core/timer_queue.cpp` (skip missed periods), `src/ui/CMakeLists.txt` (new source). App:
   `src/skate3_android_lifecycle.cpp` (JNI `nativeSetBackgrounded` and `nativeNotifyResumed`),
   `Skate3Activity` sets the flag in onPause, raises the foreground request only once the
   surface is ready (its `Skate3Surface` subclass, after `surfaceChanged`, or in onResume when
   the surface survived), and starts/stops `GameKeepAliveService` (foreground service + notification, the
   reason the process is not killed), manifest permissions and service entry. Rejected on
   device and not ported: guest-thread suspension, OS freeze, parking at the swap safepoint.

8. Benchmark and counters (main): `src/input/input_system.{cpp,h}` zeroes the guest pad
   while the menu chord is held (`chord_swallow_`) and, after the overlay closes, until
   every button is up and both triggers read below 30 (`release_latch_`);
   `src/ui/presenter.cpp` + `include/rex/ui/presenter.h` add `low_1pct_fps`, `p95_ms`,
   `p99_ms` to `GuestFrameStats` (intervals over the last 4 s of the timestamp ring);
   `src/ui/overlay/fps_overlay.cpp` defines and draws `show_fps_percentiles`;
   `simple_settings_overlay.{cpp,h}`: "FPS Percentiles" row (`fps_percentiles_`), the
   "Renderer Indicator" row and `mode_indicator_` removed, a "Benchmark" header with the
   "Run Benchmark" action at the end of Video, persisted list 32 -> 31
   (`show_fps_percentiles` in, `skate3_native_render_mode_indicator` out). App side, ours:
   `skate3_benchmark_frames/_warmup`, `skate3_bench_run`, the flythrough in
   `skate3_native_scene.cpp`, `BenchmarkTick`/`LastBenchmarkResult` in
   `skate3_native_scene_gpu.cpp`, the result panel in `skate3_native_debug_dialog.cpp`.

9. `simple_settings_overlay.cpp` (branch low-end-settings): "Pedestrians & Traffic" and
   "Movable Props" are pending-style rows like the graphics API row: the toggle only sets
   `ambient_npcs_` / `movable_props_`, `HasSettingsChanges` compares them with the cvars,
   `SaveVideo` writes them, so X / Apply & Restart persists and restarts. App side, ours:
   the two cvars are `kRequiresRestart`, read once through `AmbientNpcsAtBoot()` /
   `MovablePropsAtBoot()`, and three guest hooks in `skate3_native_render.cpp` return the
   census spawners' empty result (`sub_82E22F30` pedestrians, `sub_82C36300` vehicles,
   `sub_82C4D440` movable props).


10. `simple_settings_overlay.cpp` (branch low-end-settings): "World Texture Layers"
    loads `world_texture_layers_` as the AND of the lightmaps/macro/decals cvars, not the
    OR. The row writes all three together, so an OR made a mixed state - reachable from
    the debug dialog's independent checkboxes - read as "Full", and a row already showing
    "Full" cannot be re-selected to put the missing layers back.

11. `third_party/rexglue-sdk/{include/rex/system/kernel_state.h,
    src/system/kernel_state.cpp, src/kernel/xam/xam_user.cpp, xam_content.cpp,
    xam_content_device.cpp, xam_info.cpp, xam_msg.cpp, include/rex/system/xtypes.h}`
    (branch fix-overlapped-wakeup): XAM query APIs complete their overlapped on the kernel
    dispatch thread (`CompleteOverlappedDeferredNow`, no delay) instead of inside the
    call. Signalling the event before the caller, told `X_ERROR_IO_PENDING`, has armed its
    wait lost the wake-up: `dlc_enumerator` parked on an already-signalled event while
    holding a critical section, the main thread blocked behind it, and the frontend froze
    after Start with `render_thread` spinning. `XamAlloc` reports heap exhaustion instead
    of a null pointer under a success code. Upstream runtime commit `8dd8b36`, those files
    only; one divergence: `XamUserContentRestrictionCheckAccess` returns
    `X_ERROR_IO_PENDING` once it has queued its completion (upstream still returns success
    there).

Everything else in `third_party/` is byte-identical to upstream (verified). If you must
touch it, add an entry here in the same commit as the patch.

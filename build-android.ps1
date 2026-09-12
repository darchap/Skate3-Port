<#
.SYNOPSIS
  Build the Skate 3 Android APK on a Windows host.

.DESCRIPTION
  Windows equivalent of build-android.sh (which only supports macOS/Linux).
  Pipeline:
    1. Host codegen  : cmake preset "windows-host"    -> generated/  (skipped when present)
    2. Native libs   : cmake preset "android-release" -> out/build/android-release/*.so
    3. Stage libs    : copy .so files into android/app/libs/arm64-v8a/
    4. APK           : android\gradlew.bat assembleDebug | assembleRelease
    5. Verify        : 16 KB page alignment of every .so, zipalign check
  Retail game data is never packaged; only game/default.xex and
  game/data/webkit/EAWebkit.xex are read for codegen.

.PARAMETER Release      Build a signed release APK (needs SKATE3_RELEASE_* env vars).
.PARAMETER Install      adb install the finished APK.
.PARAMETER Regenerate   Delete generated/ and rerun codegen.
.PARAMETER SkipApk      Stop after staging native libs.
#>
[CmdletBinding()]
param(
  [switch]$Release,
  [switch]$Install,
  [switch]$Regenerate,
  [switch]$SkipApk
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

function Step($m) { Write-Host "`n==> $m" -ForegroundColor Cyan }
function Die($m)  { Write-Host "error: $m" -ForegroundColor Red; exit 1 }
function Need($cmd, $hint) { if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) { Die "$cmd not found. $hint" } }

# ---- toolchain -------------------------------------------------------------
Need cmake  "Install CMake 3.25+ (winget install Kitware.CMake)"
Need ninja  "Install Ninja (winget install Ninja-build.Ninja)"
if (-not (Test-Path 'C:\Program Files\LLVM\bin\clang++.exe')) { Die 'LLVM/Clang 18+ is required at C:\Program Files\LLVM (winget install LLVM.LLVM)' }

$sdk = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } elseif ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { "$env:LOCALAPPDATA\Android\Sdk" }
if (-not (Test-Path "$sdk\platforms\android-35\android.jar")) { Die "Android SDK platform 35 not found under $sdk (set ANDROID_SDK_ROOT)" }
$ndkVersion = '27.2.12479018'
$ndk = if ($env:ANDROID_NDK_ROOT) { $env:ANDROID_NDK_ROOT } else { "$sdk\ndk\$ndkVersion" }
if (-not (Test-Path "$ndk\build\cmake\android.toolchain.cmake")) { Die "Android NDK $ndkVersion not found at $ndk (set ANDROID_NDK_ROOT)" }
$buildTools = "$sdk\build-tools\35.0.0"
if (-not (Test-Path "$buildTools\zipalign.exe")) { Die "Android build-tools 35.0.0 missing under $sdk" }
if (-not $env:JAVA_HOME) { Die 'JAVA_HOME must point to a JDK 17+' }

$env:ANDROID_HOME     = $sdk
$env:ANDROID_SDK_ROOT = $sdk
$env:ANDROID_NDK_ROOT = ($ndk -replace '\\', '/')

# ---- inputs ----------------------------------------------------------------
if (-not (Test-Path "$root\game\default.xex"))              { Die 'missing game\default.xex' }
if (-not (Test-Path "$root\game\data\webkit\EAWebkit.xex")) { Die 'missing game\data\webkit\EAWebkit.xex' }
if (-not (Test-Path "$root\TU_12K2276_000000C000000.00000000000O3")) { Die 'missing Title Update 3 package at repo root' }
if (-not (Test-Path "$root\third_party\rexglue-sdk\CMakeLists.txt")) { Die 'third_party\rexglue-sdk is missing' }

# ---- 1. host codegen -------------------------------------------------------
if ($Regenerate -and (Test-Path "$root\generated")) { Remove-Item -Recurse -Force "$root\generated" }
if (-not (Test-Path "$root\generated\sources.cmake") -or -not (Test-Path "$root\generated\eawebkit\sources.cmake")) {
  Step 'Configuring Windows host build (codegen tool)'
  cmake --preset windows-host; if ($LASTEXITCODE) { Die 'host configure failed' }
  Step 'Generating recompiled C++ from default.xex + EAWebkit.xex'
  cmake --build --preset windows-host --parallel; if ($LASTEXITCODE) { Die 'codegen failed' }
} else {
  Step 'generated/ present, skipping codegen (use -Regenerate to redo)'
}

# ---- 2. native ARM64 libraries ---------------------------------------------
Step 'Configuring Android ARM64 release build'
cmake --preset android-release; if ($LASTEXITCODE) { Die 'android configure failed' }
Step 'Building libskate3.so + librexruntime.so (this is the slow part)'
cmake --build --preset android-release --parallel; if ($LASTEXITCODE) { Die 'android build failed' }

# ---- 3. stage libs ---------------------------------------------------------
Step 'Staging native libraries into android\app\libs\arm64-v8a'
$jni = "$root\android\app\libs\arm64-v8a"
New-Item -ItemType Directory -Force $jni | Out-Null
$b = "$root\out\build\android-release"
$cxxShared = Get-ChildItem "$ndk\toolchains\llvm\prebuilt" -Recurse -Filter 'libc++_shared.so' |
  Where-Object { $_.FullName -match 'aarch64-linux-android' } | Select-Object -First 1
if (-not $cxxShared) { Die 'libc++_shared.so not found in the NDK' }
$libs = @(
  "$b\libskate3.so",
  "$b\librexruntime.so",
  "$b\third_party\libadrenotools\src\hook\libhook_impl.so",
  "$b\third_party\libadrenotools\src\hook\libmain_hook.so",
  $cxxShared.FullName
)
foreach ($f in $libs) {
  if (-not (Test-Path $f)) { Die "expected build output missing: $f" }
  Copy-Item -Force $f $jni
}
Get-ChildItem $jni | ForEach-Object { '  {0,-24} {1,14:N0} bytes' -f $_.Name, $_.Length }

# 16 KB page alignment check (Android 15+ devices)
$readelf = Get-ChildItem "$ndk\toolchains\llvm\prebuilt" -Recurse -Filter 'llvm-readelf.exe' | Select-Object -First 1
foreach ($so in Get-ChildItem "$jni\*.so") {
  $loads = & $readelf.FullName -lW $so.FullName | Where-Object { $_ -match '^\s*LOAD' }
  foreach ($l in $loads) {
    $align = [Convert]::ToInt64((($l.Trim() -split '\s+')[-1]), 16)
    if ($align -lt 0x4000) { Die "$($so.Name) has a LOAD segment below 16 KB alignment ($align)" }
  }
}
if ($SkipApk) { Write-Host "`nNative libraries staged. Skipping APK."; exit 0 }

# ---- 4. APK ----------------------------------------------------------------
New-Item -ItemType Directory -Force "$root\out" | Out-Null
Push-Location "$root\android"
try {
  if ($Release) {
    foreach ($v in 'SKATE3_RELEASE_KEYSTORE', 'SKATE3_RELEASE_STORE_PASSWORD', 'SKATE3_RELEASE_KEY_ALIAS', 'SKATE3_RELEASE_KEY_PASSWORD') {
      if (-not (Get-Item "env:$v" -ErrorAction SilentlyContinue)) { Die "$v is required for a release build" }
    }
    Step 'Building release APK'
    .\gradlew.bat --no-daemon assembleRelease; if ($LASTEXITCODE) { Die 'gradle assembleRelease failed' }
    $apk = "$root\android\app\build\outputs\apk\release\app-release.apk"
    $out = "$root\out\Skate3-Port.apk"
  } else {
    Step 'Building debug APK'
    .\gradlew.bat --no-daemon assembleDebug; if ($LASTEXITCODE) { Die 'gradle assembleDebug failed' }
    $apk = "$root\android\app\build\outputs\apk\debug\app-debug.apk"
    $out = "$root\out\Skate3-Port-debug.apk"
  }
} finally { Pop-Location }
if (-not (Test-Path $apk)) { Die "Gradle finished but no APK at $apk" }
Copy-Item -Force $apk $out

# ---- 5. verify -------------------------------------------------------------
Step 'Verifying APK alignment'
& "$buildTools\zipalign.exe" -c -P 16 4 $out; if ($LASTEXITCODE) { Die 'APK is not zip-aligned for 16 KB devices' }
if ($Release) {
  & "$buildTools\apksigner.bat" verify --verbose $out | Out-Null
  if ($LASTEXITCODE) { Die 'release APK signature verification failed' }
}

if ($Install) {
  Need adb 'Install Android platform-tools'
  Step 'Installing APK'
  adb install -r $out; if ($LASTEXITCODE) { Die 'adb install failed' }
}

Write-Host "`nBuild complete:`n  $out`n" -ForegroundColor Green
Write-Host 'The APK contains no retail game data. On the phone, use "Select My Skate 3 ISO" on first launch.'

param(
 [Parameter(Mandatory=$true)][string]$Jdk,
 [Parameter(Mandatory=$true)][string]$Sdk,
 [Parameter(Mandatory=$true)][string]$Ndk,
 [Parameter(Mandatory=$true)][string]$KeyStore,
 # For variants kept outside this repository: more Java source folders, another manifest, C files
 # built into lib<name>.so, prebuilt native libraries. Nothing here uses them.
 [string[]]$ExtraSources = @(),
 [string]$Manifest = '',
 [string[]]$ExtraNative = @(),
 [string[]]$ExtraLibs = @(),
 # Diagnostic build only: a Vulkan layer library to bundle as an explicit app layer, e.g. RenderDoc
 # (libVkLayer_GLES_RenderDoc.so, ME_RENDERDOC=1) or Khronos validation
 # (libVkLayer_khronos_validation.so, ME_VALIDATION=1). Goes to work\private-apk\, never published.
 [string]$Layer = '',
 # Folder with libadrenotools.so and its hook libraries (android/turnip/build_adrenotools.sh):
 # bundled so the user can load an imported GPU driver package (settings, onboarding). Native
 # libraries are extracted (AndroidManifest extractNativeLibs=true; the hooks must be in
 # nativeLibraryDir). Without it the app always uses the device's own Vulkan driver.
 [string]$AdrenoTools = '',
 # Diagnostic build only: folder with the libadrenotools files above plus the Turnip driver
 # libvulkan_freedreno.so (ME_TURNIP=1 without an imported package). Goes to work\private-apk\
 # as RunnerRecompiled-turnip.apk, never published.
 [string]$Turnip = '',
 # Private build only: the PC version (work\pc-recomp\bin-android\libmepc.so, lifted from the
 # user's GOG copy) and DXVK Native (libdxvk_d3d9.so, pc/dxvk). Goes to work\private-apk\.
 [string]$PcRuntime = '',
 [string]$PcDxvk = '',
 # Personal hand-over builds of the PC version only: folder with msvcr80.dll, msvcp80.dll and
 # rebased\PhysXCore.dll, rebased\PhysXCooking.dll (not part of a game copy), put into assets/pcdeps.
 [string]$PcDeps = '',
 # Where to write the signed APK instead of the default name (pc/tools/selfbuild.py).
 [string]$OutApk = ''
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$build=Join-Path (Split-Path $root -Parent) 'build\android'
$bt=Join-Path $Sdk 'build-tools\35.0.0'
$java=Join-Path $Jdk 'bin'
$android=Join-Path $Sdk 'platforms\android-35\android.jar'
$clang=Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe'
$env:JAVA_HOME=$Jdk
function Check { if($LASTEXITCODE -ne 0){throw "Tool exited with $LASTEXITCODE"} }
New-Item -ItemType Directory -Force "$build\classes","$build\dex","$build\native\lib\arm64-v8a" | Out-Null
$sourceDirs = @(Join-Path $root 'src') + $ExtraSources
$sources=Get-ChildItem $sourceDirs -Recurse -Filter '*.java' | ForEach-Object FullName
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$build\classes"
New-Item -ItemType Directory -Force "$build\classes" | Out-Null
& "$java\javac.exe" -encoding UTF-8 --release 8 -classpath $android -d "$build\classes" @sources
Check
# All classes in one jar for d8: a list of class files outgrows cmd.exe's command line (d8.bat).
Remove-Item -Force -ErrorAction SilentlyContinue "$build\classes.jar"
& "$java\jar.exe" --create --file "$build\classes.jar" -C "$build\classes" .
Check
& "$bt\d8.bat" --lib $android --min-api 26 --output "$build\dex" "$build\classes.jar"
Check
# JNI glue (loads libmepc.so when the APK has it; no game code itself).
& $clang --target=aarch64-linux-android26 -shared -fPIC -O2 '-Wl,-z,max-page-size=16384' (Join-Path $root 'native\pc_jni.c') -o "$build\native\lib\arm64-v8a\libmepcjni.so" -llog -landroid
Check
# Resources (adaptive launcher icon in res/): compile to flat files, then link with the manifest.
Remove-Item -Force -ErrorAction SilentlyContinue "$build\res.zip"
& "$bt\aapt2.exe" compile --dir (Join-Path $root 'res') -o "$build\res.zip"
Check
$manifest=$(if ($Manifest) { $Manifest } else { Join-Path $root 'AndroidManifest.xml' })
& "$bt\aapt2.exe" link "$build\res.zip" -I $android --manifest $manifest --min-sdk-version 26 --target-sdk-version 35 --version-code 12 --version-name '0.3.1' -o "$build\base.apk"
Check
$nativeLibs = @('lib/arm64-v8a/libmepcjni.so')
# Earlier extras are not left behind: everything but the libraries below is rebuilt or copied anew.
Get-ChildItem "$build\native\lib\arm64-v8a" -Filter '*.so' | Where-Object Name -notin 'libmepcjni.so' | Remove-Item -Force
foreach ($c in $ExtraNative) {
 $name = 'lib' + [IO.Path]::GetFileNameWithoutExtension($c) + '.so'
 & $clang --target=aarch64-linux-android26 -shared -fPIC -O2 '-Wl,-z,max-page-size=16384' $c -o "$build\native\lib\arm64-v8a\$name" -llog
 Check
 $nativeLibs += "lib/arm64-v8a/$name"
}
foreach ($lib in $ExtraLibs) {
 Copy-Item -Force $lib "$build\native\lib\arm64-v8a\$(Split-Path $lib -Leaf)"
 $nativeLibs += "lib/arm64-v8a/$(Split-Path $lib -Leaf)"
}
foreach ($lib in 'libmepc.so','libdxvk_d3d9.so') { Remove-Item -Force -ErrorAction SilentlyContinue "$build\native\lib\arm64-v8a\$lib" }
if ($PcRuntime) {
 Copy-Item -Force $PcRuntime "$build\native\lib\arm64-v8a\libmepc.so"
 Copy-Item -Force $PcDxvk "$build\native\lib\arm64-v8a\libdxvk_d3d9.so"
 $nativeLibs += 'lib/arm64-v8a/libmepc.so', 'lib/arm64-v8a/libdxvk_d3d9.so'
}
Get-ChildItem "$build\native\lib\arm64-v8a" -Filter 'libVkLayer_*.so' | Remove-Item -Force
if ($Layer) {
 $layerName = Split-Path $Layer -Leaf
 Copy-Item -Force $Layer "$build\native\lib\arm64-v8a\$layerName"
 $nativeLibs += "lib/arm64-v8a/$layerName"
}
$adrenoLibs = @('libadrenotools.so','libhook_impl.so','libmain_hook.so','libfile_redirect_hook.so','libgsl_alloc_hook.so')
foreach ($lib in $adrenoLibs + 'libvulkan_freedreno.so') { Remove-Item -Force -ErrorAction SilentlyContinue "$build\native\lib\arm64-v8a\$lib" }
if ($Turnip -and -not $AdrenoTools) { $AdrenoTools = $Turnip }
$bundled = @()
if ($AdrenoTools) { $bundled += $adrenoLibs | ForEach-Object { @{ From = (Join-Path $AdrenoTools $_); Name = $_ } } }
if ($Turnip) { $bundled += @{ From = (Join-Path $Turnip 'libvulkan_freedreno.so'); Name = 'libvulkan_freedreno.so' } }
foreach ($lib in $bundled) {
 Copy-Item -Force $lib.From "$build\native\lib\arm64-v8a\$($lib.Name)"
 $nativeLibs += "lib/arm64-v8a/$($lib.Name)"
}
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$build\native\assets"
if ($PcDeps) {
 New-Item -ItemType Directory -Force "$build\native\assets\pcdeps" | Out-Null
 foreach ($dep in 'msvcr80.dll','msvcp80.dll','rebased\PhysXCore.dll','rebased\PhysXCooking.dll') {
  Copy-Item -Force (Join-Path $PcDeps $dep) "$build\native\assets\pcdeps\$(Split-Path $dep -Leaf)"
  $nativeLibs += "assets/pcdeps/$(Split-Path $dep -Leaf)"
 }
}
$jarLibs = foreach ($lib in $nativeLibs) { '-C'; "$build\native"; $lib }  # -C applies to the next file only
& "$java\jar.exe" --update --no-compress --file "$build\base.apk" -C "$build\dex" classes.dex @jarLibs
Check
& "$bt\zipalign.exe" -P 16 -f 4 "$build\base.apk" "$build\aligned.apk"
Check
if(!(Test-Path -LiteralPath $KeyStore)){
 & "$java\keytool.exe" -genkeypair -noprompt -keystore $KeyStore -storepass android -keypass android -alias androiddebugkey -dname 'CN=Android Debug,O=Research,C=US' -keyalg RSA -keysize 2048 -validity 3650
 Check
}
$apk=Join-Path (Split-Path $root -Parent) 'RunnerRecompiled.apk'
if (!$OutApk -and ($ExtraLibs -or $Layer -or $Turnip -or $PcRuntime)) {
 $privateDir=Join-Path (Split-Path (Split-Path (Split-Path $root -Parent) -Parent) -Parent) 'work\private-apk'
 New-Item -ItemType Directory -Force $privateDir | Out-Null
 $apk=Join-Path $privateDir $(if ($Turnip -and $Layer) { 'RunnerRecompiled-turnip-layer.apk' } elseif ($Turnip) { 'RunnerRecompiled-turnip.apk' } elseif ($Layer -like '*khronos_validation*') { 'RunnerRecompiled-validation.apk' } elseif ($Layer) { 'RunnerRecompiled-renderdoc.apk' } elseif ($PcRuntime) { 'RunnerRecompiled-pc-private.apk' } else { 'RunnerRecompiled-private.apk' })
}
if ($OutApk) { $apk = $OutApk }
& "$bt\apksigner.bat" sign --ks $KeyStore --ks-pass pass:android --key-pass pass:android --out $apk "$build\aligned.apk"
Check
& "$bt\apksigner.bat" verify --verbose $apk
Check
& "$bt\zipalign.exe" -c -P 16 4 $apk
Check
Write-Output "APK: $apk"

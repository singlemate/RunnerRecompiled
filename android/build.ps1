param(
 [Parameter(Mandatory=$true)][string]$Jdk,
 [Parameter(Mandatory=$true)][string]$Sdk,
 [Parameter(Mandatory=$true)][string]$Ndk,
 [Parameter(Mandatory=$true)][string]$KeyStore,
 # Private build only: path to libmeruntime.so (recompiled game code) to bundle into the APK.
 # The result goes to work\private-apk\ and must never be published.
 [string]$Runtime = '',
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
 [string]$Turnip = ''
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
$sources=Get-ChildItem (Join-Path $root 'src') -Recurse -Filter '*.java' | ForEach-Object FullName
& "$java\javac.exe" -encoding UTF-8 --release 8 -classpath $android -d "$build\classes" @sources
Check
$classFiles=Get-ChildItem "$build\classes" -Recurse -Filter '*.class' | ForEach-Object FullName
& "$bt\d8.bat" --lib $android --min-api 26 --output "$build\dex" @classFiles
Check
& $clang --target=aarch64-linux-android26 -c -fPIC -O2 (Join-Path $root 'native\probe.c') -o "$build\probe.o"
Check
& $clang --target=aarch64-linux-android26 -shared '-Wl,-z,max-page-size=16384' "$build\probe.o" -o "$build\native\lib\arm64-v8a\libmeprobe.so"
Check
# Resources (adaptive launcher icon in res/): compile to flat files, then link with the manifest.
Remove-Item -Force -ErrorAction SilentlyContinue "$build\res.zip"
& "$bt\aapt2.exe" compile --dir (Join-Path $root 'res') -o "$build\res.zip"
Check
$manifest=Join-Path $root 'AndroidManifest.xml'
& "$bt\aapt2.exe" link "$build\res.zip" -I $android --manifest $manifest --min-sdk-version 26 --target-sdk-version 35 --version-code 1 --version-name '0.1-research' -o "$build\base.apk"
Check
$nativeLibs = @('lib/arm64-v8a/libmeprobe.so')
if ($Runtime) {
 Copy-Item -Force $Runtime "$build\native\lib\arm64-v8a\libmeruntime.so"
 $nativeLibs += 'lib/arm64-v8a/libmeruntime.so'
} else {
 Remove-Item -Force -ErrorAction SilentlyContinue "$build\native\lib\arm64-v8a\libmeruntime.so"
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
if ($Runtime -or $Layer -or $Turnip) {
 $privateDir=Join-Path (Split-Path (Split-Path (Split-Path $root -Parent) -Parent) -Parent) 'work\private-apk'
 New-Item -ItemType Directory -Force $privateDir | Out-Null
 $apk=Join-Path $privateDir $(if ($Turnip -and $Layer) { 'RunnerRecompiled-turnip-layer.apk' } elseif ($Turnip) { 'RunnerRecompiled-turnip.apk' } elseif ($Layer -like '*khronos_validation*') { 'RunnerRecompiled-validation.apk' } elseif ($Layer) { 'RunnerRecompiled-renderdoc.apk' } else { 'RunnerRecompiled-private.apk' })
}
& "$bt\apksigner.bat" sign --ks $KeyStore --ks-pass pass:android --key-pass pass:android --out $apk "$build\aligned.apk"
Check
& "$bt\apksigner.bat" verify --verbose $apk
Check
& "$bt\zipalign.exe" -c -P 16 4 $apk
Check
Write-Output "APK: $apk"

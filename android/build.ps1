param(
 [Parameter(Mandatory=$true)][string]$Jdk,
 [Parameter(Mandatory=$true)][string]$Sdk,
 [Parameter(Mandatory=$true)][string]$Ndk,
 [Parameter(Mandatory=$true)][string]$KeyStore
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
& "$bt\aapt2.exe" link -I $android --manifest (Join-Path $root 'AndroidManifest.xml') --min-sdk-version 26 --target-sdk-version 35 --version-code 1 --version-name '0.1-research' -o "$build\base.apk"
Check
& "$java\jar.exe" --update --no-compress --file "$build\base.apk" -C "$build\dex" classes.dex -C "$build\native" lib/arm64-v8a/libmeprobe.so
Check
& "$bt\zipalign.exe" -P 16 -f 4 "$build\base.apk" "$build\aligned.apk"
Check
if(!(Test-Path -LiteralPath $KeyStore)){
 & "$java\keytool.exe" -genkeypair -noprompt -keystore $KeyStore -storepass android -keypass android -alias androiddebugkey -dname 'CN=Android Debug,O=Research,C=US' -keyalg RSA -keysize 2048 -validity 3650
 Check
}
$apk=Join-Path (Split-Path $root -Parent) 'mirrors-edge-research.apk'
& "$bt\apksigner.bat" sign --ks $KeyStore --ks-pass pass:android --key-pass pass:android --out $apk "$build\aligned.apk"
Check
& "$bt\apksigner.bat" verify --verbose $apk
Check
& "$bt\zipalign.exe" -c -P 16 4 $apk
Check
Write-Output "APK: $apk"



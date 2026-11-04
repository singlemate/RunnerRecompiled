# Copies the user's own PC game files to an Android device over adb for PcGameActivity and,
# optionally, installs the private APK. Nothing is downloaded and nothing is published: the files
# go from this computer to the app's own folder on the device (removed with the app).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File pc\tools\pc-device-setup.ps1 `
#       -Adb work\toolchains\android-sdk\platform-tools\adb.exe -Game work\pc -Deps work\pc-deps `
#       [-Apk work\private-apk\RunnerRecompiled-pc-private.apk] [-Serial <device>] [-SkipGame]
#
# On the device (files/pc of org.mirrorsedge.research):
#   MirrorsEdge\Binaries, Engine, TdGame   from -Game (the GOG install; installers, the uninstaller
#                                          and Redistributable are left out)
#   deps\msvcr80.dll, msvcp80.dll, rebased\  from -Deps (what build_pc_runtime.py lifted)
# A file is pushed again only when its size differs, so a second run is quick.
param(
 [Parameter(Mandatory=$true)][string]$Adb,
 [Parameter(Mandatory=$true)][string]$Game,
 [Parameter(Mandatory=$true)][string]$Deps,
 [string]$Apk = '',
 [string]$Serial = '',
 [switch]$SkipGame
)
$ErrorActionPreference = 'Stop'
$package = 'org.mirrorsedge.research'
$remote = "/sdcard/Android/data/$package/files/pc"
$adbArgs = @()
if ($Serial) { $adbArgs = @('-s', $Serial) }

function Invoke-Adb([string[]]$a) {
  $out = & $Adb @adbArgs @a 2>&1
  if ($LASTEXITCODE -ne 0) { throw "adb $($a -join ' '): $out" }
  return $out
}

$state = (& $Adb @adbArgs get-state 2>&1)
if ($LASTEXITCODE -ne 0 -or "$state".Trim() -ne 'device') { throw "No device ready over adb ($state)" }

if ($Apk) {
  if (-not (Test-Path $Apk)) { throw "No APK $Apk" }
  Write-Host "install $Apk"
  Invoke-Adb @('install', '-r', $Apk) | Out-Host
}

# Remote sizes of everything under files/pc: "size path" per line (toybox stat).
function Get-RemoteSizes {
  $map = @{}
  $lines = & $Adb @adbArgs shell "find '$remote' -type f -exec stat -c '%s %n' {} + 2>/dev/null"
  foreach ($l in $lines) {
    $i = "$l".IndexOf(' ')
    if ($i -gt 0) { $map["$l".Substring($i + 1).Trim()] = [int64]"$l".Substring(0, $i) }
  }
  return $map
}

function Push-Tree([string]$local, [string]$dest, [hashtable]$sizes) {
  $base = (Resolve-Path $local).Path
  $files = Get-ChildItem -LiteralPath $base -Recurse -File
  $todo = @($files | Where-Object {
    $rel = $_.FullName.Substring($base.Length).TrimStart('\').Replace('\', '/')
    $sizes["$dest/$rel"] -ne $_.Length
  })
  Write-Host ("{0}: {1} of {2} files to push" -f $dest, $todo.Count, $files.Count)
  if ($todo.Count -eq 0) { return }
  if ($todo.Count -eq $files.Count) {
    # nothing there yet: one push of the whole folder (much faster than file by file)
    Invoke-Adb @('shell', "rm -rf '$dest'") | Out-Null
    Invoke-Adb @('push', $base, $dest.Substring(0, $dest.LastIndexOf('/'))) | Out-Host
    return
  }
  $n = 0
  $made = @{}
  foreach ($f in $todo) {
    $rel = $f.FullName.Substring($base.Length).TrimStart('\').Replace('\', '/')
    $target = "$dest/$rel"
    $dir = $target.Substring(0, $target.LastIndexOf('/'))
    if (-not $made[$dir]) { Invoke-Adb @('shell', "mkdir -p '$dir'") | Out-Null; $made[$dir] = $true }
    Invoke-Adb @('push', $f.FullName, $target) | Out-Null
    $n++
    if ($n % 200 -eq 0) { Write-Host "  $n / $($todo.Count)" }
  }
}

Invoke-Adb @('shell', "mkdir -p '$remote/deps/rebased' '$remote/MirrorsEdge' '$remote/user'") | Out-Null
$sizes = Get-RemoteSizes
foreach ($dll in 'msvcr80.dll', 'msvcp80.dll') {
  $f = Get-Item (Join-Path $Deps $dll)
  if ($sizes["$remote/deps/$dll"] -ne $f.Length) { Invoke-Adb @('push', $f.FullName, "$remote/deps/$dll") | Out-Null }
}
Push-Tree (Join-Path $Deps 'rebased') "$remote/deps/rebased" $sizes
if (-not $SkipGame) {
  foreach ($d in 'Binaries', 'Engine', 'TdGame') { Push-Tree (Join-Path $Game $d) "$remote/MirrorsEdge/$d" $sizes }
}
Write-Host "done: $remote"

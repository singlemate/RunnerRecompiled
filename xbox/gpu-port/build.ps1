param([Parameter(Mandatory=$true)][string]$Workspace)
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $Workspace).Path
$taskPython = Join-Path $taskRoot 'work\venv\Scripts\python.exe'
$taskCmake = Join-Path $taskRoot 'work\venv\Scripts\cmake.exe'
$taskBuild = Join-Path $taskRoot 'work\gpu-port\deps-build'
$taskDependencies = Join-Path $taskRoot 'work\gpu-port\dependencies'
New-Item -ItemType Directory -Force $taskDependencies | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'dependencies.CMakeLists.txt') -Destination (Join-Path $taskDependencies 'CMakeLists.txt')
Push-Location $taskRoot
try {
    & $taskCmake -S $taskDependencies -B $taskBuild -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$taskRoot/work/venv/Scripts/ninja.exe" `
        "-DCMAKE_TOOLCHAIN_FILE=$taskRoot/work/toolchains/android-ndk-r30/build/cmake/android.toolchain.cmake" `
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 -DANDROID_STL=c++_static `
        -DCMAKE_BUILD_TYPE=Release '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' `
        "-DPYTHON_EXECUTABLE=$taskPython" "-DME_REX_SOURCE=$taskRoot/work/references/rexglue-sdk"
    if ($LASTEXITCODE) { throw 'GPU dependency configuration failed' }
    & $taskCmake --build $taskBuild --target SPIRV fmt -j 6
    if ($LASTEXITCODE) { throw 'GPU dependency build failed' }
    foreach ($taskScript in @('compile_gpu_objects.py', 'compile_support.py', 'compile_vulkan_bootstrap.py', 'compile_integration_support.py', 'link_shader_check.py', 'link_bootstrap_check.py', 'link_host_services_check.py', 'audit_integration_link.py', 'link_command_processor_check.py')) {
        & $taskPython (Join-Path $PSScriptRoot $taskScript)
        if ($LASTEXITCODE) { throw "GPU stage failed: $taskScript" }
    }
} finally { Pop-Location }

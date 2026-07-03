<#
.SYNOPSIS
    Windows convenience wrapper around the CMake build - the rough equivalent of
    `make run` on Linux.

.DESCRIPTION
    Configures (once), builds, and launches the Alryn sample game with the Visual
    Studio generator. The Vulkan SDK's VULKAN_SDK environment variable (set by the
    LunarG installer) is used to find glslc and the loader import library.

.PARAMETER Config
    Build configuration: Debug (default) or Release.

.PARAMETER Target
    CMake target to build and run. Default: alryn_game.

.PARAMETER GameArgs
    Anything after `--` is forwarded to the game. Examples:
      .\run.ps1                     # windowed client (runs until you close it)
      .\run.ps1 -- 300              # run 300 frames then exit (smoke test)
      .\run.ps1 -Config Release     # optimised build
      .\run.ps1 -- --server         # headless dedicated server
      .\run.ps1 -- --bot            # headless wandering bot

.EXAMPLE
    .\run.ps1 -- 300
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Debug',
    [string]$Target = 'alryn_game',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$GameArgs
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'build'

if (-not $env:VULKAN_SDK) {
    $machine = [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'Machine')
    if ($machine) { $env:VULKAN_SDK = $machine }
}
if (-not $env:VULKAN_SDK) {
    Write-Error "VULKAN_SDK is not set. Install the LunarG Vulkan SDK (winget install KhronosGroup.VulkanSDK) and open a new shell."
}
$env:PATH = "$env:VULKAN_SDK\Bin;$env:PATH"

# Configure once (CMakeCache.txt is the stamp). Let CMake pick the newest VS
# generator it knows about rather than pinning a year.
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    Write-Host "==> Configuring ($build)" -ForegroundColor Cyan
    cmake -S $root -B $build -A x64
    if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed" }
}

Write-Host "==> Building $Target ($Config)" -ForegroundColor Cyan
cmake --build $build --config $Config --target $Target
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed" }

$exe = Join-Path $build "bin\$Config\$Target.exe"
if (-not (Test-Path $exe)) { Write-Error "Executable not found: $exe" }

Write-Host "==> $exe $GameArgs" -ForegroundColor Green
& $exe @GameArgs
exit $LASTEXITCODE

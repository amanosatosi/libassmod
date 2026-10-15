# Only vcpkg's ABI-addressed binary archives are persisted. Never cache the
# checkout, installed/, downloads/, packages/ or buildtrees/ directories.
param([Parameter(Mandatory)][ValidateSet('key', 'before', 'after')] [string] $Phase)
$ErrorActionPreference = 'Stop'

function Get-TextHash([string] $Text) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
}

function Get-ArchiveInventory {
    # vcpkg names each immutable ZIP by its package ABI. Sizes detect incomplete
    # archives without hashing hundreds of MB on every restore/save.
    $root = $env:VCPKG_DEFAULT_BINARY_CACHE
    $files = @(Get-ChildItem -LiteralPath $root -Recurse -File -Filter *.zip |
        Sort-Object FullName)
    $entries = @($files | ForEach-Object {
        "{0}:{1}" -f [IO.Path]::GetRelativePath($root, $_.FullName), $_.Length
    })
    return @{ fingerprint = Get-TextHash ($entries -join "`n"); count = $files.Count }
}

if ($Phase -eq 'key') {
    # runner.temp is available on the executing runner, not in job-level env
    # expressions. Publish the absolute path for subsequent actions/steps.
    $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $env:RUNNER_TEMP 'vcpkg-binary-cache'
    "VCPKG_DEFAULT_BINARY_CACHE=$env:VCPKG_DEFAULT_BINARY_CACHE" >> $env:GITHUB_ENV
}
New-Item -ItemType Directory -Force $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
$inventoryPath = Join-Path $env:RUNNER_TEMP 'dll-vcpkg-inventory.json'
if ($Phase -eq 'key') {
    $revision = (git -C $env:VCPKG_ROOT rev-parse HEAD).Trim()
    if ($LASTEXITCODE) { throw 'Cannot identify the runner vcpkg revision' }
    $compiler = (Get-Command cl.exe -ErrorAction Stop).Source
    $target = Join-Path $env:VCPKG_ROOT "triplets/$env:VCPKG_DEFAULT_TRIPLET.cmake"
    $hostTriplet = Join-Path $env:VCPKG_ROOT 'triplets/x64-windows.cmake'
    $config = @(
        "vcpkg=$revision"
        "vcpkg-exe=$((Get-FileHash -LiteralPath (Join-Path $env:VCPKG_ROOT 'vcpkg.exe')).Hash)"
        "msvc=$env:VCToolsVersion"
        "compiler=$((Get-FileHash -LiteralPath $compiler).Hash)"
        "sdk=$env:WindowsSDKVersion"
        "target=$env:VCPKG_DEFAULT_TRIPLET"
        "target-file=$((Get-FileHash -LiteralPath $target).Hash)"
        "host-file=$((Get-FileHash -LiteralPath $hostTriplet).Hash)"
        "packages=$env:VCPKG_PACKAGES"
    )
    $family = "dll-vcpkg-v1-$env:RUNNER_OS-$env:RUNNER_ARCH-$env:VCPKG_DEFAULT_TRIPLET"
    $prefix = "$family-$(Get-TextHash ($config -join "`n"))"
    "prefix=$prefix" >> $env:GITHUB_OUTPUT
    "family=$family" >> $env:GITHUB_OUTPUT
    "revision=$revision" >> $env:GITHUB_OUTPUT
    Write-Host "vcpkg revision: $revision"
    Write-Host "MSVC: $env:VCToolsVersion; SDK: $env:WindowsSDKVersion; compiler: $compiler"
    Write-Host "Binary cache: $env:VCPKG_DEFAULT_BINARY_CACHE"
    Write-Host "Configuration key: $prefix"
} elseif ($Phase -eq 'before') {
    $inventory = Get-ArchiveInventory
    $inventory | ConvertTo-Json | Set-Content -LiteralPath $inventoryPath
    Write-Host "Restored archive inventory: $($inventory.count) binary packages"
} else {
    $before = Get-Content -LiteralPath $inventoryPath -Raw | ConvertFrom-Json
    $after = Get-ArchiveInventory
    $changed = $before.fingerprint -ne $after.fingerprint
    "changed=$($changed.ToString().ToLowerInvariant())" >> $env:GITHUB_OUTPUT
    "fingerprint=$($after.fingerprint)" >> $env:GITHUB_OUTPUT
    "count=$($after.count)" >> $env:GITHUB_OUTPUT
    Write-Host "Binary archives after install: $($after.count); inventory changed: $changed"
    if (!$after.count) { throw 'Successful dependency installation produced no binary archives' }
}

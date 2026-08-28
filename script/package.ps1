[CmdletBinding()]
param(
    [string]$AnomalySdkDir = $env:ANOMALY_SDK_DIR,
    [string]$AnomalyPluginTool = $env:ANOMALY_PLUGIN_TOOL,
    [switch]$Incremental,
    [switch]$SkipTests,
    [switch]$SkipValidation
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildDirectory = Join-Path $repositoryRoot 'build'
$distributionDirectory = Join-Path $repositoryRoot 'dist'
$packageDirectory = Join-Path $buildDirectory 'package\NteCombatStats'
$packagingWorkDirectory = Join-Path $buildDirectory 'packaging'

function Resolve-Executable {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [string]$ExplicitPath,
        [string]$FallbackPath
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        $resolved = Resolve-Path -LiteralPath $ExplicitPath -ErrorAction Stop
        return $resolved.Path
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }
    if (-not [string]::IsNullOrWhiteSpace($FallbackPath) -and
        (Test-Path -LiteralPath $FallbackPath -PathType Leaf)) {
        return $FallbackPath
    }
    throw "Cannot find $Name. Add it to PATH or provide its full path."
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [Parameter(Mandatory)] [string[]]$Arguments,
        [string]$WorkingDirectory = $repositoryRoot
    )

    Push-Location -LiteralPath $WorkingDirectory
    try {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
        }
    } finally {
        Pop-Location
    }
}

function Assert-ChildPath {
    param(
        [Parameter(Mandatory)] [string]$Parent,
        [Parameter(Mandatory)] [string]$Child
    )

    $parentPath = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    $childPath = [System.IO.Path]::GetFullPath($Child)
    if (-not $childPath.StartsWith($parentPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to operate outside the build directory: $childPath"
    }
}

function Reset-GeneratedDirectory {
    param([Parameter(Mandatory)] [string]$Path)

    Assert-ChildPath -Parent $buildDirectory -Child $Path
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Reset-BuildDirectory {
    $expected = [System.IO.Path]::GetFullPath(
        (Join-Path $repositoryRoot 'build')).TrimEnd('\')
    $actual = [System.IO.Path]::GetFullPath($buildDirectory).TrimEnd('\')
    if (-not $actual.Equals($expected, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a non-standard build directory: $actual"
    }
    if (Test-Path -LiteralPath $actual) {
        Remove-Item -LiteralPath $actual -Recurse -Force
    }
    New-Item -ItemType Directory -Path $actual -Force | Out-Null
}

$visualStudioCMake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$cmake = Resolve-Executable -Name 'cmake' -FallbackPath $visualStudioCMake
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctest -PathType Leaf)) {
    $ctest = Resolve-Executable -Name 'ctest'
}

if ($Incremental) {
    New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
    Reset-GeneratedDirectory -Path $packageDirectory
    Reset-GeneratedDirectory -Path $packagingWorkDirectory
} else {
    Reset-BuildDirectory
    New-Item -ItemType Directory -Path $packageDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $packagingWorkDirectory -Force | Out-Null
}

$configureArguments = @('--preset', 'default')
if (-not [string]::IsNullOrWhiteSpace($AnomalySdkDir)) {
    $resolvedSdk = (Resolve-Path -LiteralPath $AnomalySdkDir -ErrorAction Stop).Path
    $configureArguments += "-DAnomalySDK_DIR=$resolvedSdk"
}
Invoke-NativeCommand -FilePath $cmake -Arguments $configureArguments
Invoke-NativeCommand -FilePath $cmake -Arguments @(
    '--build', '--preset', 'release'
)
Invoke-NativeCommand -FilePath $cmake -Arguments @(
    '--build', '--preset', 'release', '--target', 'nte_combat_stats_package'
)

if (-not $SkipTests) {
    Invoke-NativeCommand -FilePath $ctest -Arguments @(
        '--preset', 'release', '--output-on-failure'
    )
}

$packageFileList = Join-Path $PSScriptRoot 'package-files.txt'
$expectedFiles = Get-Content -LiteralPath $packageFileList |
    ForEach-Object { $_.Trim() } |
    Where-Object { $_.Length -ne 0 }
$packageRoot = [System.IO.Path]::GetFullPath($packageDirectory).TrimEnd('\') + '\'
$actualFiles = Get-ChildItem -LiteralPath $packageDirectory -File -Recurse |
    ForEach-Object {
        $fullPath = [System.IO.Path]::GetFullPath($_.FullName)
        if (-not $fullPath.StartsWith(
                $packageRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Package file is outside the staging directory: $fullPath"
        }
        $fullPath.Substring($packageRoot.Length).Replace('\', '/')
    } |
    Sort-Object
$expectedSorted = $expectedFiles | Sort-Object
if ([string]::Join("`n", $actualFiles) -ne [string]::Join("`n", $expectedSorted)) {
    throw "Package contents differ from package-files.txt. Actual files:`n$($actualFiles -join "`n")"
}

if (-not $SkipValidation) {
    $validator = Resolve-Executable -Name 'anomaly-plugin' -ExplicitPath $AnomalyPluginTool
    Invoke-NativeCommand -FilePath $validator -Arguments @('validate', $packageDirectory)
}

$manifest = Get-Content -LiteralPath (Join-Path $packageDirectory 'manifest.json') `
    -Raw -Encoding UTF8 |
    ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($manifest.version)) {
    throw 'manifest.json does not contain a valid version.'
}
$archiveName = "NteCombatStats-$($manifest.version)-Release.zip"
$temporaryArchive = Join-Path $packagingWorkDirectory $archiveName
$finalArchive = Join-Path $distributionDirectory $archiveName

$archiveArguments = @(
    '-E', 'tar', 'cf', $temporaryArchive, '--format=zip'
) + $expectedFiles
Invoke-NativeCommand -FilePath $cmake -WorkingDirectory $packageDirectory `
    -Arguments $archiveArguments

New-Item -ItemType Directory -Path $distributionDirectory -Force | Out-Null
Move-Item -LiteralPath $temporaryArchive -Destination $finalArchive -Force

$result = Get-Item -LiteralPath $finalArchive
Write-Host "Package created: $($result.FullName) ($($result.Length) bytes)"

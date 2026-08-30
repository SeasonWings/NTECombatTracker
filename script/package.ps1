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
$packageDirectory = Join-Path $buildDirectory 'package\NteCombatTracker'
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

function Get-RelativeFileList {
    param(
        [Parameter(Mandatory)] [string]$Root,
        [Parameter(Mandatory)] [string]$OutsideRootMessage
    )

    $rootPath = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    return Get-ChildItem -LiteralPath $Root -File -Recurse |
        ForEach-Object {
            $fullPath = [System.IO.Path]::GetFullPath($_.FullName)
            if (-not $fullPath.StartsWith(
                    $rootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "$OutsideRootMessage`: $fullPath"
            }
            $fullPath.Substring($rootPath.Length).Replace('\', '/')
        } |
        Sort-Object
}

function Read-StrictJson {
    param([Parameter(Mandatory)] [string]$Path)
    $strictUtf8 = [System.Text.UTF8Encoding]::new($false, $true)
    try {
        $content = $strictUtf8.GetString([System.IO.File]::ReadAllBytes($Path))
    } catch {
        throw "JSON resource must be valid UTF-8: $Path"
    }
    $options = [System.Text.Json.JsonDocumentOptions]::new()
    $options.MaxDepth = 256
    $jsonDocument = [System.Text.Json.JsonDocument]::Parse($content, $options)
    function Assert-UniqueJsonProperties {
        param(
            [Parameter(Mandatory)] [System.Text.Json.JsonElement]$Element,
            [Parameter(Mandatory)] [string]$JsonPath
        )
        if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
            $seen = [System.Collections.Generic.HashSet[string]]::new(
                [System.StringComparer]::Ordinal)
            foreach ($property in $Element.EnumerateObject()) {
                if (-not $seen.Add($property.Name)) {
                    throw "Duplicate JSON key '$($property.Name)' at '$JsonPath' in '$Path'."
                }
                Assert-UniqueJsonProperties -Element $property.Value `
                    -JsonPath "$JsonPath.$($property.Name)"
            }
        } elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
            $index = 0
            foreach ($item in $Element.EnumerateArray()) {
                Assert-UniqueJsonProperties -Element $item -JsonPath "$JsonPath[$index]"
                ++$index
            }
        }
    }
    try {
        Assert-UniqueJsonProperties -Element $jsonDocument.RootElement -JsonPath '$'
    } finally {
        $jsonDocument.Dispose()
    }
    return $content | ConvertFrom-Json -AsHashtable
}

function Test-NameResources {
    param([Parameter(Mandatory)] [string]$Root)

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Name resource directory is unavailable: $Root"
    }
    $jsonFiles = Get-ChildItem -LiteralPath $Root -Filter '*.json' -File -Recurse
    foreach ($jsonFile in $jsonFiles) {
        $null = Read-StrictJson -Path $jsonFile.FullName
    }

    $manifest = Read-StrictJson -Path (Join-Path $Root 'data\asset_manifest.json')
    if ([string]::IsNullOrWhiteSpace($manifest.assets_git_revision)) {
        throw 'res/data/asset_manifest.json lacks assets_git_revision.'
    }
    $characters = (Read-StrictJson -Path (
        Join-Path $Root 'data\characters\characters.json')).characters
    $characterIds = [System.Collections.Generic.HashSet[uint32]]::new()
    $codenames = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $chineseNames = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    # The upstream resource intentionally shares these display names between
    # distinct playable forms. All other duplicate Chinese names are rejected.
    $allowedSharedChineseNames = [System.Collections.Generic.HashSet[string]]::new(
        [string[]]@('「零」', '安魂曲'), [System.StringComparer]::Ordinal)
    foreach ($entry in $characters.GetEnumerator()) {
        [uint32]$characterId = 0
        if (-not [uint32]::TryParse($entry.Key, [ref]$characterId) -or
            -not $characterIds.Add($characterId) -or
            [string]::IsNullOrWhiteSpace($entry.Value.codename) -or
            -not $codenames.Add([string]$entry.Value.codename) -or
            [string]::IsNullOrWhiteSpace($entry.Value.name_zh) -or
            (-not $chineseNames.Add([string]$entry.Value.name_zh) -and
             -not $allowedSharedChineseNames.Contains([string]$entry.Value.name_zh))) {
            throw "Invalid or duplicate character resource entry: $($entry.Key)"
        }
    }
    $enemies = (Read-StrictJson -Path (
        Join-Path $Root 'data\enemies\enemies.json')).enemies
    $enemyIds = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($enemy in $enemies) {
        if ([string]::IsNullOrWhiteSpace($enemy.config_id) -or
            -not $enemyIds.Add([string]$enemy.config_id)) {
            throw "Invalid or duplicate enemy config_id: $($enemy.config_id)"
        }
    }
    $damage = Read-StrictJson -Path (Join-Path $Root 'data\skills\skill_damage.json')
    $abilityTips = (Read-StrictJson -Path (
        Join-Path $Root 'data\skills\ability_tips.json')).abilities
    $semantics = (Read-StrictJson -Path (
        Join-Path $Root 'data\skills\gameplay_effect_semantics.json')).effects
    if ($null -eq $damage.Rows -or $null -eq $abilityTips -or $null -eq $semantics) {
        throw 'GE, GA, or gameplay effect semantic resources have invalid root types.'
    }
    foreach ($effect in $damage.Rows.GetEnumerator()) {
        if ($effect.Key -notmatch '^[A-Za-z][A-Za-z0-9_]*$' -or
            -not $effect.Value.Contains('GAName') -or
            $effect.Value.GAName -isnot [string] -or
            [string]::IsNullOrWhiteSpace($effect.Value.GAName)) {
            throw "Invalid GE to GA mapping: $($effect.Key)"
        }
    }
    foreach ($ability in $abilityTips.GetEnumerator()) {
        if (-not $ability.Key.StartsWith('GA_', [System.StringComparison]::OrdinalIgnoreCase) -or
            -not $ability.Value.Contains('name_zh') -or
            $ability.Value.name_zh -isnot [string]) {
            throw "Invalid ability tip entry: $($ability.Key)"
        }
    }
    foreach ($effect in $semantics.GetEnumerator()) {
        $semanticAbility = if ($effect.Value.Contains('ability')) {
            [string]$effect.Value.ability
        } else { '' }
        $hasShowParent = $effect.Value.Contains('show_parent_ability')
        if (-not $damage.Rows.Contains($effect.Key) -or
            ([string]::IsNullOrWhiteSpace($semanticAbility) -eq $false -and
             -not $abilityTips.Contains($semanticAbility)) -or
            ($hasShowParent -and
             $effect.Value.show_parent_ability -isnot [bool]) -or
            ($effect.Value.Contains('damage_name_zh') -and
             $effect.Value.damage_name_zh -isnot [string])) {
            throw "Invalid gameplay effect semantic reference: $($effect.Key)"
        }
    }
}

function Test-AttributeColorDictionary {
    param([Parameter(Mandatory)] [string]$Root)

    $path = Join-Path $Root 'dict\attribute_colors.json'
    $colors = Read-StrictJson -Path $path
    $requiredAttributes = @('灵', '相', '暗', '光', '魂', '咒')
    if ($colors.Count -ne $requiredAttributes.Count) {
        throw 'dict/attribute_colors.json must contain exactly six attributes.'
    }
    foreach ($attribute in $requiredAttributes) {
        if (-not $colors.Contains($attribute) -or
            $colors[$attribute] -isnot [string] -or
            $colors[$attribute] -notmatch '^#[0-9A-Fa-f]{6}$') {
            throw "Invalid attribute color for '$attribute' in dict/attribute_colors.json."
        }
    }
}

$visualStudioCMake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$cmake = Resolve-Executable -Name 'cmake' -FallbackPath $visualStudioCMake
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctest -PathType Leaf)) {
    $ctest = Resolve-Executable -Name 'ctest'
}

$resourceRoot = Join-Path $repositoryRoot 'res'
Test-NameResources -Root $resourceRoot
Test-AttributeColorDictionary -Root $repositoryRoot

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
$packageDirectoryList = Join-Path $PSScriptRoot 'package-directories.txt'
$sourceRoot = [System.IO.Path]::GetFullPath($repositoryRoot).TrimEnd('\') + '\'
foreach ($relativeDirectory in (Get-Content -LiteralPath $packageDirectoryList |
        ForEach-Object { $_.Trim() } | Where-Object { $_.Length -ne 0 })) {
    $sourceDirectory = Join-Path $repositoryRoot $relativeDirectory
    $expectedFiles += Get-ChildItem -LiteralPath $sourceDirectory -File -Recurse |
        ForEach-Object {
            $fullPath = [System.IO.Path]::GetFullPath($_.FullName)
            if (-not $fullPath.StartsWith(
                    $sourceRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "Package source is outside the repository: $fullPath"
            }
            $fullPath.Substring($sourceRoot.Length).Replace('\', '/')
        }
}
$actualFiles = Get-RelativeFileList -Root $packageDirectory `
    -OutsideRootMessage 'Package file is outside the staging directory'
$expectedSorted = $expectedFiles | Sort-Object
if ([string]::Join("`n", $actualFiles) -ne [string]::Join("`n", $expectedSorted)) {
    throw "Package contents differ from package-files.txt. Actual files:`n$($actualFiles -join "`n")"
}
Test-NameResources -Root (Join-Path $packageDirectory 'res')
Test-AttributeColorDictionary -Root $packageDirectory

$anomalyPluginTool = Resolve-Executable -Name 'anomaly-plugin' `
    -ExplicitPath $AnomalyPluginTool
if (-not $SkipValidation) {
    Invoke-NativeCommand -FilePath $anomalyPluginTool `
        -Arguments @('validate', $packageDirectory)
}

$manifest = Get-Content -LiteralPath (Join-Path $packageDirectory 'manifest.json') `
    -Raw -Encoding UTF8 |
    ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($manifest.version)) {
    throw 'manifest.json does not contain a valid version.'
}
Invoke-NativeCommand -FilePath $anomalyPluginTool -Arguments @(
    'pack', $packageDirectory, '--output', $packagingWorkDirectory
)
$packedPackageDirectory = Join-Path $packagingWorkDirectory $manifest.id
if (-not (Test-Path -LiteralPath $packedPackageDirectory -PathType Container)) {
    throw "Packed plugin directory is unavailable: $packedPackageDirectory"
}
$actualPackedFiles = Get-RelativeFileList -Root $packedPackageDirectory `
    -OutsideRootMessage 'Packed file is outside the packed plugin directory'
$expectedPackedFiles = @($expectedFiles + 'package.sha256') | Sort-Object
if ([string]::Join("`n", $actualPackedFiles) -ne
    [string]::Join("`n", $expectedPackedFiles)) {
    throw "Packed contents differ from the expected release files. Actual files:`n$($actualPackedFiles -join "`n")"
}
$archiveName = "NteCombatTracker-$($manifest.version)-Release.zip"
$temporaryArchive = Join-Path $packagingWorkDirectory $archiveName
$finalArchive = Join-Path $distributionDirectory $archiveName

$archiveArguments = @(
    '-E', 'tar', 'cf', $temporaryArchive, '--format=zip'
) + $expectedPackedFiles
Invoke-NativeCommand -FilePath $cmake -WorkingDirectory $packedPackageDirectory `
    -Arguments $archiveArguments

New-Item -ItemType Directory -Path $distributionDirectory -Force | Out-Null
Move-Item -LiteralPath $temporaryArchive -Destination $finalArchive -Force

$result = Get-Item -LiteralPath $finalArchive
$archiveHash = (Get-FileHash -LiteralPath $result.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumPath = "$($result.FullName).sha256"
$checksumText = "$archiveHash  $($result.Name)`n"
[System.IO.File]::WriteAllText(
    $checksumPath, $checksumText, [System.Text.UTF8Encoding]::new($false))
Write-Host "Package created: $($result.FullName) ($($result.Length) bytes)"
Write-Host "Checksum created: $checksumPath"

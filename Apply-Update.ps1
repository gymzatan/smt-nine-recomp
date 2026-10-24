param([string]$Target = $PSScriptRoot, [switch]$CheckOnly, [switch]$FunctionsOnly, [string]$Bundle = $PSScriptRoot)
$ErrorActionPreference = 'Stop'

function Get-SHA256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}

function Resolve-SafeFile([string]$Root, [string]$Relative) {
    if ($Relative -match '(^[/\\]|:|(^|[/\\])\.{1,2}([/\\]|$))') { throw "Unsafe update path: $Relative" }
    $absolute = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if (-not $absolute.StartsWith($Root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "Path escapes update root: $Relative" }
    $parent = $absolute
    while ($parent -and $parent.Length -ge $Root.Length) {
        if ((Test-Path -LiteralPath $parent) -and ((Get-Item -LiteralPath $parent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Update path uses a link: $parent" }
        $parent = [IO.Path]::GetDirectoryName($parent)
    }
    return $absolute
}

if ($FunctionsOnly) { return }
$destination = [IO.Path]::GetFullPath($Target).TrimEnd([char[]]'\/')
$bundle = [IO.Path]::GetFullPath($Bundle).TrimEnd([char[]]'\/')
$manifest = Get-Content -LiteralPath (Join-Path $bundle 'UPDATE_MANIFEST.json') -Raw | ConvertFrom-Json
if ($manifest.format -ne 'smt-nine-program-update') { throw 'This is not a supported program update package.' }
if (-not (Test-Path -LiteralPath (Join-Path $destination 'smt9_recomp.exe') -PathType Leaf)) { throw 'Choose an existing game installation, not an empty directory.' }
if ($manifest.language_resources -eq 'JapaneseChinese' -and
    -not (Test-Path -LiteralPath (Resolve-SafeFile $destination 'runtime/native_game.xbe') -PathType Leaf)) {
    throw 'This update requires an existing Chinese installation. Choose the matching update package.'
}
foreach ($name in $manifest.forbidden_files) {
    if (Test-Path -LiteralPath (Resolve-SafeFile $destination $name)) { throw 'This update is for a different installed language selection. Choose the matching update package.' }
}
foreach ($identity in $manifest.required_files) {
    $installed = Resolve-SafeFile $destination $identity.path
    if (-not (Test-Path -LiteralPath $installed -PathType Leaf) -or (Get-SHA256 $installed) -ne $identity.sha256) { throw "Installed game data does not match this package: $($identity.path)" }
}
$copies = @()
$seen = @{}
foreach ($entry in $manifest.files) {
    $name = [string]$entry.path
    if ($name -notmatch '^(smt9_recomp\.exe|Config\.exe|Updater\.exe|LICENSE|THIRD_PARTY\.md|CONTROLS\.zh-CN\.md|backends/[^/]+/[^/]+\.dll|backends/opengl/WINED3D_COPYING\.LIB|backends/vulkan/DXC_LICENSE_(LLVM|MS)|LICENSES/[^/]+|runtime/(native_game(_jp)?\.xbe|title_layers\.bin|fonts/font[234]\.bin))$') { throw "Not a program update file: $name" }
    if ($seen.ContainsKey($name)) { throw "Repeated update path: $name" }
    $seen[$name] = $true
    $source = Resolve-SafeFile $bundle ('payload/' + $name)
    $installed = Resolve-SafeFile $destination $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or (Get-SHA256 $source) -ne $entry.sha256) { throw "Update payload is missing or damaged: $name" }
    $copies += [pscustomobject]@{Source=$source; Destination=$installed; Hash=$entry.sha256}
}
if (-not $seen.ContainsKey('smt9_recomp.exe') -or -not $seen.ContainsKey('Config.exe')) { throw 'The update is missing its programs.' }
$pending = @($copies | Where-Object { -not (Test-Path -LiteralPath $_.Destination -PathType Leaf) -or (Get-SHA256 $_.Destination) -ne $_.Hash })
if ($CheckOnly) {
    [pscustomobject]@{ChangedFiles=$pending.Count; PayloadFiles=$copies.Count; SourceCommit=$manifest.source_commit}
    return
}
foreach ($process in Get-Process -Name smt9_recomp,Config -ErrorAction SilentlyContinue) {
    if (-not $process.Path) { throw 'Cannot check a running game process; close the game and Config first.' }
    if ($process.Path.StartsWith($destination + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Close the game and Config before updating.' }
}
$changed = 0
foreach ($copy in $pending) {
    $directory = [IO.Path]::GetDirectoryName($copy.Destination)
    [void][IO.Directory]::CreateDirectory($directory)
    $temporary = Join-Path $directory ([IO.Path]::GetRandomFileName())
    try {
        Copy-Item -LiteralPath $copy.Source -Destination $temporary
        Move-Item -LiteralPath $temporary -Destination $copy.Destination -Force
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
    $changed++
}
Write-Host "Update complete: $changed changed files. Game data, settings and saves were preserved."

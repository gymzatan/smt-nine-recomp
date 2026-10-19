param(
    [string]$Target,
    [ValidateSet('Gui','Check','Apply')][string]$Operation = 'Gui',
    [string]$WorkDirectory,
    [string]$ResultFile,
    [string]$Installer = (Join-Path $PSScriptRoot 'Apply-Update.ps1'),
    [switch]$LibraryOnly,
    [switch]$Cleanup
)
$ErrorActionPreference = 'Stop'
. $Installer -Target $Target -FunctionsOnly

function Get-InstalledVariant([string]$Root) {
    foreach ($name in @('smt9_recomp.exe','runtime/native_game_jp.xbe')) {
        if (-not (Test-Path -LiteralPath (Resolve-SafeFile $Root $name) -PathType Leaf)) {
            throw 'Place Updater.exe beside smt9_recomp.exe in an existing installation.'
        }
    }
    if (Test-Path -LiteralPath (Resolve-SafeFile $Root 'runtime/native_game.xbe') -PathType Leaf) { return 'japanese-chinese' }
    return 'japanese'
}

function Get-InterfaceLanguage([string]$Root) {
    # Config uses this same Windows profile API and the build's default language.
    if (-not ('NineUpdater.Profile' -as [type])) {
        Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
using System.Text;
namespace NineUpdater {
    public static class Profile {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        public static extern uint GetPrivateProfileString(string section, string key, string initial,
            StringBuilder value, uint size, string path);
    }
}
'@
    }
    $initial = if ((Get-InstalledVariant $Root) -eq 'japanese-chinese') { 'Chinese' } else { 'English' }
    $value = New-Object Text.StringBuilder 16
    $length = [NineUpdater.Profile]::GetPrivateProfileString('Interface', 'Language', $initial, $value, 16,
        (Join-Path $Root 'runtime/NINE.ini'))
    $language = $value.ToString()
    if ($length -ge 15 -or @('Chinese','Japanese','English') -cnotcontains $language) {
        throw '界面语言 / 表示言語 / UI language: Chinese, Japanese, English'
    }
    return $language
}

function Text([string]$English, [string]$Chinese, [string]$Japanese) {
    switch -CaseSensitive ($script:interfaceLanguage) {
        'Chinese' { return $Chinese }
        'Japanese' { return $Japanese }
        'English' { return $English }
        default { throw 'Interface language has not been selected.' }
    }
}

function Get-ReleaseMetadata {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-RestMethod -Uri 'https://api.github.com/repos/gymzatan/smt-nine-recomp/releases/latest' -Headers @{'User-Agent'='SMT-NINE-Updater';'Accept'='application/vnd.github+json'} -TimeoutSec 30
}

function Receive-Asset($Asset, [string]$Destination) {
    $uri = [Uri]$Asset.browser_download_url
    if ($uri.Scheme -ne 'https' -or $uri.Host -ne 'github.com' -or
        -not $uri.AbsolutePath.StartsWith('/gymzatan/smt-nine-recomp/releases/download/', [StringComparison]::Ordinal)) { throw 'Unexpected release download URL.' }
    if ($Asset.digest -notmatch '^sha256:[a-fA-F0-9]{64}$' -or $Asset.size -le 0 -or $Asset.size -gt 1GB) { throw 'The release asset has no valid size or SHA-256 digest.' }
    Invoke-WebRequest -Uri $uri.AbsoluteUri -Headers @{'User-Agent'='SMT-NINE-Updater'} -UseBasicParsing -OutFile $Destination -TimeoutSec 600
    if ((Get-Item -LiteralPath $Destination).Length -ne $Asset.size -or (Get-SHA256 $Destination) -ne $Asset.digest.Substring(7)) { throw 'The downloaded file is incomplete or its checksum differs. No program files were changed.' }
}

function Expand-VerifiedBundle([string]$Archive, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $seen = @{}; [long]$total = 0
        foreach ($entry in $zip.Entries) {
            $path = Resolve-SafeFile $Destination $entry.FullName
            if ($seen.ContainsKey($path)) { throw 'The update archive contains duplicate paths.' }
            $seen[$path] = $true
            $total += $entry.Length
            if ($total -gt 1GB) { throw 'The expanded update is too large.' }
        }
    } finally { $zip.Dispose() }
    [IO.Compression.ZipFile]::ExtractToDirectory($Archive, $Destination)
}

function Get-UpdatePlan([string]$Root, [string]$Work, [string]$ApplyScript) {
    $variant = Get-InstalledVariant $Root
    $release = Get-ReleaseMetadata
    if ($release.draft -or $release.prerelease -or $release.tag_name -notmatch '^v\d+\.\d+\.\d+(?:-[a-z0-9.-]+)?$') { throw 'No supported stable release was returned.' }
    $name = 'SMT-NINE-recomp-' + $release.tag_name + '-update-' + $variant + '.zip'
    $assets = @($release.assets | Where-Object { $_.name -ceq $name })
    $checksums = @($release.assets | Where-Object { $_.name -ceq 'SHA256SUMS.txt' })
    if ($assets.Count -ne 1 -or $checksums.Count -ne 1) { throw 'The latest release does not contain a unique update for this language build.' }
    $archive = Join-Path $Work 'update.zip'; $sums = Join-Path $Work 'SHA256SUMS.txt'
    Receive-Asset $checksums[0] $sums
    $matches = @((Get-Content -LiteralPath $sums) | Where-Object { $_ -cmatch ('^([a-fA-F0-9]{64})\s+\*?' + [Regex]::Escape($name) + '$') })
    if ($matches.Count -ne 1 -or $matches[0].Substring(0,64) -ne $assets[0].digest.Substring(7)) { throw 'The release checksums disagree.' }
    Receive-Asset $assets[0] $archive
    $bundle = Join-Path $Work 'bundle'
    Expand-VerifiedBundle $archive $bundle
    $check = & $ApplyScript -Target $Root -Bundle $bundle -CheckOnly
    $plan = [pscustomobject]@{
        Target=$Root; Tag=$release.tag_name; Variant=$variant; Asset=$name
        Archive=$archive; SHA256=(Get-SHA256 $archive); Bundle=$bundle
        ManifestSHA256=(Get-SHA256 (Join-Path $bundle 'UPDATE_MANIFEST.json'))
        ChangedFiles=$check.ChangedFiles; SourceCommit=$check.SourceCommit
    }
    $plan | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Work 'plan.json') -Encoding UTF8
    return $plan
}

function Install-UpdatePlan([string]$Root, [string]$Work, [string]$ApplyScript) {
    $plan = Get-Content -LiteralPath (Join-Path $Work 'plan.json') -Raw | ConvertFrom-Json
    if ($plan.Target -ne $Root -or (Get-SHA256 $plan.Archive) -ne $plan.SHA256 -or
        (Get-SHA256 (Join-Path $plan.Bundle 'UPDATE_MANIFEST.json')) -ne $plan.ManifestSHA256) { throw 'The prepared update changed. Check for updates again.' }
    & $ApplyScript -Target $Root -Bundle $plan.Bundle
    $check = & $ApplyScript -Target $Root -Bundle $plan.Bundle -CheckOnly
    if ($check.ChangedFiles -ne 0) { throw 'The installed files did not pass verification.' }
    return [pscustomobject]@{Tag=$plan.Tag; ChangedFiles=$plan.ChangedFiles; Verified=$true}
}

if ($LibraryOnly) { return }
if ($Operation -ne 'Gui') {
    try {
        $value = if ($Operation -eq 'Check') { Get-UpdatePlan $Target $WorkDirectory $Installer } else { Install-UpdatePlan $Target $WorkDirectory $Installer }
        [pscustomobject]@{OK=$true; Value=$value} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ResultFile -Encoding UTF8
        exit 0
    } catch {
        [pscustomobject]@{OK=$false; Error=$_.Exception.Message} | ConvertTo-Json | Set-Content -LiteralPath $ResultFile -Encoding UTF8
        exit 1
    }
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$Target = [IO.Path]::GetFullPath($Target).TrimEnd([char[]]'\/')
$script:interfaceLanguage = Get-InterfaceLanguage $Target
$title = Text 'SMT NINE Updater' 'SMT NINE 更新器' 'SMT NINE アップデーター'
$hash = [Security.Cryptography.SHA256]::Create()
try { $lockName = [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($Target.ToLowerInvariant()))).Replace('-','') } finally { $hash.Dispose() }
$created = $false
$mutex = New-Object Threading.Mutex($true, ('Local\SMT-NINE-Updater-' + $lockName), [ref]$created)
if (-not $created) { [void][Windows.Forms.MessageBox]::Show((Text 'The updater is already open for this installation.' '此目录的更新器已经打开。' 'このインストール先のアップデーターは既に開いています。'),$title); $mutex.Dispose(); exit }
$form = New-Object Windows.Forms.Form
$form.Text = $title; $form.ClientSize = New-Object Drawing.Size(620,390)
$form.StartPosition = 'CenterScreen'; $form.FormBorderStyle = 'FixedDialog'; $form.MaximizeBox = $false
$form.Font = New-Object Drawing.Font('Segoe UI',10)
$heading = New-Object Windows.Forms.Label
$heading.Text = Text 'Update this installation' '更新当前安装' 'このインストールを更新'; $heading.Font = New-Object Drawing.Font('Segoe UI',14)
$heading.SetBounds(20,18,580,30); $form.Controls.Add($heading)
$folder = New-Object Windows.Forms.TextBox
$folder.Text=$Target; $folder.ReadOnly=$true; $folder.SetBounds(20,56,580,30); $form.Controls.Add($folder)
$hint = New-Object Windows.Forms.Label
$hint.Text = Text 'Updates Japanese / Chinese programs. Game data, settings and saves stay in place.' '更新日文／中文程序，保留游戏数据、设置和存档。' '日本語／中国語版のプログラムを更新します。ゲームデータ、設定、セーブデータは保持されます。'
$hint.SetBounds(20,96,580,48); $form.Controls.Add($hint)
$status = New-Object Windows.Forms.TextBox
$status.Multiline=$true; $status.ReadOnly=$true; $status.ScrollBars='Vertical'; $status.SetBounds(20,145,580,132); $form.Controls.Add($status)
$progress = New-Object Windows.Forms.ProgressBar
$progress.SetBounds(20,290,580,16); $form.Controls.Add($progress)
$checkButton = New-Object Windows.Forms.Button
$checkButton.Text = Text 'Check again' '重新检查' '再確認'; $checkButton.SetBounds(20,326,145,38); $form.Controls.Add($checkButton)
$applyButton = New-Object Windows.Forms.Button
$applyButton.Text = Text 'Update now' '立即更新' '今すぐ更新'; $applyButton.Enabled=$false; $applyButton.SetBounds(175,326,145,38); $form.Controls.Add($applyButton)
$closeButton = New-Object Windows.Forms.Button
$closeButton.Text = Text 'Close' '关闭' '閉じる'; $closeButton.SetBounds(455,326,145,38); $form.Controls.Add($closeButton)
$script:worker=$null; $script:work=$null; $script:resultPath=$null; $script:activeOperation=''
$scriptPath=$PSCommandPath; $sessionDirectories=New-Object 'Collections.Generic.List[string]'
function Start-Operation([string]$Action) {
    if ($script:worker) { return }
    if ($Action -eq 'Check') {
        $script:work=Join-Path ([IO.Path]::GetTempPath()) ('SMT-NINE-Update-' + [Guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($script:work); $sessionDirectories.Add($script:work)
    }
    $script:resultPath=Join-Path $script:work ($Action+'.json'); $script:activeOperation=$Action
    $status.Text = if ($Action -eq 'Check') { Text 'Checking GitHub and verifying the matching update download...' '正在检查 GitHub，并下载、校验对应的更新包……' 'GitHubを確認し、対応する更新ファイルをダウンロード・検証しています…' } else { Text 'Applying verified files. Please keep this window open...' '正在应用已校验的文件，请保持窗口打开……' '検証済みのファイルを適用しています。このウィンドウを閉じないでください…' }
    $checkButton.Enabled=$false; $applyButton.Enabled=$false; $closeButton.Enabled=$false; $progress.Style='Marquee'
    try {
        $arguments='-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+$scriptPath+'" -Target "'+$Target+'" -Operation '+$Action+' -WorkDirectory "'+$script:work+'" -ResultFile "'+$script:resultPath+'" -Installer "'+$Installer+'"'
        $script:worker=Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru
    } catch {
        $status.Text=$_.Exception.Message; $progress.Style='Blocks'; $checkButton.Enabled=$true; $closeButton.Enabled=$true
    }
}
$timer=New-Object Windows.Forms.Timer; $timer.Interval=200
$timer.Add_Tick({
    if (-not $script:worker -or -not $script:worker.HasExited) { return }
    $script:worker.Dispose(); $script:worker=$null; $progress.Style='Blocks'; $checkButton.Enabled=$true; $closeButton.Enabled=$true
    try {
        $result=Get-Content -LiteralPath $script:resultPath -Raw | ConvertFrom-Json
        if (-not $result.OK) { throw $result.Error }
        $value=$result.Value
        if ($script:activeOperation -eq 'Check' -and $value.ChangedFiles -gt 0) {
            $status.Text=(Text 'Available release: ' '可用发布：' '利用可能なリリース：')+$value.Tag+"`r`n"+(Text 'Files to update: ' '待更新文件：' '更新するファイル数：')+$value.ChangedFiles+"`r`n`r`n"+(Text 'Close the game and Config, then choose Update now.' '请先关闭游戏和 Config，再点击“立即更新”。' 'ゲームとConfigを終了してから「今すぐ更新」を選んでください。')
            $applyButton.Enabled=$true
        } else {
            $status.Text=(Text 'Up to date and verified: ' '已是最新，文件校验通过：' '最新の状態です。検証済み：')+$value.Tag+"`r`n`r`n"+(Text 'Imported English programs and source code still use the source update.cmd command.' '外部导入的英文程序和源码仍使用源码目录的 update.cmd 更新。' 'インポートした英語版とソースコードは、ソースディレクトリのupdate.cmdで更新してください。')
        }
    } catch { $status.Text=(Text 'Update could not finish: ' '未能完成更新：' '更新を完了できませんでした：')+"`r`n"+$_.Exception.Message }
})
$checkButton.Add_Click({Start-Operation 'Check'}); $applyButton.Add_Click({Start-Operation 'Apply'})
$closeButton.Add_Click({$form.Close()})
$form.Add_FormClosing({ if ($script:worker) { $_.Cancel=$true } })
$form.Add_Shown({$timer.Start(); Start-Operation 'Check'})
try { [void]$form.ShowDialog() }
finally {
    $timer.Stop(); $timer.Dispose(); $form.Dispose(); $mutex.ReleaseMutex(); $mutex.Dispose()
    foreach ($directory in $sessionDirectories) {
        $resolved=[IO.Path]::GetFullPath($directory)
        $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([char[]]'\/')
        if ($resolved.StartsWith($tempRoot+'\',[StringComparison]::OrdinalIgnoreCase) -and [IO.Path]::GetFileName($resolved) -match '^SMT-NINE-Update-[a-f0-9]{32}$') { Remove-Item -LiteralPath $resolved -Recurse -Force }
    }
    if ($Cleanup -and [IO.Path]::GetFileName($PSScriptRoot) -match '^SMT-NINE-Updater-\{[a-fA-F0-9-]{36}\}$' -and [IO.Path]::GetDirectoryName($PSScriptRoot).Equals([IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([char[]]'\/'),[StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $PSCommandPath -Force
        Remove-Item -LiteralPath $Installer -Force
        Remove-Item -LiteralPath $PSScriptRoot
    }
}

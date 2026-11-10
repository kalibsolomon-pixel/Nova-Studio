# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [switch]$OwnerApproved,
    [ValidateSet('toolchain', 'qt-prebuilt')][string]$Lock = 'toolchain',
    [string]$Root = '',
    [string]$Downloads = 'C:\Dev\nova-toolchains\downloads'
)
# Phase 4-I (experimental): isolated installation of the pinned MSYS2 UCRT64 toolchain.
#
# THIS INSTALLS SOFTWARE. It runs only with -OwnerApproved, which the owner passes after approving
# the request in docs/phase-4i/MINGW_BUILD_GUIDE.md. Without it the script prints the plan and exits.
#
# What it does, and nothing else:
#   1. Downloads msys2-base-x86_64-20260927.tar.xz from the MSYS2 GitHub release and checks its
#      SHA-256 against the pinned value.
#   2. Unpacks it into its own folder (default C:\Dev\nova-toolchains\msys64-20260927). No
#      installer runs; no Start-menu entry, uninstall entry, service, scheduled task, PATH or other
#      machine/user setting is created or changed. No administrator rights are needed.
#   3. Lets MSYS2 initialise its package keyring (first login shell, minimal PATH).
#   4. Downloads every package of the lock file with its detached signature, checks the SHA-256
#      against the lock and the signature against the MSYS2 keyring, then installs exactly those
#      files with pacman -U. It never runs pacman -Sy/-Syu, so nothing floats to newer versions.
#      A dry run (pacman -U --print) first proves that pacman would install exactly the locked
#      packages and nothing resolved from a repository.
#   5. Records pacman -Q in <root>\nova-toolchain-manifest.txt, compares it with the lock, and stops
#      the keyring's gpg-agent so no background process stays running (also when a step fails).
# MSYS2 runs with HOME unset, so its home folder is <root>\home\<user> and nothing is written to the
# Windows user profile.
# Removing the toolchain later means deleting its folder; nothing else refers to it.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Windows PowerShell's progress bar slows downloads many times over
. (Join-Path $PSScriptRoot 'nova-mingw-toolchain.ps1')
if (-not $Root) { $Root = if ($env:NOVA_MSYS2_ROOT) { $env:NOVA_MSYS2_ROOT } else { $NovaMingw.DefaultRoot } }
$lockFile = Join-Path $PSScriptRoot "..\packaging\mingw\msys2-ucrt64-$Lock-lock.json"
$lockData = Get-Content -LiteralPath $lockFile -Raw | ConvertFrom-Json

Write-Host "Plan: MSYS2 base $($NovaMingw.Msys2BaseFile) -> $Root"
Write-Host "      $($lockData.package_count) packages from $(Split-Path $lockFile -Leaf), download $([math]::Round($lockData.total_download_bytes / 1MB, 1)) MiB, installed $([math]::Round($lockData.total_installed_bytes / 1MB, 1)) MiB"
Write-Host "      downloads kept in $Downloads"
if (-not $OwnerApproved) {
    Write-Host 'Not approved: nothing was downloaded or installed. Re-run with -OwnerApproved after owner approval.'
    exit 2
}
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from a normal (non-elevated) shell: the toolchain needs no administrator rights.'
}
if (Test-Path -LiteralPath $Root) { throw "$Root already exists; refusing to modify an existing toolchain folder." }
$resolvedRoot = [IO.Path]::GetFullPath($Root)
# Never inside a Git checkout (this one or any other): walk up from the target folder.
for ($folder = Split-Path $resolvedRoot -Parent; $folder; $folder = Split-Path $folder -Parent) {
    if (Test-Path -LiteralPath (Join-Path $folder '.git')) { throw "The toolchain must not live inside the Git checkout $folder" }
}
New-Item -ItemType Directory -Force -Path $Downloads | Out-Null

function Get-Verified([string]$Url, [string]$File, [string]$Sha256) {
    $target = Join-Path $Downloads $File
    if (-not (Test-Path -LiteralPath $target)) {
        Write-Host "download $File"
        Invoke-WebRequest -Uri $Url -OutFile "$target.part" -UseBasicParsing
        Move-Item -LiteralPath "$target.part" -Destination $target
    }
    $actual = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256.ToLowerInvariant()) { throw "SHA-256 mismatch for $File`: $actual, expected $Sha256" }
    $target
}

# 1-2. MSYS2 base, unpacked with Windows' own tar (libarchive) into a staging folder, then moved.
$base = Get-Verified $NovaMingw.Msys2BaseUrl $NovaMingw.Msys2BaseFile $NovaMingw.Msys2BaseSha256
$staging = "$resolvedRoot.staging"
if (Test-Path -LiteralPath $staging) { throw "$staging exists; remove it after checking its contents" }
New-Item -ItemType Directory -Path $staging | Out-Null
& "$env:SystemRoot\System32\tar.exe" -xf $base -C $staging
if ($LASTEXITCODE -ne 0) { throw 'tar failed to unpack the MSYS2 base archive' }
Move-Item -LiteralPath (Join-Path $staging 'msys64') -Destination $resolvedRoot
Remove-Item -LiteralPath $staging   # empty folder only

# 3. First login shell: MSYS2's post-install steps create the user files and the pacman keyring.
$bash = Join-Path $resolvedRoot 'usr\bin\bash.exe'
$msysEnv = @{ MSYSTEM = 'UCRT64'; CHERE_INVOKING = '1'; MSYS2_PATH_TYPE = 'minimal'; HOME = $null }
function Invoke-Msys([string]$Command) {
    $saved = @{}
    foreach ($k in $msysEnv.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process'); [Environment]::SetEnvironmentVariable($k, $msysEnv[$k], 'Process') }
    # Windows PowerShell 5.1 turns any stderr line of a native command into a terminating error under
    # ErrorActionPreference=Stop; MSYS2 and gpg report progress on stderr, so only the exit code counts.
    $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        & $bash -lc $Command 2>&1 | ForEach-Object { "$_" }
        if ($LASTEXITCODE -ne 0) { throw "MSYS2 command failed ($LASTEXITCODE): $Command" }
    }
    finally {
        $ErrorActionPreference = $previous
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
    }
}
try {
Invoke-Msys 'true'
Invoke-Msys 'pacman-key --list-keys >/dev/null'

# 4. Packages: SHA-256 from the lock, signature from the MSYS2 keyring, then pacman -U.
# pacman-key --verify refuses non-root users unless the keyring is configured lock-never, so the
# signature is checked with gpg against the same pacman keyring: a VALIDSIG from a key that the
# MSYS2 master keys make fully trusted, and no bad, expired or revoked signature.
$files = @(); $signatures = [ordered]@{}
foreach ($p in $lockData.packages) {
    $pkg = Get-Verified $p.url $p.filename $p.sha256
    $sig = Join-Path $Downloads "$($p.filename).sig"
    if (-not (Test-Path -LiteralPath $sig)) { Invoke-WebRequest -Uri "$($p.url).sig" -OutFile $sig -UseBasicParsing }
    $unix = (& (Join-Path $resolvedRoot 'usr\bin\cygpath.exe') -u $pkg)
    $status = @(Invoke-Msys "gpg --homedir /etc/pacman.d/gnupg --batch --lock-never --no-auto-key-retrieve --status-fd 1 --verify '$unix.sig' '$unix' 2>/dev/null")
    $valid = @($status | Where-Object { $_ -match '^\[GNUPG:\] VALIDSIG (\S+)' })
    $trusted = @($status | Where-Object { $_ -match '^\[GNUPG:\] TRUST_(FULLY|ULTIMATE)' })
    $bad = @($status | Where-Object { $_ -match '^\[GNUPG:\] (BADSIG|ERRSIG|EXPSIG|EXPKEYSIG|REVKEYSIG|NO_PUBKEY)' })
    if ($valid.Count -ne 1 -or -not $trusted -or $bad) { throw "Signature check failed for $($p.filename): $($status -join ' | ')" }
    $signatures[$p.filename] = ($valid[0] -split ' ')[2]
    $files += $unix
}
$signatures | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Downloads "nova-install-$Lock-signatures.json") -Encoding utf8
$list = Join-Path $Downloads "nova-install-$Lock.txt"
# LF line ends: bash's $(cat ...) would keep a CR as part of every file name.
[IO.File]::WriteAllText($list, (($files -join "`n") + "`n"), [Text.Encoding]::ASCII)
$listUnix = (& (Join-Path $resolvedRoot 'usr\bin\cygpath.exe') -u $list)
# Dry run: the set pacman would install must be exactly the lock (no dependency from a repository).
$plan = Join-Path $Downloads "nova-install-$Lock-plan.txt"
Invoke-Msys "pacman -U --print --print-format '%n %v' `$(cat '$listUnix') > '$((& (Join-Path $resolvedRoot 'usr\bin\cygpath.exe') -u $plan))'"
$planned = @(Get-Content -LiteralPath $plan | Where-Object { $_ -match '^\S+ \S+$' } | Sort-Object)
$expected = @($lockData.packages | ForEach-Object { "$($_.name) $($_.version)" } | Sort-Object)
$diff = @(Compare-Object $expected $planned | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" })
if ($diff) { throw "pacman would not install exactly the lock: $($diff -join '; ')" }
Invoke-Msys "pacman -U --noconfirm --needed `$(cat '$listUnix')"

# 5. Manifest and comparison with the lock; stop the keyring agent.
$manifest = Join-Path $resolvedRoot 'nova-toolchain-manifest.txt'
Invoke-Msys "pacman -Q > '$((& (Join-Path $resolvedRoot 'usr\bin\cygpath.exe') -u $manifest))'"
$installed = @{}
Get-Content -LiteralPath $manifest | ForEach-Object { $n, $v = $_ -split ' ', 2; $installed[$n] = $v }
$wrong = @($lockData.packages | Where-Object { $installed[$_.name] -ne $_.version } | ForEach-Object { "$($_.name): $($installed[$_.name]) (lock $($_.version))" })
}
finally {
    # Stop the keyring's gpg-agent even when a step failed, so nothing keeps running.
    $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { Invoke-Msys 'gpgconf --kill all' } catch { Write-Warning "gpgconf --kill all: $($_.Exception.Message)" }
    $ErrorActionPreference = $previous
}
if ($wrong) { throw "Installed versions differ from the lock: $($wrong -join '; ')" }
Write-Host "Installed $($lockData.package_count) locked packages into $resolvedRoot; manifest: $manifest"
Write-Host 'Next: scripts/build-qtbase-mingw.ps1 -OwnerApproved, then scripts/check-mingw-toolchain.ps1'

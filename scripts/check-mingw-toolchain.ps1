# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [ValidateSet('source', 'msys2')][string]$QtVariant = 'source',
    [string]$McpSdkPython = '',
    [string]$Report = ''
)
# Phase 4-I (experimental): read-only preflight for the GCC/MinGW-w64 build. Runs only version
# queries of tools inside the toolchain folder; installs, downloads and changes nothing.
# Exit code 0 when the toolchain is complete, 1 otherwise (with the installation request).
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path "$PSScriptRoot\..").Path
. (Join-Path $PSScriptRoot 'nova-toolchain.ps1')        # MCP SDK detection only
. (Join-Path $PSScriptRoot 'nova-mingw-toolchain.ps1')
$paths = Get-NovaMingwPaths $QtVariant
$result = Test-NovaMingwToolchain $paths
Write-Host "MSYS2 root : $($paths.Root)"
Write-Host "Qt ($QtVariant): $($paths.Qt)"
foreach ($k in $result.Found.Keys) { Write-Host ("  {0,-22} {1}" -f $k, $result.Found[$k]) }

$sdk = $null
try { $sdk = Resolve-NovaMcpSdkPython $workspace $McpSdkPython } catch { $result.Problems += "MCP SDK: $($_.Exception.Message)" }
if ($sdk) { Write-Host "  MCP SDK                $($sdk.Version) ($($sdk.Path))" }
else { $result.Problems += 'MCP SDK interpreter not found (set NOVA_MCP_SDK_PYTHON); needed only for the mcp suite' }

# Visual Studio tools must not be needed: report, never use.
$vs = @($env:PATH -split ';' | Where-Object { $_ -match '(?i)Microsoft Visual Studio|\\BuildTools\\' })
if ($vs) { Write-Host "  note: this shell's PATH contains Visual Studio folders; the MinGW scripts remove them from their own process" }

if ($Report) {
    [ordered]@{ format = 'nova-mingw-preflight'; qt_variant = $QtVariant; paths = $paths; found = $result.Found
                mcp_sdk = $sdk; problems = $result.Problems } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $Report -Encoding utf8
}
if ($result.Problems) {
    Write-Host ''
    Write-Host 'NOT READY:' -ForegroundColor Yellow
    $result.Problems | ForEach-Object { Write-Host "  - $_" }
    if (-not (Test-Path -LiteralPath $paths.Root)) {
        Write-Host ''
        Write-Host 'Installation request (needs separate owner approval; nothing was installed):'
        Write-Host "  1. MSYS2 base $($NovaMingw.Msys2BaseFile) (SHA-256 $($NovaMingw.Msys2BaseSha256)), unpacked to $($NovaMingw.DefaultRoot)"
        Write-Host '  2. The 51 pinned UCRT64 packages of packaging/mingw/msys2-ucrt64-toolchain-lock.json (GCC 16.2.0, binutils 2.47, CMake 4.4.5, Ninja 1.13.2)'
        Write-Host "  3. Qt Base 6.12.0 built from the retained source archive into $($NovaMingw.DefaultQt) (scripts/build-qtbase-mingw.ps1)"
        Write-Host '  Procedure: scripts/install-mingw-toolchain.ps1 -OwnerApproved; details in docs/phase-4i/MINGW_BUILD_GUIDE.md'
    }
    exit 1
}
Write-Host 'OK: GCC/MinGW-w64 toolchain and Qt are complete.'
exit 0

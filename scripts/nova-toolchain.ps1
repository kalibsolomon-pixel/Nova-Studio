# SPDX-License-Identifier: GPL-3.0-or-later
# Shared toolchain selection, dot-sourced by build-studio.ps1, run-ctest.ps1 and check-toolchain.ps1.
#
# Nova Studio is built with the standalone Visual Studio Build Tools, selected explicitly: a newer
# Visual Studio Community/Professional/Enterprise instance on the same machine is never picked up
# by accident. Everything the build uses is checked before it runs:
#   Build Tools instance  vswhere -products Microsoft.VisualStudio.Product.BuildTools
#                         (or NOVA_VS_INSTALL_PATH for a deliberate override)
#   MSVC toolset          14.51 (cl 19.51), selected with vcvars64 -vcvars_ver
#   Windows SDK           10.0.26100.0, selected with vcvars64 <sdk>
#   CMake / Ninja         the copies bundled with that Build Tools instance
#   Qt                    6.12.0 (Nova uses a private Qt API, so the version must match)
#   MCP SDK Python        an interpreter with the official MCP Python SDK, for the mcp suite
# Overrides (environment): NOVA_VS_INSTALL_PATH, NOVA_MSVC_TOOLSET, NOVA_WINDOWS_SDK,
# NOVA_MCP_SDK_PYTHON. Nothing here installs, updates or repairs anything.

$NovaToolchain = [ordered]@{
    Product       = 'Microsoft.VisualStudio.Product.BuildTools'
    VcComponent   = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    MsvcToolset   = '14.51'
    ClVersion     = '19.51.'
    WindowsSdk    = '10.0.26100.0'
    QtVersion     = '6.12.0'
    McpSdkVersion = '1.26.0'
}
if ($env:NOVA_MSVC_TOOLSET) { $NovaToolchain.MsvcToolset = $env:NOVA_MSVC_TOOLSET }
if ($env:NOVA_WINDOWS_SDK) { $NovaToolchain.WindowsSdk = $env:NOVA_WINDOWS_SDK }

function Find-NovaBuildTools {
    # Returns the installation folder of the Build Tools instance to use, or throws.
    if ($env:NOVA_VS_INSTALL_PATH) {
        $path = $env:NOVA_VS_INSTALL_PATH
        if (-not (Test-Path -LiteralPath (Join-Path $path 'VC\Auxiliary\Build\vcvars64.bat'))) {
            throw "NOVA_VS_INSTALL_PATH does not contain VC\Auxiliary\Build\vcvars64.bat: $path"
        }
        return (Resolve-Path -LiteralPath $path).Path
    }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "vswhere.exe not found ($vswhere). Visual Studio Build Tools 2026 with the MSVC x64 tools is required; see docs/release/BUILDING.md."
    }
    $found = @(& $vswhere -products $NovaToolchain.Product -requires $NovaToolchain.VcComponent -property installationPath)
    $found = @($found | Where-Object { $_ })
    if ($found.Count -eq 0) {
        throw ("No Visual Studio Build Tools instance with the MSVC x64 tools was found. Nova Studio selects " +
               "the standalone Build Tools only; set NOVA_VS_INSTALL_PATH to use another instance deliberately.")
    }
    if ($found.Count -gt 1) {
        throw "Several Build Tools instances were found ($($found -join '; ')). Set NOVA_VS_INSTALL_PATH to choose one."
    }
    return $found[0]
}

function Get-NovaVsVersion([string]$VsPath) {
    # installationVersion of the instance at $VsPath, from vswhere (empty if unknown).
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) { return '' }
    $target = $VsPath.TrimEnd('\')
    $match = @(& $vswhere -all -prerelease -products * -format json | ConvertFrom-Json |
               Where-Object { $_.installationPath.TrimEnd('\') -eq $target })
    if ($match.Count -eq 1) { return "$($match[0].installationVersion)" }
    ''
}

function Get-NovaToolchainPaths([string]$VsPath) {
    # Paths of the tools inside one Build Tools instance, without running any of them.
    $toolsRoot = Join-Path $VsPath 'VC\Tools\MSVC'
    $toolset = @(Get-ChildItem -LiteralPath $toolsRoot -Directory -ErrorAction SilentlyContinue |
                 Where-Object { $_.Name.StartsWith($NovaToolchain.MsvcToolset + '.') } | Sort-Object Name)
    $redistRoot = Join-Path $VsPath 'VC\Redist\MSVC'
    $redist = @(Get-ChildItem -LiteralPath $redistRoot -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name.StartsWith($NovaToolchain.MsvcToolset + '.') } | Sort-Object Name)
    $sdkBin = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin\$($NovaToolchain.WindowsSdk)\x64"
    [ordered]@{
        VsPath     = $VsPath
        Toolsets   = @($toolset | ForEach-Object { $_.FullName })
        Cl         = if ($toolset.Count -eq 1) { Join-Path $toolset[0].FullName 'bin\Hostx64\x64\cl.exe' } else { $null }
        RedistDirs = @($redist | ForEach-Object { $_.FullName })
        Crt        = if ($redist.Count -eq 1) { Join-Path $redist[0].FullName 'x64\Microsoft.VC145.CRT' } else { $null }
        SdkBin     = $sdkBin
        Vcvars     = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
        CMake      = Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        CTest      = Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
        Ninja      = Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
        RedistTxt  = Join-Path $VsPath 'Licenses\1033\Redist.txt'
    }
}

function Test-NovaToolchainFiles($Paths) {
    # Static checks (file presence and version resources only). Returns a list of problems.
    $problems = @()
    if ($Paths.Toolsets.Count -ne 1) { $problems += "expected one MSVC $($NovaToolchain.MsvcToolset).* toolset, found $($Paths.Toolsets.Count)" }
    elseif (-not (Test-Path -LiteralPath $Paths.Cl)) { $problems += "cl.exe missing: $($Paths.Cl)" }
    else {
        $clVersion = (Get-Item -LiteralPath $Paths.Cl).VersionInfo.FileVersion
        if (-not $clVersion.StartsWith($NovaToolchain.ClVersion)) { $problems += "cl.exe is $clVersion, expected $($NovaToolchain.ClVersion)*" }
    }
    if ($Paths.RedistDirs.Count -ne 1) { $problems += "expected one MSVC $($NovaToolchain.MsvcToolset).* redistributable folder, found $($Paths.RedistDirs.Count)" }
    elseif (-not (Test-Path -LiteralPath $Paths.Crt)) { $problems += "MSVC runtime folder missing: $($Paths.Crt)" }
    foreach ($key in 'Vcvars', 'CMake', 'CTest', 'Ninja') {
        if (-not (Test-Path -LiteralPath $Paths[$key])) { $problems += "$key missing: $($Paths[$key])" }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $Paths.SdkBin 'rc.exe'))) { $problems += "Windows SDK $($NovaToolchain.WindowsSdk) not found: $($Paths.SdkBin)" }
    $problems
}

function Enter-NovaMsvcEnvironment($Paths) {
    # Loads vcvars64 for the pinned toolset and SDK into this process, then verifies the result.
    $problems = @(Test-NovaToolchainFiles $Paths)
    if ($problems) { throw ('Toolchain check failed: ' + ($problems -join '; ')) }
    $arguments = "$($NovaToolchain.WindowsSdk) -vcvars_ver=$($NovaToolchain.MsvcToolset)"
    $lines = cmd /c "`"$($Paths.Vcvars)`" $arguments >nul && set"
    if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat $arguments failed" }
    $lines | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
    }
    if (-not "$env:VCToolsVersion".StartsWith($NovaToolchain.MsvcToolset + '.')) { throw "vcvars selected MSVC '$env:VCToolsVersion', expected $($NovaToolchain.MsvcToolset).*" }
    if ("$env:WindowsSDKVersion".TrimEnd('\') -ne $NovaToolchain.WindowsSdk) { throw "vcvars selected Windows SDK '$env:WindowsSDKVersion', expected $($NovaToolchain.WindowsSdk)" }
    if (-not (Test-Path -LiteralPath (Join-Path "$env:VCToolsRedistDir" 'x64\Microsoft.VC145.CRT'))) { throw "VCToolsRedistDir has no x64\Microsoft.VC145.CRT: '$env:VCToolsRedistDir'" }
    $active = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
    if (-not $active -or ((Resolve-Path -LiteralPath $active).Path -ne (Resolve-Path -LiteralPath $Paths.Cl).Path)) {
        throw "cl.exe on PATH is '$active', expected $($Paths.Cl)"
    }
}

function Test-NovaQt([string]$QtRoot) {
    # Returns a list of problems with the Qt kit (version file and the DLLs Nova links).
    $problems = @()
    $config = Join-Path $QtRoot 'lib\cmake\Qt6\Qt6ConfigVersionImpl.cmake'
    if (-not (Test-Path -LiteralPath $config)) { $config = Join-Path $QtRoot 'lib\cmake\Qt6\Qt6ConfigVersion.cmake' }
    if (-not (Test-Path -LiteralPath $config)) { return @("Qt not found at $QtRoot (no lib\cmake\Qt6)") }
    $text = Get-Content -LiteralPath $config -Raw
    if ($text -notmatch [regex]::Escape("PACKAGE_VERSION `"$($NovaToolchain.QtVersion)`"")) { $problems += "Qt at $QtRoot is not version $($NovaToolchain.QtVersion)" }
    foreach ($dll in 'Qt6Core', 'Qt6Gui', 'Qt6Widgets', 'Qt6OpenGL', 'Qt6OpenGLWidgets', 'Qt6Network') {
        if (-not (Test-Path -LiteralPath (Join-Path $QtRoot "bin\$dll.dll"))) { $problems += "$dll.dll missing in $QtRoot\bin" }
    }
    $problems
}

function Test-NovaMcpSdkPython([string]$Python) {
    # Validates one interpreter. Returns @{ Ok; Version; Detail }.
    if (-not $Python -or -not (Test-Path -LiteralPath $Python -PathType Leaf)) { return @{ Ok = $false; Detail = 'not found' } }
    $probe = "import importlib.metadata as m, json; import mcp, mcp.client.stdio; " +
             "md = m.metadata('mcp'); urls = ' '.join(md.get_all('Project-URL') or []) + ' ' + (md.get('Home-page') or ''); " +
             "print(json.dumps(dict(version=m.version('mcp'), official='modelcontextprotocol/python-sdk' in urls)))"
    # Windows PowerShell turns native stderr into error records; keep them as plain text here.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $output = @(& $Python -I -c $probe 2>&1 | ForEach-Object { "$_" }) } finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0) { return @{ Ok = $false; Detail = "cannot import the MCP SDK: $(($output | Select-Object -Last 1))" } }
    try { $info = ($output | Select-Object -Last 1) | ConvertFrom-Json } catch { return @{ Ok = $false; Detail = "unexpected output: $output" } }
    if (-not $info.official) { return @{ Ok = $false; Version = $info.version; Detail = "package 'mcp' $($info.version) is not the official modelcontextprotocol/python-sdk distribution" } }
    @{ Ok = $true; Version = $info.version; Detail = '' }
}

function Resolve-NovaMcpSdkPython([string]$Workspace, [string]$Explicit = '') {
    # Order: explicit argument, NOVA_MCP_SDK_PYTHON, then the workspace's own virtual environments.
    # An explicitly configured interpreter that fails validation is an error, never skipped over.
    $candidates = @()
    if ($Explicit) { $candidates += @{ Path = $Explicit; Source = '-McpSdkPython'; Explicit = $true } }
    elseif ($env:NOVA_MCP_SDK_PYTHON) { $candidates += @{ Path = $env:NOVA_MCP_SDK_PYTHON; Source = 'NOVA_MCP_SDK_PYTHON'; Explicit = $true } }
    else {
        foreach ($relative in 'build\phase4c-venv\Scripts\python.exe', '.venv\Scripts\python.exe', 'build\mcp-sdk-venv\Scripts\python.exe') {
            $candidates += @{ Path = (Join-Path $Workspace $relative); Source = "workspace $relative"; Explicit = $false }
        }
    }
    $tried = @()
    foreach ($candidate in $candidates) {
        $result = Test-NovaMcpSdkPython $candidate.Path
        if ($result.Ok) {
            if ($result.Version -ne $NovaToolchain.McpSdkVersion) {
                Write-Warning "MCP SDK $($result.Version) at $($candidate.Path); the accepted baseline is $($NovaToolchain.McpSdkVersion)."
            }
            return [ordered]@{ Path = (Resolve-Path -LiteralPath $candidate.Path).Path; Version = $result.Version; Source = $candidate.Source }
        }
        if ($candidate.Explicit) { throw "The MCP SDK interpreter from $($candidate.Source) is not usable ($($candidate.Path)): $($result.Detail)" }
        $tried += "$($candidate.Source): $($result.Detail)"
    }
    $script:NovaMcpSdkTried = $tried
    $null
}

function Get-NovaMcpSdkHelp {
    $tried = if ($script:NovaMcpSdkTried) { ' Tried: ' + ($script:NovaMcpSdkTried -join '; ') + '.' } else { '' }
    "The mcp test suite needs a Python interpreter with the official MCP Python SDK $($NovaToolchain.McpSdkVersion).$tried " +
    "Set NOVA_MCP_SDK_PYTHON (or pass -McpSdkPython) to its python.exe, or create build\phase4c-venv " +
    "(docs/release/BUILDING.md, 'Test prerequisites'). To run everything else, pass -SkipMcp; the run is then not a full regression."
}

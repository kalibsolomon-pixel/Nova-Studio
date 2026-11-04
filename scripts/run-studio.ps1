param([string]$Project = '', [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
      [string]$BuildName = "studio-$($Configuration.ToLower())")
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path "$PSScriptRoot\..").Path
$env:PATH = (Join-Path $workspace '.tools\qt\bin') + ';' + $env:PATH
if ($BuildName -notmatch '^[a-zA-Z0-9_-]+$') { throw 'BuildName must be a simple directory name.' }
$exe = Join-Path $workspace "build\$BuildName\src\nova-studio.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Build Nova Studio before launching this build.' }
if ($Project) { & $exe --project $Project } else { & $exe }

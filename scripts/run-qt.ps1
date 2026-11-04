param([string]$Project = 'fixtures/generated/pack-1500')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot\..").Path
$env:PATH=(Join-Path $workspace '.tools\qt\bin')+';'+$env:PATH
Push-Location $workspace
try { & "$workspace\build\qt-review\nova-qt.exe" --project $Project } finally { Pop-Location }

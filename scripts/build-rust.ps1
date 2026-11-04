$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot\..").Path
New-Item -ItemType Directory -Path "$workspace\build" -Force | Out-Null
Push-Location $workspace
try {
    cargo rustc --locked --release -p nova-egui --bin nova-egui -- -o "$workspace\build\nova-egui.exe"
    if($LASTEXITCODE -ne 0){throw 'Rust build failed'}
} finally { Pop-Location }

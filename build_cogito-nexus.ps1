param ($Module = "check")

$dist = "$PSScriptRoot\dist"
$bridge = "$PSScriptRoot\nao_bridge"
$brain = "$PSScriptRoot\main_app"

function Build-Bridge {
    Write-Host "> Buduje Most (nao_bridge)..." -F Yellow
    $oldPath = $env:PATH
    $env:PATH = "C:\Program Files\CMake-3.2.3\bin;" + $env:PATH
    
    Push-Location $bridge
    if (Test-Path "build-nao-bridge-config") { rm -Recurse -Force "build-nao-bridge-config" }
    
    $vs = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
    cmd /c "`"$vs`" -arch=x64 && qibuild configure -c nao-bridge-config --release && qibuild make -c nao-bridge-config"
    
    # Tworzenie specyficznego podfolderu w dist
    $target = "$dist\nao_bridge"
    if (!(Test-Path $target)) { mkdir $target | Out-Null }
    
    cp "build-nao-bridge-config/sdk/bin/*" $target -Force
    Pop-Location
    $env:PATH = $oldPath
}

function Build-Brain {
    Write-Host "> Buduje Mózg (main_app)..." -F Yellow
    if (Test-Path "$brain\build") { rm -Recurse -Force "$brain\build" }
    
    cmake -S $brain -B "$brain\build" -G "Visual Studio 17 2022" -A x64
    cmake --build "$brain\build" --config Release
    
    if (!(Test-Path $dist)) { mkdir $dist | Out-Null }
    cp "$brain\build\Release\brain.exe" $dist -Force
}

# --- Logika wyboru ---
if ($Module -eq "bridge") { Build-Bridge }
elseif ($Module -eq "brain") { Build-Brain }
elseif ($Module -eq "all") { Build-Bridge; Build-Brain }
else { 
    # Sprawdzanie czy pliki istnieją w odpowiednich miejscach
    if (!(Test-Path "$dist\nao_bridge\nao_bridge.dll")) { Build-Bridge }
    if (!(Test-Path "$dist\brain.exe")) { Build-Brain }
}

Write-Host "--- Gotowe ---" -F Cyan
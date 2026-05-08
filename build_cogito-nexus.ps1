param ($Module = "check")

#$OutputEncoding = [System.Text.Encoding]::UTF8
#[console]::InputEncoding = [System.Text.Encoding]::UTF8
#[console]::OutputEncoding = [System.Text.Encoding]::UTF8

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
    
    # Kopiowanie wygenerowanych plików .dll do podfolderu dist\whisper
    $whisperDist = "$dist\whisper"
    if (!(Test-Path $whisperDist)) { mkdir $whisperDist | Out-Null }
    Get-ChildItem -Path "$brain\build" -Recurse -Filter "*.dll" | Copy-Item -Destination $whisperDist -Force
    
    # Automatyczne generowanie skryptu uruchomieniowego CogitoNexus.bat z pauzą na końcu
    $batPath = "$dist\CogitoNexus.bat"
    $batContent = "@echo off`r`nchcp 65001`r`nset PATH=%~dp0whisper;`%PATH%`r`nbrain.exe`r`npause"
    Set-Content -Path $batPath -Value $batContent
}

# --- Funkcja sprawdzająca zmiany w kodzie ---
function Test-NeedsBuild {
    param (
        [string]$SourceDir,
        [string]$TargetFile
    )
    # Jeśli plik docelowy nie istnieje, wymuszamy budowanie
    if (!(Test-Path $TargetFile)) { return $true }
    
    $targetDate = (Get-Item $TargetFile).LastWriteTime
    
    # Szukamy najnowszego pliku źródłowego (pomijając foldery kompilacji)
    $latestSource = Get-ChildItem -Path $SourceDir -Recurse -File -Include *.cpp, *.hpp, *.h, *.c, CMakeLists.txt | 
                    Where-Object { $_.FullName -notmatch '\\build|\\dist' } |
                    Sort-Object LastWriteTime -Descending | 
                    Select-Object -First 1
                    
    if ($null -ne $latestSource -and $latestSource.LastWriteTime -gt $targetDate) {
        Write-Host "-> Wykryto modyfikację w pliku: $($latestSource.Name)" -ForegroundColor Magenta
        return $true
    }
    return $false
}

# --- Logika wyboru ---
if ($Module -eq "bridge") { Build-Bridge }
elseif ($Module -eq "brain") { Build-Brain }
elseif ($Module -eq "all") { Build-Bridge; Build-Brain }
else { 
    if (Test-NeedsBuild -SourceDir $bridge -TargetFile "$dist\nao_bridge\nao_bridge.dll") { Build-Bridge }
    else { Write-Host "> Most (nao_bridge) jest aktualny." -F Green }

    if (Test-NeedsBuild -SourceDir $brain -TargetFile "$dist\brain.exe") { Build-Brain }
    else { Write-Host "> Mózg (main_app) jest aktualny." -F Green }
}

Write-Host "--- Gotowe ---" -F Cyan
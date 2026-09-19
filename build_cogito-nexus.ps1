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

    # VCTargetsPath trzeba wskazać ręcznie na v140 -- inaczej MSBuild nie
    # potrafi znaleźć targetów C++ dla starego toolsetu, którego wymaga
    # SDK NAOqi. Ustawiamy to TYLKO na czas budowania mostu i przywracamy
    # zaraz potem, żeby nie zepsuć budowania main_app (który wymaga własnej,
    # dynamicznie wykrywanej ścieżki v143 z VS2022).
    $oldVCTargetsPath = $env:VCTargetsPath
    $env:VCTargetsPath = "C:\Program Files (x86)\MSBuild\Microsoft.Cpp\v4.0\V140\"

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
    $env:VCTargetsPath = $oldVCTargetsPath
}

function Build-Brain {
    Write-Host "> Buduje Mózg (main_app)..." -F Yellow
    
    # 1. Szybka kompilacja inkrementalna (zostawiamy folder build w spokoju)
    if (!(Test-Path "$brain\build")) {
        Write-Host "  -> Generowanie nowych plików CMake..." -F DarkGray
        cmake -S $brain -B "$brain\build" -G "Visual Studio 17 2022" -A x64
    } else {
        # Jeśli folder istnieje, tylko odświeżamy konfigurację
        cmake -S $brain -B "$brain\build"
    }
    
    # 2. Wielowątkowa kompilacja (wykorzysta 100% procesora)
    Write-Host "  -> Kompilacja..." -F DarkGray
    cmake --build "$brain\build" --config Release --parallel
    
    # Kopiowanie pliku wykonywalnego
    if (!(Test-Path $dist)) { mkdir $dist | Out-Null }
    cp "$brain\build\Release\brain.exe" $dist -Force
    
    # -----------------------------------------------------------------
    # 3. SEPARACJA BIBLIOTEK (Whisper w 'whisper', Llama w 'LLM')
    # -----------------------------------------------------------------
    Write-Host "  -> Kopiowanie bibliotek dynamicznych (DLL)..." -F DarkGray

    # -- WHISPER --
    $whisperDist = "$dist\whisper"
    if (!(Test-Path $whisperDist)) { mkdir $whisperDist | Out-Null }
    # Kopiujemy wszystkie DLL-ki zawierające "whisper" w nazwie
    Get-ChildItem -Path "$brain\build" -Recurse -Filter "*whisper*.dll" | Copy-Item -Destination $whisperDist -Force
    
    # -- LLM (Llama + silnik matematyczny GGML + CUDA) --
    $llmDist = "$dist\LLM"
    if (!(Test-Path $llmDist)) { mkdir $llmDist | Out-Null }
    # Kopiujemy pliki Llama oraz wszystkie odmiany silnika ggml (cuda, base, cpu)
    Get-ChildItem -Path "$brain\build" -Recurse -Filter "*llama*.dll" | Copy-Item -Destination $llmDist -Force
    Get-ChildItem -Path "$brain\build" -Recurse -Filter "*ggml*.dll" | Copy-Item -Destination $llmDist -Force

    # -----------------------------------------------------------------
    # 4. AKTUALIZACJA PLIKU BAT (na wypadek ręcznego uruchamiania)
    # -----------------------------------------------------------------
    $batPath = "$dist\CogitoNexus.bat"
    $batContent = "@echo off`r`nchcp 65001`r`nset PATH=%~dp0LLM;%~dp0whisper;`%PATH%`r`nbrain.exe`r`npause"
    Set-Content -Path $batPath -Value $batContent
}

# --- Funkcja sprawdzająca zmiany w kodzie ---
function Test-NeedsBuild {
    param (
        [string]$SourceDir,
        [string]$TargetFile
    )
    if (!(Test-Path $TargetFile)) { return $true }
    
    $targetDate = (Get-Item $TargetFile).LastWriteTime
    
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

# --- Kopiowanie ikony i pliku do tworzenia skrótów do dist ---
# Uwaga: NIE tworzymy tu skrótów automatycznie. Kopiujemy tylko ikonę
# oraz CogitoNexus-Skroty.bat do dist, żeby użytkownik mógł go uruchomić
# ręcznie (np. jako administrator, jeśli chce skróty na Pulpicie
# Wszystkich Użytkowników / w Menu Start).
function Copy-ShortcutTools {
    if (!(Test-Path $dist)) { return }

    $iconSource = "$PSScriptRoot\icons\CogitoNexus-icon.ico"
    if (Test-Path $iconSource) {
        cp $iconSource "$dist\CogitoNexus.ico" -Force
    } else {
        Write-Host "  -> Uwaga: brak ikony w $iconSource." -F DarkYellow
    }

    $scriptSource = "$PSScriptRoot\CogitoNexus-Skroty.bat"
    if (Test-Path $scriptSource) {
        cp $scriptSource "$dist\CogitoNexus-Skroty.bat" -Force
    } else {
        Write-Host "  -> Uwaga: brak pliku $scriptSource." -F DarkYellow
    }
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

Copy-ShortcutTools

Write-Host "--- Gotowe --- (aby utworzyć skróty, uruchom ręcznie dist\CogitoNexus-Skroty.bat)" -F Cyan
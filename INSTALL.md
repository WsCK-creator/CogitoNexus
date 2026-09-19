# CogitoNexus — instrukcja kompilacji na nowym komputerze

Projekt składa się z dwóch części budowanych **osobnymi, niekompatybilnymi ze sobą
zestawami narzędzi**:

- **`nao_bridge`** — most do robota NAO, budowany starym SDK NAOqi (qibuild, CMake 3.2.3, VS2015/v140).
- **`main_app`** — "mózg" (Whisper + LLM/llama.cpp + CUDA), budowany nowoczesnym CMake + VS2022/v143.

Obie części budowane są jednym skryptem: `build_cogito-nexus.ps1` (w katalogu głównym repo).

---

## 1. Wymagania dla `nao_bridge` (most NAO)

1. **NAOqi C++ SDK 2.8.5.10** — rozpakowany np. do `C:\nao\naoqi-sdk-c++-2.8.5.10`.
2. **Python** (potrzebny do `qibuild`) + zainstalowany pakiet `qibuild`:
   ```
   pip install qibuild
   ```
3. **CMake 3.2.3** (stara, samodzielna wersja — nowsze CMake odrzucają
   `cmake_minimum_required(VERSION 2.8)` używane w `nao_bridge/CMakeLists.txt`).
   Zainstaluj do `C:\Program Files\CMake-3.2.3\bin` (dokładnie ta ścieżka — skrypt
   builda się do niej odwołuje).
   Wersja do pobrania: https://cmake.org/files/v3.2/cmake-3.2.3-win32-x86.zip
4. **Visual Studio 2015 Community** z komponentem **Visual C++** (toolset v140).
   Stare, oficjalne "Build Tools 2015" bez C++ NIE wystarczą — musi być pełne IDE
   albo wybrany właśnie workload C++.
5. **Windows 8.1 SDK** (samodzielny instalator, wymagany przez toolset v140):
   https://go.microsoft.com/fwlink/p/?LinkId=323507
   *(uwaga: to stary, nieserwisowany instalator Microsoftu — działa, ale nie jest już aktualizowany pod kątem bezpieczeństwa)*

### Konfiguracja qibuild (jednorazowo, po instalacji powyższych)

```
qitoolchain create nao-bridge-toolchain "C:\nao\naoqi-sdk-c++-2.8.5.10\toolchain.xml"
qibuild add-config nao-bridge-config --toolchain nao-bridge-toolchain --cmake-generator "Visual Studio 14 2015 Win64"
```

Ważne: generator **musi** kończyć się na `Win64` — bez tego qibuild konfiguruje
kompilację 32-bitową i linkowanie padnie błędem `LNK1112` (konflikt x86/x64).

Sprawdzić poprawność konfiguracji można komendą `qibuild config`.

`VCTargetsPath` (ścieżka do targetów MSBuild dla v140) jest ustawiana i przywracana
**automatycznie** przez funkcję `Build-Bridge` w skrypcie builda — nie trzeba tego
robić ręcznie ani ustawiać jako stałej zmiennej systemowej (robienie tego na stałe
psuje budowanie `main_app`, bo koliduje z toolsetem v143 z VS2022).

---

## 2. Wymagania dla `main_app` (mózg: Whisper + LLM + CUDA)

1. **Visual Studio 2022 Community** z workloadem **"Desktop development with C++"**
   (daje toolset v143). Zainstaluj/dodaj przez Visual Studio Installer → Modify → Workloads.
2. **CMake** (wersja aktualna, dowolna nowa — instalowana normalnie, np. przez
   `winget install Kitware.CMake` albo instalator ze strony cmake.org). To NIE jest
   ta sama instalacja co CMake 3.2.3 z punktu 1 — obie mogą współistnieć, skrypt sam
   przełącza `PATH` na czas budowania mostu.
3. **CUDA Toolkit — dokładnie wersja 13.2** (build 13.2.78). To krytyczne: kod
   `ggml-cuda` w tym repo używa API biblioteki `cub`/CCCL, które NVIDIA zmieniła
   między wydaniami 13.2 i 13.4 — nowsza wersja (13.4+) się nie skompiluje
   (błędy typu `namespace "cuda" has no member "make_counting_iterator"`).
   Pobierz dokładnie tę wersję z archiwum:
   https://developer.nvidia.com/cuda-13-2-0-download-archive
   Podczas instalacji wybierz opcję niestandardową i zaznacz integrację z Visual
   Studio 2022 — instalator doda wtedy plik `.targets` do
   `...\2022\Community\MSBuild\Microsoft\VC\v170\BuildCustomizations\`.
   Inne wersje CUDA mogą być zainstalowane obok — nie trzeba ich usuwać.

**Uruchamianie builda main_app musi się odbywać z uprawnieniami Administratora**
(bez tego CMake potrafi nie wykryć poprawnie zainstalowanej instancji Visual Studio —
błąd "could not find any instance of Visual Studio" mimo że `vswhere.exe` widzi ją
poprawnie).

---

## 3. Budowanie

Z katalogu głównego repo, w terminalu **uruchomionym jako Administrator**:

```powershell
# całość (most + mózg)
.\build_cogito-nexus.ps1 -Module all

# albo osobno
.\build_cogito-nexus.ps1 -Module bridge
.\build_cogito-nexus.ps1 -Module brain

# albo bez parametru - skrypt sam sprawdzi co wymaga przebudowy
.\build_cogito-nexus.ps1
```

Gotowe pliki (`nao_bridge`, `brain.exe`, biblioteki `whisper`/`LLM`) trafiają do
folderu `dist\` w katalogu głównym repo.

---

## 4. Skrót — kolejność instalacji od zera

1. Python + `pip install qibuild`
2. NAOqi C++ SDK 2.8.5.10 (rozpakowany np. do `C:\nao\...`)
3. CMake 3.2.3 → `C:\Program Files\CMake-3.2.3\bin`
4. Visual Studio 2015 Community + workload C++
5. Windows 8.1 SDK (standalone)
6. `qitoolchain create` + `qibuild add-config` (patrz punkt 1 wyżej)
7. Visual Studio 2022 Community + workload "Desktop development with C++"
8. CUDA Toolkit **13.2** (z integracją VS2022)
9. Aktualny CMake (dla main_app)
10. Uruchomić `build_cogito-nexus.ps1 -Module all` jako Administrator

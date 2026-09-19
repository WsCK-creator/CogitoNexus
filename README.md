# CogitoNexus

CogitoNexus to system sterowania robotami humanoidalnymi (**NAO** oraz **Booster K1**) za pomocą lokalnej sztucznej inteligencji: robot słucha użytkownika (VAD + Whisper), rozumie wypowiedź i generuje odpowiedź (lokalny model językowy uruchamiany przez llama.cpp), a następnie mówi do użytkownika i może wykonać gest/ruch/taniec — wszystko offline, bez wysyłania danych na zewnątrz.

## Architektura

Projekt składa się z trzech głównych części:

- **`main_app` ("Mózg")** — program uruchamiany na komputerze PC (Windows), będący centralnym punktem systemu. Odpowiada za:
  - VAD (`vad/`) — wykrywanie mowy w strumieniu audio (na bazie RNNoise) i wycinanie pojedynczej wypowiedzi z ciągłego strumienia dźwięku,
  - Whisper (`whisper/`) — transkrypcję mowy na tekst (whisper.cpp),
  - LLM (`LLM/`) — generowanie odpowiedzi robota lokalnym modelem językowym (llama.cpp, model Gemma w formacie GGUF),
  - komunikację z robotem — przez `nao_bridge.dll` (NAO) albo bezpośrednio przez TCP/JSON (`booster/BoosterBridge`, Booster).

- **`nao_bridge`** — most (DLL) do robota NAO, budowany starym zestawem narzędzi NAOqi C++ SDK 2.8.5.10 (qibuild/CMake 3.2.3/VS2015). Przekazuje audio z mikrofonu NAO do `main_app` i wysyła do robota polecenia mowy/ruchu przez ALMemory.

- **`booster-CogitoNexus`** — serwer C++ uruchamiany **na samym robocie Booster K1** (Linux), niezależny projekt budowany osobno (`build.sh`) Booster Robotics SDK. Odpowiada za: przechwytywanie audio z mikrofonu-array robota i streamowanie go do `main_app` przez TCP, syntezę mowy (TTS przez MMS-TTS/Piper, `mms_worker.py`), wykonywanie tańców/gestów (`B1LocoClient`) i odczyt przycisków kontrolera. Szczegóły w [`booster-CogitoNexus/README.md`](booster-CogitoNexus/README.md).

Dodatkowo:
- **`dataTypes/`** — wspólne typy i sygnatury callbacków używane między `main_app` a `nao_bridge`, oraz prompty systemowe dla LLM (osobne dla NAO i Boostera).
- **`new/animations/`**, **`animations.pml`** — paczki animacji/zachowań Choregraphe dla NAO.

## Wymagania i budowanie

Pełna lista narzędzi do zainstalowania (dla obu robotów) oraz kolejność instalacji znajduje się w [`INSTALL.md`](INSTALL.md).

Budowanie całości (most NAO + mózg) odbywa się jednym skryptem z katalogu głównego, **w terminalu uruchomionym jako Administrator**:

```powershell
.\build_cogito-nexus.ps1 -Module all
```

Gotowe pliki trafiają do folderu `dist/` w katalogu głównym repo.

`booster-CogitoNexus` buduje się i wdraża osobno, bezpośrednio na robocie Booster (`./build.sh`) — to nie jest część `build_cogito-nexus.ps1`.

## Folder `dist/models` — modele wymagane do działania

Skrypt budujący **nie pobiera modeli automatycznie** — trzeba je ręcznie umieścić w folderze `models` wewnątrz `dist`, w dokładnie takiej strukturze i pod dokładnie takimi nazwami plików (te ścieżki są zaszyte na sztywno w kodzie, patrz `main_app/brain/brain.cpp`):

```
dist/
├── brain.exe
├── nao_bridge/            <- wynik budowania (nao_bridge.dll + zależności NAOqi)
├── whisper/               <- wynik budowania (DLL-ki whisper.cpp)
├── LLM/                   <- wynik budowania (DLL-ki llama.cpp/ggml/CUDA)
├── CogitoNexus.bat
└── models/
    ├── LLM/
    │   └── gemma-4-E4B-it-UD-Q4_K_XL.gguf
    └── whisper/
        └── ggml-large-v3-turbo.bin
```

Foldery `nao_bridge/`, `whisper/` i `LLM/` (te z bibliotekami DLL, nie mylić z `models/LLM`!) są tworzone automatycznie przy budowaniu — **folder `models/` trzeba stworzyć i wypełnić samodzielnie**, ręcznie pobierając poniższe pliki:

| Plik | Skąd pobrać |
|---|---|
| `models/LLM/gemma-4-E4B-it-UD-Q4_K_XL.gguf` | [unsloth/gemma-4-E4B-it-GGUF](https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF/blob/main/gemma-4-E4B-it-UD-Q4_K_XL.gguf) (Hugging Face) |
| `models/whisper/ggml-large-v3-turbo.bin` | [ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp/blob/main/ggml-large-v3-turbo.bin) (Hugging Face) |

Bez tych plików, dokładnie pod tymi nazwami i w tych podfolderach, `brain.exe` zgłosi błąd wczytywania modelu (LLM: "Failed to load model!" / Whisper: "Unable to load whisper model") i program się nie uruchomi.

## Uruchamianie

Program uruchamia się z folderu `dist` (np. przez `CogitoNexus.bat` albo bezpośrednio `brain.exe`) i pyta w konsoli o:
1. rodzaj robota (NAO / Booster / Trumna),
2. dla Boostera dodatkowo: głośność TTS i długość wypowiedzi (Enter = bez zmian),
3. adres IP robota,
4. opcjonalny kontekst sytuacji/wydarzenia (dowolny tekst wpisywany wieloliniowo, zakończony Ctrl+Z + Enter) — trafia do promptu systemowego LLM jako dodatkowy kontekst.

## Dokumentacja uzupełniająca

- [`INSTALL.md`](INSTALL.md) — pełna lista wymaganych narzędzi i kolejność instalacji.
- [`booster-CogitoNexus/README.md`](booster-CogitoNexus/README.md) — konfiguracja serwera na robocie Booster (TTS, głosy, systemd service).

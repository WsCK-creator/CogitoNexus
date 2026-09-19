#pragma once

// Jak w main.cpp -- musi być zdefiniowane przed pierwszym włączeniem
// <windows.h> w danej jednostce translacji (dla brain.cpp to właśnie tutaj
// następuje pierwsze włączenie), inaczej windows.h ściąga stary
// <winsock.h>, co koliduje z <winsock2.h> z BoosterBridge.hpp włączanym
// niżej w tym samym pliku.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <iostream>
#include <conio.h>
#include <windows.h>
#include <direct.h>

#include <thread>
#include <atomic>
#include <chrono>

#include <vector>
#include <queue>
#include <mutex>

#include <fstream>
#include <cstdint>

#include "./../../dataTypes/dataTypes.hpp"
#include "vad.hpp"
#include "whisper-wrapper.hpp"
#include "LLM.hpp"
#include "booster/BoosterBridge.hpp"

class Brain
{
private:
    static constexpr const char* moduleName = "[Brain] ";

    inline static DataTypes::BridgeInitFunc _bridgeInitFunc;
    inline static DataTypes::BridgeStopFunc _bridgeStopFunc;
    inline static DataTypes::DataToNaoFunc _dataToNaoFunc;
    inline static DataTypes::ProcessingStartedFunc _procesingStartedFunc;

    static void __stdcall _messageCallback(const char* str, DataTypes::MessageType type);
    static void __stdcall _errorCallback(const char* module, const char* error, DataTypes::Errorcodes code);
    static void __stdcall _fromNaoGetAudio(const char* data);
    static void __stdcall _vadCallback(std::vector<float> normalizedData);
    
    std::atomic<bool> _bridgeState{false};
    inline static std::atomic<bool> _vadState{false};
    std::atomic<bool> _llmState{false};
    std::thread _naoBridgeThread;
    inline static std::thread _vadThread;
    std::thread _llmThread;
    HMODULE _hBridge = nullptr;
   
    inline static std::unique_ptr<VAD> _vad = nullptr;
    inline static std::unique_ptr<WhisperWrapper> _whisper = nullptr;
    inline static std::unique_ptr<LLM> _llm = nullptr;
    inline static std::unique_ptr<BoosterBridge> _boosterBridge = nullptr;
    inline static std::queue<std::vector<unsigned short>> audioQueue;
    inline static std::mutex mtx;
    inline static unsigned int totalSamplesCount = 0;
    inline static std::string _dataFromNao = "";
    inline static std::string _robotIP;
    inline static std::string _robotContext;
    inline static DataTypes::RobotType _robotType;

    inline static std::chrono::time_point<std::chrono::steady_clock> start_time;

    // New members for button logic and booster
    // (_boosterBridge był tu zadeklarowany drugi raz -- redefinicja pola,
    // błąd kompilacji. Jedyna instancja to statyczne pole zdefiniowane wyżej.
    // _buttonStartTriggered/_isListening/_audioHistoryBuffer zniknęły --
    // patrz _triggerListening: obie ścieżki, NAO i Booster, używają teraz
    // tego samego, już wcześniej działającego mechanizmu VAD/Whisper.)
    inline static bool _lastButtonState = false;

    // Ustawienia TTS Boostera (głośność / długość wypowiedzi) podawane na
    // starcie programu -- tylko dla Boostera, NAO ich nie obsługuje.
    // -1.0 oznacza "nie zmieniaj" (użytkownik zostawił pole puste), dlatego
    // NIE wysyłamy wtedy komendy "settings" wcale -- robot ma zostać przy
    // swoich domyślnych/poprzednich wartościach.
    inline static double _boosterVolume = -1.0;
    inline static double _boosterLengthScale = -1.0;
    static void _sendBoosterSettings();

    bool _init();
    bool _loadDLL();
    static void _startNaoBridge(std::atomic<bool>& state);
    static void _stopNaoBridge();
    static void _whenWhisperFinished(std::atomic<bool>& state);
    void _loop();
    // Statyczne, bo wołane z callbacków C (_messageCallback,
    // _whenWhisperFinished), które same muszą być statyczne (wskaźniki na
    // funkcje __stdcall nie mogą nieść ze sobą "this") -- nie da się z nich
    // wywołać zwykłej metody instancyjnej bez obiektu.
    static void _sendData(const char* data);
    static void _handleButtonEvent(const char* json);
    static void _startInteractionSequence();
    // Wspólne dla obu robotów: startuje analizę VAD bieżącego nagrania,
    // jeśli nie jest już w trakcie. Na NAO wywołuje to
    // _fromNaoGetAudio (event "CogitoNexusBroker/ProcessData" z nao_bridge),
    // na Boosterze -- _startInteractionSequence (przycisk "start").
    static void _triggerListening();

    // Minimalny parser JSON używany przez _handleButtonEvent (te same
    // narzędzia co w BoosterBridge -- tam są prywatne dla tamtej klasy,
    // więc Brain potrzebuje własnej kopii).
    static bool jsonHas(const std::string& j, const std::string& key);
    static std::string parseJsonStr(const std::string& j, const std::string& key);
public:
    // boosterVolume/boosterLengthScale: -1.0 = bez zmian (wartość domyślna,
    // gdy użytkownik w main.cpp zostawi pole puste i wciśnie Enter).
    Brain(DataTypes::RobotType robotType, std::string robotIP, std::string context,
          double boosterVolume = -1.0, double boosterLengthScale = -1.0);
    ~Brain();

    // Uruchamia Brain (_init + główna pętla). main.cpp wcześniej tylko
    // tworzył obiekt Brain i od razu kończył program -- nic się nie działo.
    void run();
};

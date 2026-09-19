#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <memory>
#include <iostream>
#include <cstring>
#include <chrono>

#include "./../../dataTypes/dataTypes.hpp"

// Networking
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    typedef int SOCKET;
    #ifndef INVALID_SOCKET
    #define INVALID_SOCKET (-1)
    #endif
    #ifndef SOCKET_ERROR
    #define SOCKET_ERROR (-1)
    #endif
#endif

class BoosterBridge {
public:
    BoosterBridge(std::string ip, int port);
    ~BoosterBridge();

    bool init();
    void stop();

    // Metody komunikacji (zgodne z wymaganiami Braina)
    void sendData(const char* data);
    void onMessage(const char* str, DataTypes::MessageType type);
    void onError(const char* module, const char* error, DataTypes::Errorcodes code);
    void onAudio(const signed short* buffer, int count);
    void onDataFromRobot(const char* data);
    void onProcessingStarted();

    // Rejestracja callbacków wołanych, gdy z robota przyjdzie odpowiednia
    // wiadomość (patrz handleRawMessage). Bez tego most parsował dane, ale
    // nigdy nie informował Brain o zdarzeniach (przycisk/audio/tts/dance).
    void setMessageCallback(std::function<void(const char*, DataTypes::MessageType)> cb);
    void setErrorCallback(std::function<void(const char*, const char*, DataTypes::Errorcodes)> cb);
    void setAudioCallback(std::function<void(const signed short*, int)> cb);
    void setDataFromRobotCallback(std::function<void(const char*)> cb);
    void setProcessingStartedCallback(std::function<void()> cb);

private:
    void receiveLoop();
    void handleRawMessage(const std::string& line);
    void _audioWatchdog();

    std::string _ip;
    int _port;
    SOCKET _socket;
    std::atomic<bool> _running{false};
    std::thread _receiveThread;
    // Bufor na niedokończone dane TCP między kolejnymi recv() -- wiadomość
    // może zostać rozdzielona na granicy pakietów (linia nie musi przyjść
    // w jednym recv()).
    std::string _recvBuffer;

    // Zamiast logować KAŻDY odebrany pakiet audio (spamuje konsolę, bo
    // leci ciągłym strumieniem), logujemy tylko ciszę na łączu: brak
    // jakiegokolwiek pakietu audio przez 5s.
    std::atomic<long long> _lastAudioMs{0};
    std::atomic<bool> _audioTimeoutLogged{false};
    std::thread _audioWatchdogThread;
    static constexpr long long AUDIO_TIMEOUT_MS = 5000;

    // Callbacks (zostaną powiązane z Brainem)
    std::function<void(const char*, DataTypes::MessageType)> _messageCallback;
    std::function<void(const char*, const char*, DataTypes::Errorcodes)> _errorCallback;
    std::function<void(const signed short*, int)> _audioCallback;
    std::function<void(const char*)> _dataFromRobotCallback;
    std::function<void()> _processingStartedCallback;

    // Proste parsowanie JSON (na potrzeby komunikacji z Boosterem)
    std::string parseJsonStr(const std::string& j, const std::string& key);
    bool jsonHas(const std::string& j, const std::string& key);
    // parseJsonStr obsługuje tylko wartości w cudzysłowach -- "channels" to
    // liczba (np. "channels":3), więc potrzebny jest osobny parser liczb
    // całkowitych. Zwraca defaultValue, jeśli pole nie występuje/jest
    // niepoprawne.
    int parseJsonInt(const std::string& j, const std::string& key, int defaultValue);
};

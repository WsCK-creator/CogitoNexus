#include "BoosterBridge.hpp"
#include <sstream>
#include <iostream>
#include <cctype>

namespace {
// Minimalny dekoder base64 -- serwer na robocie wysyła audio jako
// {"type":"audio", ..., "data":"<base64 PCM>"} (patrz booster-CogitoNexus.cpp).
std::vector<unsigned char> base64Decode(const std::string& in) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> table(256, -1);
    for (int i = 0; i < 64; i++) table[static_cast<unsigned char>(chars[i])] = i;

    std::vector<unsigned char> out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        if (table[c] == -1) continue; // pomiń białe znaki / śmieci
        val = (val << 6) + table[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}
}

BoosterBridge::BoosterBridge(std::string ip, int port) : _ip(ip), _port(port), _socket(INVALID_SOCKET) {}

BoosterBridge::~BoosterBridge() {
    stop();
}

bool BoosterBridge::init() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    _socket = socket(AF_INET, SOCK_STREAM, 0);
    if (_socket == INVALID_SOCKET) {
        std::cerr << "[BoosterBridge] Error creating socket" << std::endl;
        return false;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(_port);
    inet_pton(AF_INET, _ip.c_str(), &serverAddr.sin_addr);

    if (connect(_socket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "[BoosterBridge] Connection failed to " << _ip << ":" << _port << std::endl;
        return false;
    }

    _running = true;
    _lastAudioMs = 0;
    _audioTimeoutLogged = false;
    _receiveThread = std::thread(&BoosterBridge::receiveLoop, this);
    _audioWatchdogThread = std::thread(&BoosterBridge::_audioWatchdog, this);
    std::cout << "[BoosterBridge] Connected to " << _ip << ":" << _port << std::endl;
    return true;
}

void BoosterBridge::stop() {
    _running = false;
    if (_receiveThread.joinable()) {
        _receiveThread.join();
    }
    if (_audioWatchdogThread.joinable()) {
        _audioWatchdogThread.join();
    }
    if (_socket != INVALID_SOCKET) {
        closesocket(_socket);
        _socket = INVALID_SOCKET;
    }
}

void BoosterBridge::_audioWatchdog() {
    // Zamiast logować każdy odebrany pakiet audio (leci ciągłym strumieniem,
    // więc zalewałoby to konsolę), zgłaszamy tylko ciszę na łączu -- brak
    // JAKIEGOKOLWIEK pakietu audio przez AUDIO_TIMEOUT_MS.
    using namespace std::chrono;
    while (_running) {
        std::this_thread::sleep_for(milliseconds(500));

        long long last = _lastAudioMs.load();
        if (last == 0) continue; // jeszcze nie odebrano żadnego pakietu audio

        long long now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        if (now - last >= AUDIO_TIMEOUT_MS) {
            if (!_audioTimeoutLogged.exchange(true)) {
                std::cout << "[BoosterBridge] Brak pakietow audio od " << (AUDIO_TIMEOUT_MS / 1000)
                          << "s" << std::endl;
            }
        }
    }
}

void BoosterBridge::sendData(const char* data) {
    if (_socket == INVALID_SOCKET || !data) return;

    std::string msg(data);
    // Serwer na robocie czeka na '\n' żeby uznać linię za kompletną
    // (patrz booster-CogitoNexus.cpp, pętla po buffer.find('\n')) -- bez
    // tego komenda nigdy nie zostałaby rozpoznana.
    if (msg.empty() || msg.back() != '\n') msg += '\n';

    size_t total = 0;
    while (total < msg.size()) {
        int sent = send(_socket, msg.c_str() + total, (int)(msg.size() - total), 0);
        if (sent == SOCKET_ERROR || sent <= 0) break;
        total += (size_t)sent;
    }
}

void BoosterBridge::receiveLoop() {
    char buf[65536];
    while (_running) {
        int bytesRead = recv(_socket, buf, sizeof(buf), 0);
        if (bytesRead > 0) {
            // TCP nie gwarantuje, że jedna wiadomość dotrze w jednym recv() --
            // trzeba buforować i wyciągać kompletne linie, tak jak robi to
            // serwer na robocie. Wcześniej każdy recv() był traktowany jako
            // komplet linii, co gubiło/psuło wiadomości rozdzielone na
            // granicy pakietów (a przy ciągłym streamingu audio zdarza się
            // to bardzo często).
            _recvBuffer.append(buf, bytesRead);
            size_t pos;
            while ((pos = _recvBuffer.find('\n')) != std::string::npos) {
                std::string line = _recvBuffer.substr(0, pos);
                _recvBuffer.erase(0, pos + 1);
                if (!line.empty()) handleRawMessage(line);
            }
        } else if (bytesRead == 0 || bytesRead == SOCKET_ERROR) {
            std::cout << "[BoosterBridge] Connection closed" << std::endl;
            _running = false;
            break;
        }
    }
}

void BoosterBridge::handleRawMessage(const std::string& line) {
    bool isAudio = jsonHas(line, "\"type\":\"audio\"");

    // Pakiety audio lecą ciągłym strumieniem (kilka-kilkanaście na sekundę),
    // więc logowanie każdego z nich zalewałoby konsolę -- dla nich
    // zamiast tego zgłaszamy tylko ciszę na łączu (patrz _audioWatchdog).
    if (!isAudio) {
        std::cout << "[BoosterBridge] Received: " << line << std::endl;
    }

    // "button"/"tts"/"dance" mają ten sam kształt co komunikaty z mostu NAO
    // ("type":"..." + pola), więc przekazujemy surową linię do
    // _messageCallback -- dokładnie tak samo jak robi to nao_bridge.dll --
    // a Brain::_handleButtonEvent (wołany z Brain::_messageCallback) sam
    // wyciąga z niej to, czego potrzebuje.
    if (jsonHas(line, "\"type\":\"button\"") ||
        jsonHas(line, "\"type\":\"tts\"") ||
        jsonHas(line, "\"type\":\"dance\"")) {
        if (_messageCallback) _messageCallback(line.c_str(), DataTypes::MessageType::MLog);
    } else if (isAudio) {
        _lastAudioMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        _audioTimeoutLogged = false;

        std::string b64 = parseJsonStr(line, "\"data\"");
        if (!b64.empty() && _audioCallback) {
            std::vector<unsigned char> pcmBytes = base64Decode(b64);
            int totalSamples = static_cast<int>(pcmBytes.size() / sizeof(signed short));
            const signed short* samples = reinterpret_cast<const signed short*>(pcmBytes.data());

            // Robot wysyła surowe, PRZEPLECIONE dane z mikrofonu-array (patrz
            // komentarz w booster-CogitoNexus.cpp: SDK domyślnie zwraca 3
            // kanały -- czyli sygnał sprzed beamformingu/miksu do mono -- a
            // nie pojedynczy kanał mono). Wcześniej traktowaliśmy te bajty
            // jako zwykłe mono próbki, co psuło samą treść mowy (VAD widział
            // poprawny "kształt" energii, więc timing wyglądał OK, ale
            // Whisper dostawał zniekształcony sygnał i "wymyślał" tekst).
            //
            // Próbowaliśmy najpierw uśredniania wszystkich kanałów do mono --
            // ale mikrofony są fizycznie rozsunięte, więc proste uśrednianie
            // bez wyrównania fazy dawało zniekształcenia grzebieniowe (comb
            // filtering). Próbowaliśmy też gotowego strumienia NAEC z SDK
            // (beamforming + redukcja szumu/echa) -- ten był czysty
            // technicznie, ale jego redukcja szumu okazała się działać jak
            // bramka ucinająca/tłumiąca samą mowę, gdy mówca stał dalej niż
            // ok. 0.5m od robota. Dlatego teraz po prostu WYBIERAMY JEDEN
            // kanał (pierwszy z array'a) i ignorujemy pozostałe -- to nie
            // daje zniekształceń grzebieniowych (nic nie jest mieszane) i
            // nie ucina mowy z odległości (brak agresywnej redukcji szumu),
            // kosztem nieco gorszego SNR niż dawałby poprawny beamforming.
            int channels = parseJsonInt(line, "\"channels\"", 1);
            if (channels < 1) channels = 1;

            if (channels <= 1) {
                if (totalSamples > 0) {
                    _audioCallback(samples, totalSamples);
                }
            } else {
                int frameCount = totalSamples / channels;
                if (frameCount > 0) {
                    std::vector<signed short> singleChannel(frameCount);
                    constexpr int kChannelToUse = 0;
                    for (int f = 0; f < frameCount; f++) {
                        const signed short* frame = samples + static_cast<size_t>(f) * channels;
                        singleChannel[f] = frame[kChannelToUse];
                    }
                    _audioCallback(singleChannel.data(), frameCount);
                }
            }
        }
    } else if (jsonHas(line, "\"type\":\"error\"")) {
        std::string msg = parseJsonStr(line, "\"message\"");
        if (_errorCallback) _errorCallback("[Booster]", msg.c_str(), DataTypes::Errorcodes::UnknownError);
    }
    // "welcome" i inne nierozpoznane typy -- zalogowane wyżej, nic więcej do zrobienia.
}

void BoosterBridge::setMessageCallback(std::function<void(const char*, DataTypes::MessageType)> cb) {
    _messageCallback = std::move(cb);
}
void BoosterBridge::setErrorCallback(std::function<void(const char*, const char*, DataTypes::Errorcodes)> cb) {
    _errorCallback = std::move(cb);
}
void BoosterBridge::setAudioCallback(std::function<void(const signed short*, int)> cb) {
    _audioCallback = std::move(cb);
}
void BoosterBridge::setDataFromRobotCallback(std::function<void(const char*)> cb) {
    _dataFromRobotCallback = std::move(cb);
}
void BoosterBridge::setProcessingStartedCallback(std::function<void()> cb) {
    _processingStartedCallback = std::move(cb);
}

std::string BoosterBridge::parseJsonStr(const std::string& j, const std::string& key) {
    // UWAGA: wołający przekazuje klucz JUŻ w cudzysłowach (np. "\"data\"" albo
    // "\"type\":\"audio\""). Wcześniej ta funkcja doklejała do niego kolejną
    // parę cudzysłowów ("\"" + key + "\""), więc szukany ciąg nigdy nie
    // występował w odbieranych liniach -- rozpoznawanie wiadomości z robota
    // (button/tts/dance/audio) było martwym kodem od samego początku.
    size_t pos = j.find(key);
    if (pos == std::string::npos) return "";
    pos = j.find(':', pos + key.size());
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\t')) pos++;
    if (pos >= j.size() || j[pos] != '"') return "";
    pos++;
    size_t end = pos;
    while (end < j.size()) {
        if (j[end] == '"' && (end == 0 || j[end-1] != '\\')) break;
        end++;
    }
    return j.substr(pos, end - pos);
}

bool BoosterBridge::jsonHas(const std::string& j, const std::string& key) {
    return j.find(key) != std::string::npos;
}

int BoosterBridge::parseJsonInt(const std::string& j, const std::string& key, int defaultValue) {
    size_t pos = j.find(key);
    if (pos == std::string::npos) return defaultValue;
    pos = j.find(':', pos + key.size());
    if (pos == std::string::npos) return defaultValue;
    pos++;
    while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\t')) pos++;

    bool neg = false;
    if (pos < j.size() && (j[pos] == '-' || j[pos] == '+')) {
        neg = (j[pos] == '-');
        pos++;
    }
    if (pos >= j.size() || !isdigit(static_cast<unsigned char>(j[pos]))) return defaultValue;

    long value = 0;
    while (pos < j.size() && isdigit(static_cast<unsigned char>(j[pos]))) {
        value = value * 10 + (j[pos] - '0');
        pos++;
    }
    return static_cast<int>(neg ? -value : value);
}

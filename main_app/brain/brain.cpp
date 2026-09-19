#include "brain.hpp"
#include <sstream>

bool Brain::_init()
{
    start_time = std::chrono::steady_clock::now();

    std::cout << "--- CogitoNexus: Main Brain Starting ---" << std::endl;
    std::cout << moduleName << "Loading LLM" << std::endl;
    // _robotContext to kontekst sytuacji/wydarzenia wpisany na starcie
    // programu (main.cpp) -- wcześniej był zapisywany w Brain, ale nigdy
    // nigdzie dalej nie trafiał, więc model LLM o nim nie wiedział.
    _llm = std::make_unique<LLM>("models/LLM/gemma-4-E4B-it-UD-Q4_K_XL.gguf", _robotType, _robotContext);
    std::cout << moduleName << "Loading Whisper." << std::endl;
    _whisper = std::make_unique<WhisperWrapper>("models/whisper/ggml-large-v3-turbo.bin");
    std::cout << moduleName << "Loading VAD." << std::endl;
    _vad = std::make_unique<VAD>();

    if (_robotType == DataTypes::RobotType::NAO)
    {
        if(!_loadDLL()) return false;
        _startNaoBridge(_bridgeState);
    }
    else if (_robotType == DataTypes::RobotType::Booster)
    {
        _boosterBridge = std::make_unique<BoosterBridge>(_robotIP, 9000);
        // Bez tego most parsował wiadomości z robota, ale nigdy nie
        // informował o nich Brain -- przycisk "start" i audio z Boostera
        // nigdzie nie docierały.
        _boosterBridge->setMessageCallback(_messageCallback);
        _boosterBridge->setErrorCallback(_errorCallback);
        _boosterBridge->setAudioCallback(VAD::audiCallback);
        if (!_boosterBridge->init()) {
            std::cerr << "[ERROR] Failed to init BoosterBridge" << std::endl;
            return false;
        }
        _bridgeState.store(true);
        std::cout << moduleName << "BoosterBridge started" << std::endl;
        _sendBoosterSettings();
    }
    else if (_robotType == DataTypes::RobotType::Trumna)
    {
        return true;
    }
    else
    {
        return false;
    }
    return true;
}

bool Brain::_loadDLL()
{
    char cwd[1024];
    if (_getcwd(cwd, sizeof(cwd)) != NULL) {
        std::cout << "[INFO] Szukam DLL w: " << cwd << std::endl;
    }

    std::string fullDllPath = std::string(cwd) + "\\nao_bridge\\nao_bridge.dll";
    _hBridge = LoadLibraryExA(fullDllPath.c_str(), NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    if (!_hBridge) {
        std::cerr << "[ERROR] Nie mozna zaladowac: " << fullDllPath << std::endl;
        std::cerr << "Kod bledu Windows: " << GetLastError() << std::endl;
        return false;
    }

    std::cout << "[SUCCESS] Most nao_bridge.dll zaladowany poprawnie!\n" << std::endl;

    _bridgeInitFunc = (DataTypes::BridgeInitFunc)GetProcAddress(_hBridge, "nao_bridge_init");
    _bridgeStopFunc = (DataTypes::BridgeStopFunc)GetProcAddress(_hBridge, "nao_bridge_stop");
    _dataToNaoFunc = (DataTypes::DataToNaoFunc)GetProcAddress(_hBridge, "send_to_nao");
    _procesingStartedFunc = (DataTypes::ProcessingStartedFunc)GetProcAddress(_hBridge, "processing_started");

    if (!_bridgeInitFunc || !_bridgeStopFunc || !_dataToNaoFunc || !_procesingStartedFunc) {
        std::cerr << "[ERROR] Nie znaleziono funkcji w DLL!" << std::endl;
        return false;
    }
    return true;
}

void Brain::_startNaoBridge(std::atomic<bool>& state)
{
    _bridgeInitFunc(_messageCallback, _errorCallback, _vad->audiCallback, _fromNaoGetAudio, _robotIP.c_str(), 9559, false);
    std::cout << moduleName << "Broker started" << std::endl;
}

void Brain::_stopNaoBridge()
{
    if(_bridgeStopFunc) _bridgeStopFunc();
    std::cout << moduleName << "end of stop func" << std::endl;
}

void Brain::_whenWhisperFinished(std::atomic<bool>& state)
{
    auto current_time = std::chrono::steady_clock::now();
    bool newChat = false;
    if (current_time - start_time >= std::chrono::minutes(1)) newChat = true;
    else start_time = std::chrono::steady_clock::now();
    
    std::string str = "=== TWÓJ BIEŻĄCY STAN FIZYCZNY I OTOCZENIE ===\n"
    + _dataFromNao
    + "\n=== SŁOWA UŻYTKOWNIKA  ===\n\""
    + _whisper->getText() + "\"\n";
    
    _llm->generateResponse(str, state, newChat);
    _sendData(_llm->getLastJsonResponse().c_str());
    state.store(false);
}

void Brain::_loop()
{
    while(true)
    {
        if(!_bridgeState.load()){
            std::cout << moduleName << "Starting Broker" << std::endl;
            if(_robotType == DataTypes::RobotType::NAO) {
                _naoBridgeThread = std::thread(&Brain::_startNaoBridge, std::ref(_bridgeState));
                _naoBridgeThread.detach();
            } else if(_robotType == DataTypes::RobotType::Booster) {
                // Booster is already started in _init
            }
            _bridgeState.store(true);
            std::cout << moduleName << "Working" << std::endl;
        }

        if(_whisper->finished && !_llmState.load())
        {
            if(_llmThread.joinable()) _llmThread.join();
            _whisper->finished = false;
            _llmState.store(true);
            _llmThread = std::thread(&Brain::_whenWhisperFinished, std::ref(_llmState));
        }

        if (_kbhit())
        {
            std::cout << moduleName << "Stoping all" << std::endl;
            _vadState.store(false);
            if(_vadThread.joinable()) _vadThread.join();
            _llmState.store(false);
            if(_llmThread.joinable()) _llmThread.join();
            _stopNaoBridge();
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

void Brain::_messageCallback(const char *str, DataTypes::MessageType type)
{
    switch (type)
    {
    case DataTypes::MessageType::MLog :
        std::cout << str;
        break;
    case DataTypes::MessageType::MError :
        std::cerr << str;
        break;
    }
    _handleButtonEvent(str);
}

void Brain::_errorCallback(const char *module, const char *error, DataTypes::Errorcodes code)
{
    std::cerr << module << error << " (Code: " << (int)code << ")" << std::endl;
}

void Brain::_fromNaoGetAudio(const char* data)
{
    // Mimo nazwy, to NIE jest surowe audio (i traktowanie go jak
    // zero-terminowanego C-stringa przez strlen() na binarnych próbkach PCM
    // było błędem -- ucinało dane na pierwszym bajcie 0x00). To callback
    // eventu NAO "CogitoNexusBroker/ProcessData" (patrz
    // MemoryModule::callback w nao_bridge) -- semantyczny odpowiednik
    // wciśnięcia przycisku "start" na Boosterze: sygnał "przeanalizuj to,
    // co właśnie nagrano". Samo audio z mikrofonu NAO leci osobnym kanałem
    // prosto do VAD::audiCallback (patrz SoundModule::process oraz
    // Brain::_startNaoBridge, gdzie jest rejestrowany jako AudioCallback).
    std::cout << moduleName << "ProcessData event z NAO: " << data << std::endl;
    _dataFromNao = data;
    _triggerListening();
}

void Brain::_triggerListening()
{
    // Wspólne dla NAO (event ProcessData) i Boostera (przycisk start).
    // VAD::update samo ustawia state=false, gdy wykryje koniec wypowiedzi
    // (patrz vad.cpp), więc sprawdzenie _vadState tutaj to bezpieczny
    // "debounce" przeciwko odpaleniu drugiej analizy, zanim poprzednia się
    // skończy.
    if (_vadState.load()) return;

    _vad->setNewAudio(true);
    _vadState.store(true);
    _vadThread = std::thread(&VAD::update, _vad.get(), _vadCallback, std::ref(_vadState));
    _vadThread.detach();
}

void Brain::_vadCallback(std::vector<float> normalizedData)
{
    // Samo wykrycie końca wypowiedzi jeszcze nie generuje odpowiedzi LLM --
    // robi to _whenWhisperFinished, wołane z _loop() dopiero gdy Whisper
    // skończy transkrypcję (_whisper->finished), żeby do promptu trafił
    // faktyczny tekst użytkownika (a nie tylko _dataFromNao, które dla
    // Boostera i tak jest puste).
    _whisper->audiCallback(normalizedData);
}

void Brain::_sendBoosterSettings() // static
{
    // Wysyłamy komendę tylko jeśli użytkownik faktycznie coś podał na
    // starcie (main.cpp) -- puste pole (Enter) zostaje jako -1.0 i wtedy
    // NIE wysyłamy nic, żeby nie nadpisać ustawień robota wartościami
    // domyślnymi, których użytkownik nie prosił się zmieniać.
    if (_boosterVolume < 0.0 && _boosterLengthScale < 0.0) return;

    std::ostringstream oss;
    oss << "{\"type\":\"settings\"";
    if (_boosterVolume >= 0.0) oss << ",\"volume\":" << _boosterVolume;
    if (_boosterLengthScale >= 0.0) oss << ",\"length_scale\":" << _boosterLengthScale;
    oss << "}";

    std::cout << moduleName << "Wysylam ustawienia do robota: " << oss.str() << std::endl;
    _sendData(oss.str().c_str());
}

void Brain::_sendData(const char* data) // static
{
    if(_robotType == DataTypes::RobotType::NAO && _dataToNaoFunc) {
        _dataToNaoFunc(data);
    } else if(_robotType == DataTypes::RobotType::Booster && _boosterBridge) {
        _boosterBridge->sendData(data);
    }
}

bool Brain::jsonHas(const std::string& j, const std::string& key) {
    return j.find(key) != std::string::npos;
}

std::string Brain::parseJsonStr(const std::string& j, const std::string& key) {
    size_t pos = j.find(key);
    if (pos == std::string::npos) return "";
    pos = j.find(':', pos + key.size());
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\t')) pos++;
    if (pos >= j.size()) return "";

    if (j[pos] == '"') {
        pos++;
        size_t end = pos;
        while (end < j.size()) {
            if (j[end] == '"' && (end == 0 || j[end - 1] != '\\')) break;
            end++;
        }
        return j.substr(pos, end - pos);
    }

    // Wartość niecytowana (np. true/false/liczba w "pressed":true)
    size_t end = pos;
    while (end < j.size() && j[end] != ',' && j[end] != '}' && j[end] != ' ') end++;
    return j.substr(pos, end - pos);
}

void Brain::_handleButtonEvent(const char* json) // static
{
    std::string j(json);
    if (jsonHas(j, "\"type\":\"button\"")) {
        std::string btn = parseJsonStr(j, "\"button\"");
        std::string pressed = parseJsonStr(j, "\"pressed\"");
        
        if (btn == "start" && pressed == "true") {
            if (!_lastButtonState) {
                std::cout << moduleName << "Button 'start' pressed. Starting sequence..." << std::endl;
                _startInteractionSequence();
            }
        }
        _lastButtonState = (pressed == "true");
    }
}

void Brain::_startInteractionSequence() // static
{
    // Odpowiednik przycisku "start" na Boosterze -- na NAO tę samą rolę
    // pełni event "CogitoNexusBroker/ProcessData" obsługiwany przez
    // nao_bridge i docierający tutaj przez _fromNaoGetAudio. Booster nie ma
    // odpowiednika kontekstu/stanu przekazywanego przez NAO, więc
    // _dataFromNao zostaje puste.
    std::cout << moduleName << "Button 'start' pressed. Starting VAD listening..." << std::endl;
    _dataFromNao.clear();
    _triggerListening();
}

Brain::Brain(DataTypes::RobotType robotType, std::string robotIP, std::string context,
             double boosterVolume, double boosterLengthScale)
{
    _robotType = robotType;
    _robotIP = robotIP;
    _robotContext = context;
    _boosterVolume = boosterVolume;
    _boosterLengthScale = boosterLengthScale;
}

Brain::~Brain()
{
    if(_boosterBridge) _boosterBridge->stop();
    if(_hBridge) {
        FreeLibrary(_hBridge);
    }
}

void Brain::run()
{
    // main.cpp wcześniej tylko konstruował obiekt Brain i kończyło main() --
    // _init()/_loop() nigdy nie były wywoływane, więc program nic nie robił.
    if (!_init()) {
        std::cerr << moduleName << "Initialization failed" << std::endl;
        return;
    }
    _loop();
}

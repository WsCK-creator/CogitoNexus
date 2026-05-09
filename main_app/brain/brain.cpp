#include "brain.hpp"

bool Brain::_init()
{
    std::cout << "--- CogitoNexus: Main Brain Starting ---" << std::endl;
    std::cout << moduleName << "Loading LLM" << std::endl;
    _llm = std::make_unique<LLM>("models/LLM/gemma-4-E4B-it-UD-Q4_K_XL.gguf");
    std::cout << moduleName << "Loading Whisper." << std::endl;
    _whisper = std::make_unique<WhisperWrapper>("models/whisper/ggml-large-v3-turbo.bin");
    std::cout << moduleName << "Loading VAD." << std::endl;
    _vad = std::make_unique<VAD>();

    if(!_loadDLL()) return true;

    //_startNaoBridge();

    return false;
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
        std::cin.get();
        return true;
    }

    std::cout << "[SUCCESS] Most nao_bridge.dll zaladowany poprawnie!\n" << std::endl;

    _bridgeInitFunc = (DataTypes::BridgeInitFunc)GetProcAddress(_hBridge, "nao_bridge_init");
    _bridgeStopFunc = (DataTypes::BridgeStopFunc)GetProcAddress(_hBridge, "nao_bridge_stop");

    if (!_bridgeInitFunc) {
        std::cerr << "[ERROR] Nie znaleziono funkcji 'nao_bridge_init'! Blad: " << GetLastError() << std::endl;
        return true;
    }
    if (!_bridgeStopFunc) {
        std::cerr << "[ERROR] Nie znaleziono funkcji 'nao_bridge_stop'! Blad: " << GetLastError() << std::endl;
        return true;
    }
    return false;
}

void Brain::_startNaoBridge(std::atomic<bool>& state)
{
    _bridgeInitFunc(_messageCallback, _errorCallback, _vad->audiCallback, "192.168.0.123", 9559, false);
    std::cout << moduleName << "Broker started" << std::endl;
}

void Brain::_stopNaoBridge()
{
    _bridgeStopFunc();
    std::cout << moduleName << "end of stop func" << std::endl;
}

void Brain::_loop()
{
    while(true)
    {
        if(!_bridgeState.load()){
            std::cout << moduleName << "Starting Broker" << std::endl;
            _naoBridgeThread = std::thread(&Brain::_startNaoBridge, std::ref(_bridgeState));
            _naoBridgeThread.detach();
            _bridgeState.store(true);
            _vad->setNewAudio(true);
        }
        if(!_vadState.load())
        {
            std::cout << moduleName << "Starting VAD" << std::endl;
            _vadState.store(true);
            _vadThread = std::thread(&VAD::update, _vad.get(), _whisper->audiCallback, std::ref(_vadState));
            _vadThread.detach();
        }
        if(!_vad->getNewAudio() && _whisper->finished)
        {
            _llm->generateResponse("siema");
            std::cout << moduleName << "Stoping all" << std::endl;
            _vadState.store(false);
            if(_vadThread.joinable()) _vadThread.join();
            std::thread stopNaoBridge(&Brain::_stopNaoBridge);
            if(stopNaoBridge.joinable()) stopNaoBridge.join();
            break;
        }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    //std::cout << "Samples: " << totalSamplesCount << std::endl;
}

void __stdcall Brain::_messageCallback(const char *str, DataTypes::MessageType type)
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
}

void __stdcall Brain::_errorCallback(const char *module, const char *error, DataTypes::Errorcodes code)
{
    switch (code)
    {
    case DataTypes::Errorcodes::CannotConnect :
        std::cerr << module << error << std::endl;
        if (_bridgeStopFunc) {
            //_bridgeStopFunc();
        }
        break;

    case DataTypes::Errorcodes::CannotPing :
        std::cerr << module << error << std::endl;
        if (_bridgeStopFunc) {
            //_bridgeStopFunc();
        }
        break;

    case DataTypes::Errorcodes::UnknownError :
        std::cerr << module << error << std::endl;
        if (_bridgeStopFunc) {
           // _bridgeStopFunc();
        }
        break;
    }
}

Brain::Brain()
{
    if (!_init()) return;
    _loop();
}

Brain::~Brain()
{
    FreeLibrary(_hBridge);
    std::cout << "----------------------Program zakończył pracę----------------------";
}

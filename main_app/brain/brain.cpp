#include "brain.hpp"

bool Brain::_init()
{
    std::cout << "--- CogitoNexus: Main Brain Starting (C++20) ---" << std::endl;
    
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
    /*audioTime = std::chrono::steady_clock::now();
    std::cout << moduleName << "Waiting 5s" << std::endl;*/
}

void __stdcall Brain::audiCallback(std::vector<float> normalizedData)
{
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
            /*audioTime = std::chrono::steady_clock::now();
            std::cout << moduleName << "Waiting 5s" << std::endl;*/
            _bridgeState.store(true);
            _vad->setNewAudio(true);
        }
        if(!_vadState.load())
        {
            std::cout << moduleName << "Starting VAD" << std::endl;
            _vadState.store(true);
            _vadThread = std::thread(&VAD::update, _vad.get(), audiCallback, std::ref(_vadState));
            _vadThread.detach();
        }
        if(!_vad->getNewAudio())
        {
            std::cout << moduleName << "Stoping all" << std::endl;
            _vadState.store(false);
            if(_vadThread.joinable()) _vadThread.join();
            std::thread stopNaoBridge(&Brain::_stopNaoBridge);
            if(stopNaoBridge.joinable()) stopNaoBridge.join();
            break;
        }
        /*std::chrono::steady_clock::time_point ct = std::chrono::steady_clock::now();
        if(std::chrono::duration_cast<std::chrono::milliseconds>(ct - audioTime) > std::chrono::seconds(5))
        {
            std::cout << moduleName << "Stoping broker" << std::endl;
            std::atomic<bool> stopp{true};
            std::thread stop(&Brain::_stopNaoBridge, std::ref(stopp));
            if(stop.joinable()) stop.join();
            saveQueueToWav("nagranie.wav", audioQueue, totalSamplesCount);
            break;
        }*/
    }
    std::cout << "Samples: " << totalSamplesCount << std::endl;
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

void Brain::saveQueueToWav(std::string filename, std::queue<std::vector<unsigned short>> audioQueue, unsigned int totalSamplesCount)
{
    uint32_t dataSize = totalSamplesCount * sizeof(unsigned short);
    
    DataTypes::WavHeader header;
    header.fileSize = 36 + dataSize;
    header.byteRate = 16000 * 1 * (16 / 8);
    header.blockAlign = 1 * (16 / 8);
    header.subChunk2Size = dataSize;

    std::ofstream outFile(filename, std::ios::binary);

    if (!outFile) {
        std::cerr << "Nie można otworzyć pliku do zapisu!" << std::endl;
        return;
    }

    // 1. Zapisz nagłówek
    outFile.write(reinterpret_cast<const char*>(&header), sizeof(DataTypes::WavHeader));

    // 2. Zapisz dane z kolejki
    // Uwaga: przekazujemy kolejkę przez kopię, aby nie "opróżnić" oryginału, 
    // jeśli jest jeszcze potrzebny w programie.
    while (!audioQueue.empty()) {
        const std::vector<unsigned short>& buffer = audioQueue.front();
        outFile.write(reinterpret_cast<const char*>(buffer.data()), buffer.size() * sizeof(unsigned short));
        audioQueue.pop();
    }

    outFile.close();
    std::cout << "Zapisano pomyślnie do " << filename << std::endl;
}

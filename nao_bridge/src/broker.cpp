#include "broker.hpp"

Broker::Broker(std::string robotIpValue, int robotPortValue)
    : _robotIp(robotIpValue), _robotPort(robotPortValue), _mockArgc(1)
{
    _mockArgv = new char*[2];

    /*std::string name = "nao_bridge";
    _mockArgv[0] = new char[name.size() + 1];
    std::copy(name.begin(), name.end(), _mockArgv[0]);
    _mockArgv[0][name.size()] = '\0';*/
    _mockArgv[0] = "nao_bridge";
    _mockArgv[1] = nullptr;

    _app = std::make_unique<qi::Application>(_mockArgc, _mockArgv);

    std::cout << appName << "Starting." << std::endl;
    init();
}

Broker::~Broker()
{
    std::cout << _brokerName << "Stopping application" << std::endl;

    if (_app) {
        _app->stop();
    }

    if (_appThread.joinable()) {
        _appThread.join();
    }

    delete[] _mockArgv[0];
    delete[] _mockArgv;

    std::cout << _brokerName << "Broker destroyed." << std::endl;
}

void Broker::init()
{
    try
    {
        std::cout << appName << "Connecting to robot." << std::endl;
        _broker = AL::ALBroker::createBroker(_brokerName, "0.0.0.0", 54000, _robotIp, _robotPort);
        std::cout << appName << "Connected to:" << _robotIp << _robotPort << "." << std::endl;
        std::cout << appName << "Starting SoundModule." << std::endl;
        _soundModule = AL::ALModule::createModule<SoundModule>(_broker, "SoundModule");
        
        // Wlasnie w tym miejscu wstrzykujesz dodatkowe parametry do swojego modulu!
        
        _appThread = std::thread([this]() {
                _app->run(); 
        });

    }
    catch(const std::exception& e)
    {
        std::cerr << appName << "CRITICAL ERROR: " << e.what() << std::endl;
    }
    
}

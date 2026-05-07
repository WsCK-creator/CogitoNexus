#include "broker.hpp"

static Broker* gBrokerInstance = nullptr;

Broker::Broker(DataTypes::MessageCallback mcb, DataTypes::ErrorCallback ecb, DataTypes::AudioCallback acb, std::string robotIpValue, int robotPortValue, bool silentQiLog)
    : _robotIp(robotIpValue), _robotPort(robotPortValue), _mockArgc(1)
{
    gBrokerInstance = this;
    
    if (silentQiLog) qi::log::setLogLevel(qi::LogLevel_Silent);

    _messageCallback = mcb;
    _errorCallback = ecb;
    _mockArgv = new char*[2];

    // Odkomentowane i poprawione, by uniknąć wyrzucania błędu przy delete[] _mockArgv[0]
    std::string name = "nao_bridge";
    _mockArgv[0] = new char[name.size() + 1];
    std::copy(name.begin(), name.end(), _mockArgv[0]);
    _mockArgv[0][name.size()] = '\0';
    _mockArgv[1] = nullptr;

    //_app = std::make_unique<qi::Application>(_mockArgc, _mockArgv);
    
    _messageStream << appName << "Starting." << std::endl;
    _messageCallback(_messageStream.str().c_str(), DataTypes::MessageType::MLog);
    _messageStream.str("");
}

void Broker::init(DataTypes::AudioCallback acb)
{
    try
    {
        std::string command = "ping -n 1 " + _robotIp + " > nul 2>&1";
        if(system(command.c_str()) != 0)
        {
            std::string errorMsg = "Unable to ping " + _robotIp + ".";
            _errorCallback(appName, errorMsg.c_str(), DataTypes::Errorcodes::CannotPing);
            return;
        }

        _messageStream << appName << "Connecting to robot." << std::endl;
        _messageCallback(_messageStream.str().c_str(), DataTypes::MessageType::MLog);
        _messageStream.str("");

        _broker = AL::ALBroker::createBroker(_brokerName, "0.0.0.0", 54000, _robotIp, _robotPort);
        

        _messageStream << appName << "Connected to: " << _robotIp << ":" << _robotPort << std::endl 
                        << appName << "Starting " << SoundModule::moduleName << "." << std::endl;
        _messageCallback(_messageStream.str().c_str(), DataTypes::MessageType::MLog);
        _messageStream.str("");

        _soundModule = AL::ALModule::createModule<SoundModule>(_broker, "SoundModule");
        _soundModule->setCallbacks(acb, moduleMessageCallback);
        
    }
    catch(const AL::ALError& e)
    {
        if (strstr(e.what() , "Cannot connect to") == nullptr)
        {
            _errorCallback(appName, e.what(), DataTypes::Errorcodes::CannotConnect);
            return;
        }

        _messageStream << "CRITICAL AL::ALError: " << e.what();
        _errorCallback(appName, _messageStream.str().c_str(), DataTypes::Errorcodes::UnknownError);
        _messageStream.str("");
    }
    catch(const std::exception& e)
    {
        _messageStream << "CRITICAL ERROR: " << e.what();
        _errorCallback(appName, _messageStream.str().c_str(), DataTypes::Errorcodes::UnknownError);
        _messageStream.str("");
    }
}

void __stdcall Broker::moduleMessageCallback(const char* error, DataTypes::Errorcodes code)
{
    if (gBrokerInstance && gBrokerInstance->_errorCallback)
    {
        gBrokerInstance->_errorCallback(appName, error, code);
    }
}

Broker::~Broker()
{

    _messageStream << appName << "Stopping " << _brokerName << std::endl;
    _messageCallback(_messageStream.str().c_str(), DataTypes::MessageType::MLog);
    _messageStream.str("");

    _messageStream << appName << "Stopping " << SoundModule::moduleName << std::endl;
    _messageCallback(_messageStream.str().c_str(), DataTypes::MessageType::MLog);
    _messageStream.str("");

    _soundModule.reset();

    _messageStream << appName << SoundModule::moduleName << " destroyed." << std::endl;
    _messageCallback(_messageStream.str().c_str(), DataTypes::MessageType::MLog);
    _messageStream.str("");

    _broker.reset();

    delete[] _mockArgv[0];
    delete[] _mockArgv;

    _messageStream << appName << _brokerName << " destroyed." << std::endl;
    _messageCallback(_messageStream.str().c_str(), DataTypes::MessageType::MLog);
    _messageStream.str("");

    if (gBrokerInstance == this) gBrokerInstance = nullptr;
}

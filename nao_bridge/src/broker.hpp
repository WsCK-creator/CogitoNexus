#pragma once

#include "sound_module.hpp"
#include "memory_module.hpp"
#include "./../../dataTypes/dataTypes.hpp"
#include <alcommon/albroker.h>
#include <alerror/alerror.h>
#include <alerror/alnetworkerror.h>
#include <boost/shared_ptr.hpp>
#include <qi/application.hpp>
#include <qi/log.hpp>
#include <thread>
#include <cstring>
#include <string>
#include <cstdlib>
#include <sstream>

using namespace std;


class Broker
{
public:
    
    static constexpr const char* appName = "[NAO broker] ";
    Broker(DataTypes::MessageCallback mcb, DataTypes::ErrorCallback ecb, std::string robotIpValue, int robotPortValue, bool silentQiLog);
    ~Broker();

    void init(DataTypes::AudioCallback acb, DataTypes::DataFromNaoCallback fr_cb);
    static void __stdcall moduleMessageCallback(const char* error, DataTypes::Errorcodes code);
    void sendProcessedData(const char* data);
    void processingEvent();
private:
    const std::string _brokerName = "CogitoNexusBroker";
    const std::string _robotIp;
    const int _robotPort;

    int _mockArgc;
    char** _mockArgv;

    stringstream _messageStream;
    std::unique_ptr<qi::Application> _app;
    boost::shared_ptr<AL::ALBroker> _broker;
    boost::shared_ptr<SoundModule> _soundModule;
    boost::shared_ptr<MemoryModule> _memoryModuel;

    DataTypes::MessageCallback _messageCallback = nullptr;
    DataTypes::ErrorCallback _errorCallback = nullptr;

};

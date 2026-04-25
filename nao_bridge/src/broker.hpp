#pragma once
#include "sound_module.hpp"
#include <alcommon/albroker.h>
#include <boost/shared_ptr.hpp>
#include <qi/application.hpp>
#include <thread>

class Broker
{
private:
    const std::string _brokerName = "CogitoNexusBroker";
    const std::string _robotIp;
    const int _robotPort;

    int _mockArgc;
    char** _mockArgv;

    std::unique_ptr<qi::Application> _app;
    boost::shared_ptr<AL::ALBroker> _broker;
    boost::shared_ptr<SoundModule> _soundModule;
    std::thread _appThread;

    void init();
public:
    const std::string appName = "[NAO broker] ";
    Broker(std::string robotIpValue, int robotPortValue);
    ~Broker();
};

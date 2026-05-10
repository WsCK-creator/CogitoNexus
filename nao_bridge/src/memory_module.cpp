#include "memory_module.hpp"

MemoryModule::MemoryModule(boost::shared_ptr<AL::ALBroker> pBroker, const std::string &pName):
  ALModule(pBroker, pName), fMemoryProxy(getParentBroker())
{
    setModuleDescription("This module allows for conneting to memory.");

    functionName("callback", getName(), "");
    BIND_METHOD(MemoryModule::callback);

    functionName("voidCallback", getName(), "");
    BIND_METHOD(MemoryModule::voidCallback);
}

MemoryModule::~MemoryModule()
{
    fMemoryProxy.unsubscribeToEvent("CogitoNexusBroker/ProcessData", moduleName);
    fMemoryProxy.unsubscribeToEvent("CogitoNexusBroker/Processing", moduleName);
    fMemoryProxy.unsubscribeToEvent("CogitoNexusBroker/ProcessedData", moduleName);
}

void MemoryModule::init()
{
    fMemoryProxy.subscribeToEvent("CogitoNexusBroker/ProcessData", moduleName, "", "callback");
    fMemoryProxy.subscribeToEvent("CogitoNexusBroker/Processing", moduleName, "", "voidCallback");
    fMemoryProxy.subscribeToEvent("CogitoNexusBroker/ProcessedData", moduleName, "", "voidCallback");
}

void MemoryModule::callback(const std::string &key, const AL::ALValue &value, const AL::ALValue &msg)
{
    if(_fr_cb != nullptr && value.isString()) 
    {
        std::string str = value;
        _fr_cb(str.c_str());
    }
}

void MemoryModule::voidCallback(const std::string &key, const AL::ALValue &value, const AL::ALValue &msg)
{
    return;
}

void MemoryModule::sendProcessedData(const char *data)
{
    fMemoryProxy.raiseEvent("CogitoNexusBroker/Processing", "false");
    fMemoryProxy.raiseEvent("CogitoNexusBroker/ProcessedData", data);
}

void MemoryModule::processingEvent()
{
    fMemoryProxy.raiseEvent("CogitoNexusBroker/Processing", "true");
}

void MemoryModule::setCallbacks(DataTypes::DataFromNaoCallback fr_cb)
{
    _fr_cb = fr_cb;
}

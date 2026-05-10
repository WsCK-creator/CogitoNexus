#pragma once

#include <alcommon/almodule.h>
#include <alproxies/almemoryproxy.h>
#include <alvalue/alvalue.h>
#include <alcommon/alproxy.h>

#include "./../../dataTypes/dataTypes.hpp"

class MemoryModule : public AL::ALModule
{
public:
    static constexpr const char* moduleName = "MemoryModule";

    MemoryModule(boost::shared_ptr<AL::ALBroker> pBroker, const std::string& pName);
    virtual ~MemoryModule();
    virtual void init();

    void sendProcessedData(const char* data);
    void processingEvent();
    void callback(const std::string &key, const AL::ALValue &value, const AL::ALValue &msg);
    void voidCallback(const std::string &key, const AL::ALValue &value, const AL::ALValue &msg);
    void setCallbacks(DataTypes::DataFromNaoCallback fr_cb);

private:
    AL::ALMemoryProxy fMemoryProxy;
    DataTypes::DataFromNaoCallback _fr_cb = nullptr;
};

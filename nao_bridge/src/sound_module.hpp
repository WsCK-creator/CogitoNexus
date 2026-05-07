#pragma once
#include <string>
#include <sstream>
#include <alaudio/alsoundextractor.h>
#include <alcommon/alproxy.h>
#include <iostream>
#include "./../../dataTypes/dataTypes.hpp"


class SoundModule : public AL::ALSoundExtractor
{
public:
    typedef void(__stdcall* ModuleErrorCallback)(const char* error, DataTypes::Errorcodes code);

    static constexpr const char* moduleName = "SoundModule";

    SoundModule(boost::shared_ptr<AL::ALBroker> pBroker, const std::string& pName);
    virtual ~SoundModule();
    void process(const int & nbOfChannels, const int & nbrOfSamplesByChannel,
               const AL_SOUND_FORMAT * buffer, const AL::ALValue & timeStamp);
    void setCallbacks(DataTypes::AudioCallback acb, ModuleErrorCallback ecb);
private:
    DataTypes::AudioCallback _audioCallback = nullptr;
    ModuleErrorCallback _errorCallback = nullptr;
};
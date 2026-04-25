#pragma once
#include <string>
#include <alaudio/alsoundextractor.h>
#include <alcommon/alproxy.h>
#include <iostream>


class SoundModule : public AL::ALSoundExtractor
{
private:
public:
    const std::string moduleName = "SoundModule";

    SoundModule(boost::shared_ptr<AL::ALBroker> pBroker, const std::string& pName);
    virtual ~SoundModule();
    void init() override;
    void process(const int & nbOfChannels, const int & nbrOfSamplesByChannel,
               const AL_SOUND_FORMAT * buffer, const AL::ALValue & timeStamp);
};
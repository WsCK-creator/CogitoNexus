#include "sound_module.hpp"


SoundModule::SoundModule(boost::shared_ptr<AL::ALBroker> pBroker, const std::string& pName) 
    : AL::ALSoundExtractor(pBroker, pName)
{ 
    setModuleDescription("This module copys audio data and provides it futher");
}

void SoundModule::init()
{
  audioDevice->callVoid("setClientPreferences", getName(), 16000, (int)AL::FRONTCHANNEL, 0);
  startDetection();
}

SoundModule::~SoundModule()
{
  stopDetection();
}

void SoundModule::process(const int & nbOfChannels,
                                const int & nbOfSamplesByChannel,
                                const AL_SOUND_FORMAT * buffer,
                                const AL::ALValue & timeStamp)
{
 
}

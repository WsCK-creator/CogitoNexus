#include "sound_module.hpp"



SoundModule::SoundModule(boost::shared_ptr<AL::ALBroker> pBroker, const std::string& pName) 
    : AL::ALSoundExtractor(pBroker, pName)
{ 
    setModuleDescription("This module copys audio data and provides it futher");
}

SoundModule::~SoundModule()
{
    if (audioDevice) {
      try {
          stopDetection();
      } catch (...) {}
  }
}

void SoundModule::process(const int & nbOfChannels,
                                const int & nbOfSamplesByChannel,
                                const AL_SOUND_FORMAT * buffer,
                                const AL::ALValue & timeStamp)
{
    if(_audioCallback != nullptr) {_audioCallback(buffer, nbOfSamplesByChannel); }
}

void SoundModule::setCallbacks(DataTypes::AudioCallback acb, ModuleErrorCallback ecb)
{
    _audioCallback = acb;
    _errorCallback = ecb;

    if (audioDevice) {
        try {
            audioDevice->callVoid("setClientPreferences", getName(), 16000, (int)AL::FRONTCHANNEL, 0);
            startDetection();
        } catch (const std::exception& e) {
            if (_errorCallback) {
                std::stringstream ss;
                ss << moduleName << ": Failed to start detection - " << e.what()<< std::endl;
                _errorCallback(ss.str().c_str(), DataTypes::Errorcodes::FaieldToStartAudioTransfer);
            }
        }
    } else {
        if (_errorCallback) {
            std::stringstream ss;
            ss << moduleName << ": ALAudioDevice not found. Audio disabled (Virtual Robot?)" << std::endl;
            _errorCallback(ss.str().c_str(), DataTypes::Errorcodes::NoAlAudioDevice);
        }
    }
}

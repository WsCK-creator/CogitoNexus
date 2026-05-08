#pragma once

#include <vector>
#include <queue>
#include <array>
#include <mutex>
#include <chrono>
#include <iostream>
#include <fstream>
#include <algorithm>

#include "./../../dataTypes/dataTypes.hpp"
#include "rnnoise.h"


class VAD
{
public:
    static constexpr const char* moduleName = "[VAD] ";
    static constexpr const short SAMPLES_16K = 160;
    static constexpr const short SAMPLES_48K = 480;
    
    static void __stdcall audiCallback(const signed short* buffer, int count);

    void setNewAudio(bool b);
    bool getNewAudio();
    void update(DataTypes::VADDataCallback callback, std::atomic<bool>& state);
    VAD();
    ~VAD();

private:
    inline static std::queue<std::vector<signed short>> newAudioQueue;
    inline static std::mutex newAudioMtx;
    inline static unsigned int _newSamplesCount = 0;
    inline static bool newAudioFlag = false;
    
    std::queue<signed short> audioDataToProcess;
    std::vector<std::array<float, SAMPLES_16K>> audioDataProcessed;
    std::vector<float> normalizedAudoData;
    std::vector<float> vdaScore;

    //bool silience = false;
    
    float threshold = 0.75f;
    unsigned int samplesCount = 0;
    unsigned int quietThresholdTime = 150; //one equals to 10ms
    
    DenoiseState* _rnnoise_state;

    //unsigned int _processedDataSets = 0;
    void _moveAudtioToProcessing();
    void _convertToFloat();
    void _getVadAndNormalize();

    void _exportToWav(const std::string& filename);
};
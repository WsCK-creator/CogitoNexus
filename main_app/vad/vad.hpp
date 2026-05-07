#pragma once

#ifdef VAD_EXPORTS
    #define VAD_API __declspec(dllexport)
#else
    #define VAD_API __declspec(dllimport)
#endif

#include <vector>
#include <queue>
#include <array>
#include <mutex>

#include "./../../dataTypes/dataTypes.hpp"
#include "rnnoise.h"


class VAD_API VAD
{
public:
    static constexpr const short SAMPLES_16K = 160;
    static constexpr const short SAMPLES_48K = 480;
    
    static void __stdcall audiCallback(const signed short* buffer, int count);

    void update();
    VAD();
    ~VAD();

private:
    static std::queue<std::vector<unsigned short>> newAudioQueue;
    static std::mutex newAudioMtx;
    static unsigned int _newSamplesCount;
    static bool getNewAudio;
    
    std::queue<unsigned short> audioDataToProcess;
    std::vector<std::array<float, SAMPLES_16K>> audioDataProcessed;
    std::vector<float> normalizedAudoData;
    std::vector<float> vdaScore;

    bool silience = false;
    
    float threshold = 0.6f;
    std::chrono::steady_clock::time_point elapsedTime;
    std::chrono::milliseconds time = std::chrono::milliseconds(1500);
    
    DenoiseState* _rnnoise_state;

    //unsigned int _processedDataSets = 0;
    
    void _moveAudtioToProcessing();
    void _convertToFloat();
    void _getVadAndNormalize();
};
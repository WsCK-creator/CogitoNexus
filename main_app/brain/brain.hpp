#pragma once

#include <iostream>
#include <windows.h>
#include <direct.h>

#include <thread>
#include <atomic>
#include <chrono>

#include <vector>
#include <queue>
#include <mutex>

#include <fstream>
#include <cstdint>

#include "./../../dataTypes/dataTypes.hpp"
#include "vad.hpp"

class Brain
{
private:
    static constexpr const char* moduleName = "[Brain] ";

    inline static DataTypes::BridgeInitFunc _bridgeInitFunc;
    inline static DataTypes::BridgeStopFunc _bridgeStopFunc;

    static void __stdcall _messageCallback(const char* str, DataTypes::MessageType type);
    static void __stdcall _errorCallback(const char* module, const char* error, DataTypes::Errorcodes code);
    static void __stdcall audiCallback(std::vector<float> normalizedData);
    
    std::atomic<bool> _bridgeState{false};
    std::atomic<bool> _vadState{false};
    std::thread _naoBridgeThread;
    std::thread _vadThread;
    HMODULE _hBridge = nullptr;
   
    inline static std::unique_ptr<VAD> _vad = nullptr;
    //inline static std::chrono::steady_clock::time_point audioTime;
    inline static std::queue<std::vector<unsigned short>> audioQueue;
    inline static std::mutex mtx;
    inline static unsigned int totalSamplesCount = 0;

    bool _init();
    bool _loadDLL();
    static void _startNaoBridge(std::atomic<bool>& state);
    static void _stopNaoBridge();
    void saveQueueToWav(std::string filename, std::queue<std::vector<unsigned short>> audioQueue, unsigned int totalSamplesCount);
    void _loop();
public:
    Brain(/* args */);
    ~Brain();
};

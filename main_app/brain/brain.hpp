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

    static DataTypes::BridgeInitFunc _bridgeInitFunc;
    static DataTypes::BridgeStopFunc _bridgeStopFunc;

    static void __stdcall _messageCallback(const char* str, DataTypes::MessageType type);
    static void __stdcall _errorCallback(const char* module, const char* error, DataTypes::Errorcodes code);

    
    std::atomic<bool> _bridgeState{false};
    std::thread _naoBridgeThread;
    HMODULE _hBridge = nullptr;
   
    static std::unique_ptr<VAD> _vad;
    static std::chrono::steady_clock::time_point audioTime;
    static std::queue<std::vector<unsigned short>> audioQueue;
    static std::mutex mtx;
    static unsigned int totalSamplesCount;

    bool _init();
    bool _loadDLL();
    static void _startNaoBridge(std::atomic<bool>& state);
    static void _stopNaoBridge(std::atomic<bool>& state);
    void saveQueueToWav(std::string filename, std::queue<std::vector<unsigned short>> audioQueue, unsigned int totalSamplesCount);
    void _loop();
public:
    Brain(/* args */);
    ~Brain();
};

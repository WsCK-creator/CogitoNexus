#pragma once

#include <iostream>
#include <conio.h>
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
#include "whisper-wrapper.hpp"
#include "LLM.hpp"

class Brain
{
private:
    static constexpr const char* moduleName = "[Brain] ";

    inline static DataTypes::BridgeInitFunc _bridgeInitFunc;
    inline static DataTypes::BridgeStopFunc _bridgeStopFunc;
    inline static DataTypes::DataToNaoFunc _dataToNaoFunc;
    inline static DataTypes::ProcessingStartedFunc _procesingStartedFunc;

    static void __stdcall _messageCallback(const char* str, DataTypes::MessageType type);
    static void __stdcall _errorCallback(const char* module, const char* error, DataTypes::Errorcodes code);
    static void __stdcall _fromNaoGetAudio(const char* data);
    static void __stdcall _vadCallback(std::vector<float> normalizedData);
    //static void __stdcall audiCallback(std::vector<float> normalizedData);
    
    std::atomic<bool> _bridgeState{false};
    inline static std::atomic<bool> _vadState{false};
    std::atomic<bool> _llmState{false};
    std::thread _naoBridgeThread;
    inline static std::thread _vadThread;
    std::thread _llmThread;
    HMODULE _hBridge = nullptr;
   
    inline static std::unique_ptr<VAD> _vad = nullptr;
    inline static std::unique_ptr<WhisperWrapper> _whisper = nullptr;
    inline static std::unique_ptr<LLM> _llm = nullptr;
    inline static std::queue<std::vector<unsigned short>> audioQueue;
    inline static std::mutex mtx;
    inline static unsigned int totalSamplesCount = 0;
    inline static std::string _dataFromNao = "";

    inline static std::chrono::time_point<std::chrono::steady_clock> start_time;

    bool _init();
    bool _loadDLL();
    static void _startNaoBridge(std::atomic<bool>& state);
    static void _stopNaoBridge();
    static void _whenWhisperFinished(std::atomic<bool>& state);
    void _loop();
public:
    Brain(/* args */);
    ~Brain();
};

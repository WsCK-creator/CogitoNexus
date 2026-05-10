#pragma once
#include <string>
#include <vector>
#include <iostream>
#include <atomic>

#include "llama.h"
#include "./../../dataTypes/dataTypes.hpp"

class LLM
{
public:
    static constexpr const char* moduleName = "[LLM] ";

    LLM(const std::string& model_path);
    ~LLM();

    void generateResponse(const std::string &sensorData, std::atomic<bool>& state, bool newChat = false);
    std::string getLastJsonResponse() const { return lastJsonResponse; }
    
private:
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;

    std::string systemPrompt;
    std::string chatHistory;
    std::string lastJsonResponse;
    
    std::string stripThoughts(const std::string& rawResponse);

};


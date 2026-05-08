#pragma once

#include <vector>
#include <string>
#include <iostream>

#include "./../../dataTypes/dataTypes.hpp"
#include "whisper.h"

struct whisper_context;

class WhisperWrapper
{
public:

    static constexpr const char* moduleName = "[Whisper] ";
    inline static bool finished = false;

    static void __stdcall audiCallback(std::vector<float> normalizedData);

    explicit WhisperWrapper(const std::string& model_path);
    ~WhisperWrapper();

    WhisperWrapper(const WhisperWrapper&) = delete;
    WhisperWrapper& operator=(const WhisperWrapper&) = delete;

    std::string getText();
    

private:
    inline static whisper_context* ctx = nullptr;
    inline static std::string data = "";
};
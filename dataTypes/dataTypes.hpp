#pragma once

#include <cstdint>

namespace DataTypes
{
    enum class MessageType: unsigned char{
        MLog = 0,
        MError = 1
    };

    enum class Errorcodes : unsigned char{
        CannotPing = 0,
        CannotConnect = 1,
        NoAlAudioDevice = 2,
        FaieldToStartAudioTransfer = 3,
        UnknownError = 4
    };

    typedef void(__stdcall* ErrorCallback)(const char* module, const char* error, Errorcodes code);
    typedef void(__stdcall* MessageCallback)(const char* str, MessageType type);
    typedef void(__stdcall* AudioCallback)(const signed short* buffer, int count);
    typedef void(__stdcall* VADDataCallback)(std::vector<float> normalizedData);

    typedef void (*BridgeInitFunc)(DataTypes::MessageCallback, DataTypes::ErrorCallback, DataTypes::AudioCallback, const char*, int, bool);
    typedef void (*BridgeStopFunc)();

}

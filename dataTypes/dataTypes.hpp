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

    struct WavHeader {
        // RIFF Chunk
        char riffId[4] = {'R', 'I', 'F', 'F'};
        uint32_t fileSize;          // 4 + (8 + subChunk1Size) + (8 + subChunk2Size)
        char format[4] = {'W', 'A', 'V', 'E'};

        // fmt Sub-chunk
        char subChunk1Id[4] = {'f', 'm', 't', ' '};
        uint32_t subChunk1Size = 16; // Dla PCM
        uint16_t audioFormat = 1;    // 1 = PCM (bez kompresji)
        uint16_t numChannels = 1;    // Mono
        uint32_t sampleRate = 16000;
        uint32_t byteRate;           // SampleRate * NumChannels * BitsPerSample/8
        uint16_t blockAlign;         // NumChannels * BitsPerSample/8
        uint16_t bitsPerSample = 16;

        // data Sub-chunk
        char subChunk2Id[4] = {'d', 'a', 't', 'a'};
        uint32_t subChunk2Size;      // Liczba bajtów danych (totalSamplesCount * 2)
    };
}

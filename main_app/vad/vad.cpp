#include "vad.hpp"

static std::queue<std::vector<unsigned short>> newAudioQueue;
static std::mutex newAudioMtx;
static unsigned int _newSamplesCount = 0;
static bool getNewAudio = false;


void __stdcall VAD::audiCallback(const signed short *buffer, int count)
{
    if(getNewAudio)
    {
        std::vector<unsigned short> local_buffer;
        local_buffer.reserve(3000);
        local_buffer.insert(local_buffer.end(), buffer, buffer + count);
        {
                std::lock_guard<std::mutex> lock(newAudioMtx);
                newAudioQueue.push(std::move(local_buffer));
                _newSamplesCount += count;
        }
    }
}

void VAD::update()
{
    while (true)
    {
        if(getNewAudio)
        {
            _moveAudtioToProcessing();
            _convertToFloat();
            _getVadAndNormalize();
            if(silience && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - elapsedTime) > time)
            {
                getNewAudio = false;
                //TODO: call whisper
            }
        }
    }
}

VAD::VAD()
{
    _rnnoise_state = rnnoise_create(nullptr);
}

VAD::~VAD()
{
    rnnoise_destroy(_rnnoise_state);
}

void VAD::_moveAudtioToProcessing()
{
    if(_newSamplesCount > 0 && newAudioMtx.try_lock())
    {
        while(!newAudioQueue.empty())
        {
            std::vector temp = newAudioQueue.front();
            for (unsigned short i = 0; i < temp.size(); i++)
            {
                audioDataToProcess.push(temp.at(i));
            }
            newAudioQueue.pop();
        }
        _newSamplesCount = 0;
        newAudioMtx.unlock();
    }
}

void VAD::_convertToFloat()
{
    while(audioDataToProcess.size() > 0 && audioDataToProcess.size() % SAMPLES_16K == 0)
    {
        std::array<float, SAMPLES_16K> temp;
        for (unsigned short i = 0; i < SAMPLES_16K; i++)
        {
            temp[i] = static_cast<float>(static_cast<int>(audioDataToProcess.front()) - 32768);
            audioDataToProcess.pop();
        }
        audioDataProcessed.push_back(temp);
    }
}

void VAD::_getVadAndNormalize()
{
    if(audioDataProcessed.size() > 0)
    {
        float temp_out[SAMPLES_48K];
        float temp_in[SAMPLES_48K];
        for (unsigned short i = 0; i < audioDataProcessed.size(); i++)
        {
            for (unsigned short j = 0; j < SAMPLES_16K - 1; j++)
            {
                float d1 = audioDataProcessed.at(i)[j];
                float d2 = audioDataProcessed.at(i)[j + 1];
                normalizedAudoData.push_back(d1 / 32768.0f); // normalization

                temp_in[i * 3 + 0] = d1;
                temp_in[i * 3 + 1] = d1 * 0.666f + d2 * 0.333f;
                temp_in[i * 3 + 2] = d1 * 0.333f + d2 * 0.666f;
            }
            float d = audioDataProcessed.at(i)[SAMPLES_16K - 1];
            normalizedAudoData.push_back(d / 32768.0f); //normalization
            temp_in[SAMPLES_48K - 3] = d;
            temp_in[SAMPLES_48K - 2] = d;
            temp_in[SAMPLES_48K - 1] = d;

            //calculating vda
            vdaScore.at(i) = rnnoise_process_frame(_rnnoise_state, temp_out, temp_in);
            if (!silience && vdaScore.at(i) <= threshold)
            {
                std::chrono::steady_clock::time_point elapsedTime = std::chrono::steady_clock::now();
                silience = true;
            }
            else if (silience && vdaScore.at(i) >= threshold) silience = false;
        }
        audioDataProcessed.clear();
    }
}

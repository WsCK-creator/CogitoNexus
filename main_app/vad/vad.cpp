#include "vad.hpp"

void __stdcall VAD::audiCallback(const signed short *buffer, int count)
{
    if(newAudioFlag)
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

void VAD::setNewAudio(bool b)
{
    if(b && b != newAudioFlag )
    {
        //TODO: Clear all data
        samplesCount = 0;
        std::queue<std::vector<unsigned short>>().swap(newAudioQueue);
        std::queue<unsigned short>().swap(audioDataToProcess);
        audioDataProcessed.clear();
        normalizedAudoData.clear();
        vdaScore.clear();
    }
    newAudioFlag = b;
}

bool VAD::getNewAudio()
{
    return newAudioFlag;
}

void VAD::update(DataTypes::VADDataCallback callback, std::atomic<bool>& state)
{
    while (state.load())
    {
        if(newAudioFlag)
        {
            _moveAudtioToProcessing();
            _convertToFloat();
            _getVadAndNormalize();
            if(samplesCount >= quietThresholdTime)
            {
                newAudioFlag = false;
                for (unsigned int i = 0; i < samplesCount; i++)
                {
                    normalizedAudoData.pop_back();
                    vdaScore.pop_back();
                }
                callback(normalizedAudoData);
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
    //std::cout << moduleName << "Runing move audio:" << _newSamplesCount << std::endl;
    if(_newSamplesCount > 0)
    {
        //std::cout<< moduleName << "New Data, count:" << _newSamplesCount << std::endl;
        if(newAudioMtx.try_lock())
        {
            if(newAudioFlag)
            {
                while(!newAudioQueue.empty())
                {
                    std::vector<unsigned short> temp = newAudioQueue.front();
                    for (unsigned short i = 0; i < temp.size(); i++)
                    {
                        audioDataToProcess.push(temp.at(i));
                    }
                    newAudioQueue.pop();
                }
            }
            else std::queue<std::vector<unsigned short>>().swap(newAudioQueue);

            _newSamplesCount = 0;
            newAudioMtx.unlock();
        }
        //else std::cout<< moduleName << "Data locked ;(" << std::endl;
    }
}

void VAD::_convertToFloat()
{
    //std::cout << moduleName << "Runing convert to float:" << audioDataToProcess.size() << std::endl;
    while(audioDataToProcess.size() > 0 && (static_cast<int>(audioDataToProcess.size()) - SAMPLES_16K) >= 0)
    {
        //std::cout << moduleName << "converting to float" << audioDataToProcess.size() << std::endl;
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
    //std::cout << moduleName << "Runing get VAD" << std::endl;
    if(audioDataProcessed.size() > 0)
    {
        //std::cout<< moduleName << "Processing New Data, size:" << audioDataProcessed.size() << std::endl;
        float temp_out[SAMPLES_48K];
        float temp_in[SAMPLES_48K];
        for (unsigned short i = 0; i < audioDataProcessed.size(); i++)
        {
            for (unsigned short j = 0; j < SAMPLES_16K - 1; j++)
            {
                float d1 = audioDataProcessed.at(i)[j];
                float d2 = audioDataProcessed.at(i)[j + 1];
                normalizedAudoData.push_back(d1 / 32768.0f); // normalization

                temp_in[j * 3 + 0] = d1;
                temp_in[j * 3 + 1] = d1 * 0.666f + d2 * 0.333f;
                temp_in[j * 3 + 2] = d1 * 0.333f + d2 * 0.666f;
            }
            float d = audioDataProcessed.at(i)[SAMPLES_16K - 1];
            normalizedAudoData.push_back(d / 32768.0f); //normalization
            temp_in[SAMPLES_48K - 3] = d;
            temp_in[SAMPLES_48K - 2] = d;
            temp_in[SAMPLES_48K - 1] = d;

            // calculating vad
            float score = rnnoise_process_frame(_rnnoise_state, temp_out, temp_in);
            vdaScore.push_back(score); 
            //std::cout << moduleName << "Procesed data score:" << score << std::endl;

            if (samplesCount == 0 && score <= threshold)
            {
                samplesCount = 1;
                std::cout << moduleName << "Silience detected" << std::endl; 
            }
            else if (samplesCount > 0) 
            {
                if(score >= threshold)
                {
                    samplesCount = 0;
                    std::cout << moduleName << "End of silience" << std::endl; 
                }
                else samplesCount++;
            }
        }
        audioDataProcessed.clear();
    }
}
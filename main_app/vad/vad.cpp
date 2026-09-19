#include "vad.hpp"
#include <filesystem>
#include <ctime>
#include <cmath>

void __stdcall VAD::audiCallback(const signed short *buffer, int count)
{
    // Bufor historii aktualizujemy ZAWSZE, niezależnie od newAudioFlag --
    // to on trzyma ostatnią ~1s audio, z której korzysta setNewAudio(true)
    // jako "pre-roll" w chwili triggera (przycisk na Boosterze / event
    // ProcessData z NAO).
    {
        std::lock_guard<std::mutex> lock(_historyMtx);
        _historyBuffer.insert(_historyBuffer.end(), buffer, buffer + count);
        if (_historyBuffer.size() > PRE_ROLL_SAMPLES) {
            _historyBuffer.erase(_historyBuffer.begin(), _historyBuffer.end() - PRE_ROLL_SAMPLES);
        }
    }

    if(newAudioFlag)
    {
        std::vector<signed short> local_buffer;
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
        samplesCount = 0;
        _speechStarted = false;
        _totalFrameCount = 0;
        _voicedFrameCount = 0;
        std::queue<std::vector<signed short>>().swap(newAudioQueue);
        std::queue<signed short>().swap(audioDataToProcess);
        audioDataProcessed.clear();
        normalizedAudoData.clear();
        vdaScore.clear();

        // Migawka pre-rollu -- surowe próbki sprzed triggera, robimy jej
        // kopię TERAZ, ale CELOWO nie trafia do newAudioQueue/
        // audioDataToProcess: nie ma być przepuszczana przez rnnoise/licznik
        // ciszy-mowy (patrz komentarz przy _prerollSnapshot w vad.hpp).
        // Zostanie doklejona dopiero w update(), tuż przed oddaniem gotowego
        // audio do callbacku.
        {
            std::lock_guard<std::mutex> lock(_historyMtx);
            _prerollSnapshot.assign(_historyBuffer.begin(), _historyBuffer.end());
        }
    }
    newAudioFlag = b;
}

bool VAD::getNewAudio()
{
    return newAudioFlag;
}

void VAD::update(DataTypes::VADDataCallback callback, std::atomic<bool>& state)
{
    std::cout << moduleName << "Starting procesing audio" << std::endl;
    while (state.load())
    {
        if(newAudioFlag)
        {
            _moveAudtioToProcessing();
            _convertToFloat();
            _getVadAndNormalize();
            if(samplesCount >= quietThresholdTime)
            {
                if (_speechStarted && _voicedFrameCount < MIN_VOICED_FRAMES)
                {
                    // Za mało realnej mowy (np. pojedynczy szum/echo, które
                    // na moment przebiło próg) -- ignorujemy i wracamy do
                    // nasłuchu, zamiast oddawać Whisperowi śmieci, na
                    // których "wymyśla" tekst.
                    std::cout << moduleName << "Ignoring false trigger (too little speech: "
                              << _voicedFrameCount << " frames)" << std::endl;
                    samplesCount = 0;
                    _speechStarted = false;
                    _voicedFrameCount = 0;
                    normalizedAudoData.clear();
                    vdaScore.clear();
                    continue;
                }
                setNewAudio(false);
                state.store(false);

                // Doklejamy pre-roll (surowe audio sprzed triggera, BEZ
                // przepuszczania przez VAD/próg mowy-ciszy) na sam POCZĄTEK
                // dopiero teraz, gdy wiadomo, że to realna wypowiedź, a nie
                // fałszywe wyzwolenie odrzucone wyżej.
                if (!_prerollSnapshot.empty())
                {
                    std::vector<float> combined;
                    combined.reserve(_prerollSnapshot.size() + normalizedAudoData.size());
                    for (signed short s : _prerollSnapshot) combined.push_back(s / 32768.0f);
                    combined.insert(combined.end(), normalizedAudoData.begin(), normalizedAudoData.end());
                    normalizedAudoData = std::move(combined);
                    _prerollSnapshot.clear();
                }

                // Wzmocnienie głośności: sygnał (zwłaszcza NAEC z Boostera --
                // redukcja szumu/echa potrafi mocno przytłumić też samą mowę)
                // bywa na tyle cichy, że Whisper ledwo go "słyszy" i źle
                // rozpoznaje słowa. Skalujemy tak, by szczyt amplitudy sięgał
                // ~0.9 (margines przed clippingiem) -- ale TYLKO gdy sygnał
                // faktycznie jest cichszy niż to, żeby nie przycinać nagrań,
                // które już są wystarczająco głośne.
                {
                    float peak = 0.0f;
                    for (float s : normalizedAudoData) peak = std::max(peak, std::fabs(s));
                    constexpr float TARGET_PEAK = 0.9f;
                    if (peak > 0.0001f && peak < TARGET_PEAK)
                    {
                        float gain = TARGET_PEAK / peak;
                        for (float& s : normalizedAudoData) s *= gain;
                        std::cout << moduleName << "Wzmocniono ciche audio, gain=" << gain
                                  << " (peak bylo " << peak << ")" << std::endl;
                    }
                }

                // TYMCZASOWO na potrzeby diagnozy jakości rozpoznawania:
                // zapisz dokładnie to audio (po VAD, z doklejonym
                // pre-rollem), które za chwilę trafi do Whispera, do pliku
                // WAV -- żeby można było je odsłuchać i ocenić, czy problem
                // jest w samym sygnale (np. dalej zniekształcony/cichy) czy
                // gdzieś dalej (Whisper/model/prompt). Plik ląduje w
                // podfolderze "debug_audio" w katalogu roboczym programu
                // (czyli tam, gdzie leży brain.exe -- w dist). Do usunięcia,
                // gdy diagnoza się skończy.
                {
                    std::error_code ec;
                    std::filesystem::create_directories("debug_audio", ec);
                    std::string dbgFile = "debug_audio/rec_" + std::to_string(std::time(nullptr)) + ".wav";
                    _exportToWav(dbgFile);
                    std::cout << moduleName << "Zapisano audio testowe: " << dbgFile << std::endl;
                }

                callback(normalizedAudoData);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

VAD::VAD()
{
    _rnnoise_state = rnnoise_create(nullptr);
}

VAD::~VAD()
{
    rnnoise_destroy(_rnnoise_state);
    std::cout << moduleName << "Module destroyes" << std::endl;
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
                    std::vector<signed short> temp = newAudioQueue.front();
                    for (unsigned short i = 0; i < temp.size(); i++)
                    {
                        audioDataToProcess.push(temp.at(i));
                    }
                    newAudioQueue.pop();
                }
            }
            else std::queue<std::vector<signed short>>().swap(newAudioQueue);

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
            temp[i] = static_cast<float>(audioDataToProcess.front());
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
            _totalFrameCount++;

            if (!_speechStarted)
            {
                // Dopóki użytkownik jeszcze nic nie powiedział, cisza na
                // starcie NIE liczy się do warunku "koniec wypowiedzi" --
                // wcześniej liczyła się od pierwszej ramki, więc po ~1.5s od
                // wciśnięcia przycisku (zanim ktokolwiek zdążył cokolwiek
                // powiedzieć) VAD uznawał wypowiedź za zakończoną i oddawał
                // Whisperowi prawie pustą ramkę (stąd halucynacje typu
                // "Dziękuję"/"Wszystkie prawa zastrzeżone").
                if (score >= threshold)
                {
                    _speechStarted = true;
                    _voicedFrameCount++;
                    std::cout << moduleName << "Speech detected" << std::endl;
                }
                else if (_totalFrameCount >= MAX_RECORDING_FRAMES)
                {
                    // Zabezpieczenie -- mowa nigdy nie została pewnie
                    // wykryta, nie nasłuchujemy w nieskończoność.
                    std::cout << moduleName << "Timeout -- no speech detected" << std::endl;
                    samplesCount = quietThresholdTime;
                }
                continue;
            }

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
                    _voicedFrameCount++;
                    std::cout << moduleName << "End of silience" << std::endl;
                }
                else samplesCount++;
            }
            else if (score >= threshold)
            {
                // samplesCount == 0 i score >= threshold -- dalej trwa
                // mowa bez przerwy, tu też liczymy ją jako "głos".
                _voicedFrameCount++;
            }

            if (_totalFrameCount >= MAX_RECORDING_FRAMES && samplesCount < quietThresholdTime)
            {
                // Zabezpieczenie -- twardy limit długości nagrania, nawet
                // gdyby mowa trwała bez wystarczającej przerwy na końcu.
                std::cout << moduleName << "Timeout -- max recording length reached" << std::endl;
                samplesCount = quietThresholdTime;
            }
        }
        audioDataProcessed.clear();
    }
}

void VAD::_exportToWav(const std::string& filename)
{
    // Otwieramy plik w trybie binarnym
    std::ofstream file(filename, std::ios::binary);
    if (!file)
    {
        std::cerr << moduleName << " Blad: Nie mozna otworzyc pliku do zapisu: " << filename << std::endl;
        return;
    }

    uint32_t sample_rate = 16000; // Z Twojego kodu wynika, że to 16 kHz
    uint16_t num_channels = 1;    // Mono
    uint16_t bits_per_sample = 16;
    
    // Obliczamy rozmiary dla nagłówka WAV
    uint32_t data_size = normalizedAudoData.size() * sizeof(int16_t);
    uint32_t chunk_size = 36 + data_size;
    uint32_t byte_rate = sample_rate * num_channels * sizeof(int16_t);
    uint16_t block_align = num_channels * sizeof(int16_t);

    // 1. Zapis subchunka RIFF
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&chunk_size), 4);
    file.write("WAVE", 4);

    // 2. Zapis subchunka fmt (format)
    file.write("fmt ", 4);
    uint32_t subchunk1_size = 16;
    uint16_t audio_format = 1; // 1 oznacza nieskompresowane PCM
    file.write(reinterpret_cast<const char*>(&subchunk1_size), 4);
    file.write(reinterpret_cast<const char*>(&audio_format), 2);
    file.write(reinterpret_cast<const char*>(&num_channels), 2);
    file.write(reinterpret_cast<const char*>(&sample_rate), 4);
    file.write(reinterpret_cast<const char*>(&byte_rate), 4);
    file.write(reinterpret_cast<const char*>(&block_align), 2);
    file.write(reinterpret_cast<const char*>(&bits_per_sample), 2);

    // 3. Zapis subchunka data (dane audio)
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&data_size), 4);

    // 4. Konwersja z float [-1.0, 1.0] z powrotem na int16_t i zapis do pliku
    for (float sample : normalizedAudoData)
    {
        // Zabezpieczenie (clamping), by nie przekroczyć zakresu int16_t
        int16_t s = static_cast<int16_t>(std::max<float>(-32768.0f, std::min<float>(32767.0f, sample * 32767.0f)));
        file.write(reinterpret_cast<const char*>(&s), 2);
    }

    file.close();
    std::cout << moduleName << " Zapisano plik WAV (" << normalizedAudoData.size() << " probek): " << filename << std::endl;
}

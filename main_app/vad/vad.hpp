#pragma once

#include <vector>
#include <queue>
#include <deque>
#include <array>
#include <mutex>
#include <chrono>
#include <iostream>
#include <fstream>
#include <algorithm>

#include "./../../dataTypes/dataTypes.hpp"
#include "rnnoise.h"


class VAD
{
public:
    static constexpr const char* moduleName = "[VAD] ";
    static constexpr const short SAMPLES_16K = 160;
    static constexpr const short SAMPLES_48K = 480;
    
    static void __stdcall audiCallback(const signed short* buffer, int count);

    void setNewAudio(bool b); 
    bool getNewAudio();
    void update(DataTypes::VADDataCallback callback, std::atomic<bool>& state);
    VAD();
    ~VAD();

private:
    inline static std::queue<std::vector<signed short>> newAudioQueue;
    inline static std::mutex newAudioMtx;
    inline static unsigned int _newSamplesCount = 0;
    inline static bool newAudioFlag = false;

    // Bufor "pre-roll": ostatnia ~1s surowego audio, zapisywana CIĄGLE
    // (niezależnie od newAudioFlag) w audiCallback -- zarówno NAO jak i
    // Booster wołają dokładnie ten sam audiCallback, więc to jedno miejsce
    // pokrywa oba roboty naraz. Bez tego pierwsze słowo wypowiedziane w
    // momencie/tuż przed wciśnięciem przycisku bywało ucinane, zanim VAD
    // zdążył zacząć nasłuch.
    static constexpr unsigned int PRE_ROLL_SAMPLES = 16000; // 1s przy 16kHz
    inline static std::deque<signed short> _historyBuffer;
    inline static std::mutex _historyMtx;

    // Migawka bufora historii zrobiona w chwili triggera (setNewAudio(true)).
    // CELOWO nie jest wpuszczana do normalnego potoku VAD
    // (audioDataToProcess/rnnoise/licznik ciszy-mowy) -- gdy było to zrobione
    // tak (poprzednia wersja), cisza/szum z KOŃCA pre-rollu (sprzed samego
    // triggera) wliczały się w licznik ciszy "koniec wypowiedzi" i potrafiły
    // od razu przekroczyć quietThresholdTime, zanim użytkownik w ogóle zdążył
    // cokolwiek powiedzieć -- stąd przedwczesne kończenie nasłuchu. Zamiast
    // tego pre-roll jest doklejany jako gotowe, znormalizowane próbki na sam
    // POCZĄTEK dopiero PO tym, jak VAD (licząc wyłącznie na żywym audio)
    // uzna wypowiedź za zakończoną -- patrz update().
    inline static std::vector<signed short> _prerollSnapshot;

    std::queue<signed short> audioDataToProcess;
    std::vector<std::array<float, SAMPLES_16K>> audioDataProcessed;
    std::vector<float> normalizedAudoData;
    std::vector<float> vdaScore;

    //bool silience = false;

    // UWAGA na wartość progu: rnnoise_process_frame zwraca prawdopodobieństwo
    // mowy w ramce (0..1). Przy 0.98 niemal ŻADNA pojedyncza ramka -- nawet
    // w środku głośnego, ciągłego mówienia -- nie osiąga go powtarzalnie w
    // kolejnych ramkach, więc licznik ciszy (samplesCount), który resetuje
    // się TYLKO gdy jakaś ramka znów przekroczy próg, praktycznie nigdy się
    // nie zerował i po prostu leciał do 150 (czyli ~1.5s od pierwszej ramki
    // mowy) NIEZALEŻNIE od tego, czy użytkownik dalej mówił -- stąd ucinanie
    // w środku wyrazu/zdania. 0.5 to bardziej realistyczny próg "czy w tej
    // ramce jest mowa"; może wymagać dostrojenia pod konkretny mikrofon/
    // otoczenie.
    float threshold = 0.98f;
    unsigned int samplesCount = 0;
    unsigned int quietThresholdTime = 150; //one equals to 10ms

    // Bez tego VAD liczył ciszę na START nagrania (zanim użytkownik w ogóle
    // zaczął mówić) jako "koniec wypowiedzi" i kończył nasłuch po ~1.5s od
    // wciśnięcia przycisku, zanim padło jedno słowo -- stąd Whisper dostawał
    // prawie pustą/cichą ramkę i halucynował krótkie frazy ("Dziękuję",
    // "Wszystkie prawa zastrzeżone" itp.). Teraz odliczanie ciszy do końca
    // wypowiedzi startuje dopiero PO wykryciu pierwszej realnej mowy.
    bool _speechStarted = false;
    unsigned int _totalFrameCount = 0;
    // Zabezpieczenie: jeśli mowa nigdy nie zostanie pewnie wykryta (np. za
    // cichy mikrofon / zbyt wysoki próg), nie nasłuchujemy w nieskończoność.
    static constexpr unsigned int MAX_RECORDING_FRAMES = 1500; //15s (one frame = 10ms)

    // Ile ramek z rzędu/łącznie faktycznie przekroczyło próg mowy w trakcie
    // bieżącej próby nasłuchu -- pojedynczy szum (silnik, trzask, echo
    // głośnika) też potrafi na moment przebić próg. Jeśli po zakończeniu
    // "wypowiedzi" okaże się, że realnej mowy było za mało, traktujemy to
    // jako fałszywe wyzwolenie i wracamy do nasłuchu zamiast oddawać
    // Whisperowi szum, na którym on "wymyśla" tekst.
    unsigned int _voicedFrameCount = 0;
    static constexpr unsigned int MIN_VOICED_FRAMES = 15; //150ms realnej mowy

    DenoiseState* _rnnoise_state;

    //unsigned int _processedDataSets = 0;
    void _moveAudtioToProcessing();
    void _convertToFloat();
    void _getVadAndNormalize();

    void _exportToWav(const std::string& filename);
};
#include "whisper-wrapper.hpp"

void __stdcall WhisperWrapper::audiCallback(std::vector<float> normalizedData)
{
    std::cout << moduleName << "Converting audio" << std::endl;
    if(normalizedData.empty()) return;

    if (ctx == nullptr) {
        std::cerr << moduleName << "Model not loaded!" << std::endl;
        return ;
    }

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.language         = "pl";
    wparams.print_progress   = false;
    wparams.print_timestamps = false;
    //wparams.single_segment   = true;
    wparams.n_threads = 12;

    if (whisper_full(ctx, wparams, normalizedData.data(), normalizedData.size()) != 0) {
        std::cerr << moduleName << "Error during inference!" << std::endl;
        return;
    }

    std::string result = "";
    int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text(ctx, i);
        result += text;
    }
    data = result;
    std::cout << moduleName << "Recognized: " << data << std::endl;
    finished = true;
}

WhisperWrapper::WhisperWrapper(const std::string &model_path)
{
    struct whisper_context_params cparams = whisper_context_default_params();
    
    // Loading Model    
    ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    
    if (ctx == nullptr) std::cerr << moduleName << "Error: Unable to load whisper model form: " << model_path << std::endl;
    else std::cout << moduleName << "Whisper model loaded correctly." << std::endl;
}

WhisperWrapper::~WhisperWrapper()
{
    if (ctx != nullptr) whisper_free(ctx);
    std::cout << moduleName << "Module destroyed" << std::endl;
}

std::string WhisperWrapper::getText()
{
    return data;
}

#include <iostream>
#include <string>
#include <windows.h>
#include <direct.h>
#include <thread>
#include <chrono>
#include "brain.hpp"
#include "LLM.hpp"


int main() {
    system("chcp 65001");
    /*std::cout << "[SYSTEM] Ladowanie modelu do pamieci...\n";
    LLM llm("models/LLM/gemma-4-E4B-it-UD-Q4_K_XL.gguf");

    // ========================================================================
    // MESSAGE 1: Starting the conversation
    // ========================================================================
    std::string state1 = 
        "Wydarzenie: Zabawa w pokoju\n"
        "Bateria: 80%\n"
        "Wykryto twarz: TAK\n"
        "=== SŁOWA UŻYTKOWNIKA (MIKROFON) ===\n"
        "\"Cześć Nao! Wymyśl dla mnie jakieś śmieszne imię, proszę.\"";
    
    std::cout << "\n[--- SENDING MESSAGE 1 (New context implicitly) ---]" << std::endl;
    // Default parameter is false, but since it's the first call, it creates history
    llm.generateResponse(state1);
    std::string response1 = llm.getLastJsonResponse();
    std::cout << response1 << std::endl;


    // ========================================================================
    // MESSAGE 2: Continuing the chat (No reset)
    // ========================================================================
    std::string state2 = 
        "Wydarzenie: Zabawa w pokoju\n"
        "Bateria: 79%\n"
        "Wykryto twarz: TAK\n"
        "=== SŁOWA UŻYTKOWNIKA (MIKROFON) ===\n"
        "\"Hahaha, świetne! A potrafisz to przeliterować?\"";
    
    std::cout << "\n[--- SENDING MESSAGE 2 (Continuing the conversation) ---]" << std::endl;
    // newChat = false (default). The robot remembers the joke/name from Message 1.
    llm.generateResponse(state2);
    std::string response2 = llm.getLastJsonResponse();
    std::cout << response2 << std::endl;


    // ========================================================================
    // MESSAGE 3: Hard reset (Wiping memory)
    // ========================================================================
    std::string state3 = 
        "Wydarzenie: Prezentacja przed nowymi ludźmi\n"
        "Bateria: 78%\n"
        "Wykryto twarz: TAK\n"
        "=== SŁOWA UŻYTKOWNIKA (MIKROFON) ===\n"
        "\"Przedstaw się ładnie, kim jesteś?\"";
    
    std::cout << "\n[--- SENDING MESSAGE 3 (HARD RESET - Wiping Memory) ---]" << std::endl;
    // newChat = true. The robot completely forgets Messages 1 and 2.
    llm.generateResponse(state3, true);
    std::string response3 = llm.getLastJsonResponse();
    std::cout << response3 << std::endl;


    std::cout << "\n[SYSTEM] Test completed successfully." << std::endl;*/
    Brain brain;
    return 0;
}
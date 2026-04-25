#include <iostream>
#include <windows.h>
#include <direct.h>

int main() {
    std::cout << "--- CogitoNexus: Main Brain Starting (C++20) ---" << std::endl;

    char cwd[1024];
    if (_getcwd(cwd, sizeof(cwd)) != NULL) {
        std::cout << "[INFO] Szukam DLL w: " << cwd << std::endl;
    }

    const char* dllPath = "nao_bridge.dll";
    HMODULE hBridge = LoadLibraryA(dllPath);

    if (!hBridge) {
        std::cerr << "[ERROR] Nie mozna zaladowac: " << dllPath << std::endl;
        std::cerr << "Kod bledu Windows: " << GetLastError() << std::endl;
        std::cin.get();
        return 1;
    }

    std::cout << "[SUCCESS] Most nao_bridge.dll zaladowany poprawnie!\n" << std::endl;

    // --- NOWA CZĘŚĆ: URUCHAMIANIE FUNKCJI ---
    
    // 1. Definiujemy "typ" naszej funkcji (dopasowany do DLL: przyjmuje IP i Port)
    typedef void (*InitFunc)(const char*, int);

    // 2. Szukamy jej wewnątrz biblioteki DLL
    InitFunc initRobot = (InitFunc)GetProcAddress(hBridge, "nao_bridge_init");

    // 3. Sprawdzamy, czy system ją znalazł
    if (!initRobot) {
        std::cerr << "[ERROR] Nie znaleziono funkcji 'nao_bridge_init'! Blad: " << GetLastError() << std::endl;
    } else {
        // 4. FAKTYCZNE URUCHOMIENIE FUNKCJI Z MOSTU
        initRobot("127.0.0.1", 9559); // Wpisz właściwe IP i port Twojego robota NAO!
    }

    // ----------------------------------------

    FreeLibrary(hBridge);
    
    std::cout << "\nProgram dziala! Nacisnij ENTER, aby zakonczyc..." << std::endl;
    std::cin.get();
    
    return 0;
}
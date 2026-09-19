// WIN32_LEAN_AND_MEAN musi być zdefiniowane PRZED pierwszym włączeniem
// <windows.h> w tej jednostce translacji -- inaczej windows.h ciągnie za
// sobą stary <winsock.h>, a gdy BoosterBridge.hpp (dołączane przez
// brain.hpp) później włącza <winsock2.h>/<ws2tcpip.h>, powstaje konflikt
// deklaracji (błędy "redefinition; different linkage" w winsock2.h).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <iostream>
#include <string>
#include <windows.h>
#include <direct.h>
#include <thread>
#include <chrono>
#include <limits>
#include "brain.hpp"
#include "LLM.hpp"
#include "./../../dataTypes/dataTypes.hpp"

void constrain(unsigned char max, unsigned char min, unsigned char* input){
    if(*input > max) *input = max;
    if(*input < min) *input = min;
}
bool checkIP(std::string ip){
    unsigned char numberCount = 0;
    unsigned char dotCount = 0;
    for (unsigned char i = 0; i < ip.length(); i++)
    {
        if(ip[i] >= 0x30 && ip[i] <= 0x39) numberCount++;
        else if(ip[i] != 0x2E) return false;
        else {
            if(numberCount == 0 || numberCount > 3) return false;
            numberCount = 0;
            dotCount++;
        }
    }
    if(dotCount != 3) return false;
    
    return true;
}

int main() {
    unsigned char robot = 0;;
    std::string temp;
    std::string robotIp;
    std::string situation;
    system("chcp 65001");
    std::cout << "---------------------- Witaj w CogitoNexus ----------------------";
    do
    {
        std::cout << "Wybierz robota: \n1 - NAO\n2 - Booster\n3 - Trumna\n";
        std::cin >> temp;
        try { robot = std::stoi(temp); }
        catch(const std::exception& e){}
        
    } while (robot == 0);
    
    DataTypes::RobotType robotType = DataTypes::RobotType(robot);

    // Nowa opcja robota: głośność TTS i długość wypowiedzi (length_scale) --
    // dotyczy WYŁĄCZNIE Boostera (NAO/Trumna tego nie obsługują). Puste pole
    // (samo Enter) oznacza "bez zmian" -- wartość zostaje -1.0 i Brain w
    // ogóle nie wysyła wtedy komendy "settings" do robota.
    double boosterVolume = -1.0;
    double boosterLengthScale = -1.0;
    if (robotType == DataTypes::RobotType::Booster)
    {
        // std::cin >> temp (wybór robota) zostawia w buforze niewczytany
        // znak nowej linii -- trzeba go pominąć, inaczej pierwszy getline
        // poniżej od razu "przeczyta" pusty wiersz zamiast czekać na
        // odpowiedź użytkownika.
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        std::string line;
        std::cout << "Glosnosc TTS (0.0-1.0+, Enter = bez zmian): ";
        std::getline(std::cin, line);
        if (!line.empty()) {
            try { boosterVolume = std::stod(line); }
            catch (const std::exception&) {
                std::cout << "Nieprawidlowa wartosc, pomijam zmiane glosnosci.\n";
                boosterVolume = -1.0;
            }
        }

        std::cout << "Dlugosc wypowiedzi / length_scale (1.0 = domyslnie, >1 wolniej, Enter = bez zmian): ";
        std::getline(std::cin, line);
        if (!line.empty()) {
            try { boosterLengthScale = std::stod(line); }
            catch (const std::exception&) {
                std::cout << "Nieprawidlowa wartosc, pomijam zmiane dlugosci wypowiedzi.\n";
                boosterLengthScale = -1.0;
            }
        }
    }

    constrain(3, 1, &robot);
    do{
        std::cout << "\nPodaj adres ip: ";
        std::cin >> robotIp;
    }
    while (!checkIP(robotIp));
    std::cout << "Podaj kontekst sytuacji/wydarzenia (aby zakonczyc, wcisnij Ctrl+Z w nowej linii i Enter):\n";

    while (std::getline(std::cin, temp)) {
        situation += temp + "\n";
    }

    Brain brain(robotType, robotIp, situation, boosterVolume, boosterLengthScale);
    brain.run();
    return 0;
}
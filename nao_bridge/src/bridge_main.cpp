#include <iostream>
#include <memory>
#include "broker.hpp"

std::unique_ptr<Broker> _naoBroker;


extern "C" {
    
    __declspec(dllexport) void nao_bridge_init(DataTypes::MessageCallback mcb, DataTypes::ErrorCallback ecb, DataTypes::AudioCallback acb, const char* robotIp, int robotPort, bool log) {
        _naoBroker = std::make_unique<Broker>(mcb, ecb, acb, robotIp, robotPort, !log);
        _naoBroker->init(acb);
    }
    __declspec(dllexport) void nao_bridge_stop() {
        _naoBroker.reset();
    }
}
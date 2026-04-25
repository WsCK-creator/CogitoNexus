#include <iostream>
#include <memory>
#include "broker.hpp"

std::unique_ptr<Broker> _naoBroker;

extern "C" {
    
    __declspec(dllexport) void nao_bridge_init(const char* robotIp, int robotPort) {
        _naoBroker = std::make_unique<Broker>(robotIp, robotPort);
    }
    __declspec(dllexport) void nao_bridge_stop() {
        _naoBroker.reset();
    }
}
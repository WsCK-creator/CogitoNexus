#include <iostream>
#include <memory>
#include "broker.hpp"

std::unique_ptr<Broker> _naoBroker;


extern "C" {
    
    __declspec(dllexport) void nao_bridge_init(DataTypes::MessageCallback mcb, DataTypes::ErrorCallback ecb, DataTypes::AudioCallback acb, DataTypes::DataFromNaoCallback fr_cb, const char* robotIp, int robotPort, bool log) {
        _naoBroker = std::make_unique<Broker>(mcb, ecb, robotIp, robotPort, !log);
        _naoBroker->init(acb, fr_cb);
    }
    __declspec(dllexport) void nao_bridge_stop() {
        _naoBroker.reset();
    }
    __declspec(dllexport) void send_to_nao(const char* data)
    {
        _naoBroker->sendProcessedData(data);
    }
    __declspec(dllexport) void processing_started()
    {
        _naoBroker->processingEvent();
    }
}
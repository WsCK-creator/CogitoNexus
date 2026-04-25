cmake_minimum_required(VERSION 2.8)
project(nao_bridge)
find_package(qibuild)

qi_create_lib(nao_bridge SHARED 
src/bridge_main.cpp
src/broker.cpp
src/sound_module.cpp
)

qi_use_lib(nao_bridge ALCOMMON ALPROXIES ALAUDIO QI)
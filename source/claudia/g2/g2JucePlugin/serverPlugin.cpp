// ReSharper disable once CppUnusedIncludeDirective
#include "client/plugin.h"

#include "g2Device.h"

synthLib::Device* createBridgeDevice(const synthLib::DeviceCreateParams& _params) { return new g2::Device(_params); }

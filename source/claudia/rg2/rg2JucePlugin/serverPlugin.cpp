// ReSharper disable once CppUnusedIncludeDirective
#include "client/plugin.h"

#include "rg2Device.h"

synthLib::Device* createBridgeDevice(const synthLib::DeviceCreateParams& _params) { return new rg2::Device(_params); }

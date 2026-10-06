#pragma once

#include "jucePluginLib/controller.h"

namespace rg2JucePlugin
{
    class AudioPluginAudioProcessor;

    class Controller : public pluginLib::Controller
    {
    public:
        explicit Controller(AudioPluginAudioProcessor& _processor);
        ~Controller() override;

        void onStateLoaded() override;

        uint8_t getPartCount() const override { return 4; }

        void sendParameterChange(const pluginLib::Parameter& _parameter, pluginLib::ParamValue _value,
                                 pluginLib::Parameter::Origin _origin) override;

        bool parseSysexMessage(const pluginLib::SysEx& _msg, synthLib::MidiEventSource _source) override;

        std::vector<uint8_t> getPartsForMidiChannel(uint8_t _channel) override;
    };
} // namespace rg2JucePlugin

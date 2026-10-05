#include "g2Controller.h"

#include "g2PluginProcessor.h"

namespace g2JucePlugin
{
    Controller::Controller(AudioPluginAudioProcessor& _processor) :
        pluginLib::Controller(_processor, "parameterDescriptions_g2.json")
    {
        registerParams(_processor,
                       [](const uint8_t _part, const bool _isNonPartExclusive)
                       {
                           if (_isNonPartExclusive)
                               return juce::String();
                           const char temp[2] = {static_cast<char>('A' + _part), 0};
                           return juce::String(temp);
                       });

        Controller::onStateLoaded();
    }

    Controller::~Controller() = default;

    void Controller::onStateLoaded() {}

    void Controller::sendParameterChange(const pluginLib::Parameter& /*_parameter*/, pluginLib::ParamValue /*_value*/,
                                         pluginLib::Parameter::Origin /*_origin*/)
    {
    }

    bool Controller::parseSysexMessage(const pluginLib::SysEx& /*_msg*/, synthLib::MidiEventSource /*_source*/)
    {
        return false;
    }

    std::vector<uint8_t> Controller::getPartsForMidiChannel(const uint8_t _channel)
    {
        if (_channel < getPartCount())
            return {_channel};
        return {};
    }
} // namespace g2JucePlugin

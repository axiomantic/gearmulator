#include "g2Controller.h"

#include "g2Device.h"
#include "g2PluginProcessor.h"

#include <algorithm>

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

    void Controller::sendParameterChange(const pluginLib::Parameter& _parameter, const pluginLib::ParamValue _value,
                                         const pluginLib::Parameter::Origin /*_origin*/)
    {
        auto* g2Proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
        if (!g2Proc)
            return;
        auto* dev = g2Proc->getG2Device();
        if (!dev)
            return;

        const auto idx = _parameter.getDescription().index;
        const float norm = std::clamp(static_cast<float>(_value) / 127.0f, 0.0f, 1.0f);
        switch (idx)
        {
        case 0:
            dev->setPanelControl(g2::PanelControl::MasterVolume, norm);
            break;
        case 1:
            dev->setPanelControl(g2::PanelControl::ModWheel, norm);
            break;
        case 2:
            dev->setPanelControl(g2::PanelControl::PitchStick, norm);
            break;
        case 3:
            dev->setPanelControl(g2::PanelControl::Aftertouch, norm);
            break;
        case 4:
            dev->setPanelControl(g2::PanelControl::ControlPedal, norm);
            break;
        default:
            break;
        }
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

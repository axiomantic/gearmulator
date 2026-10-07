#include "rg2Controller.h"

#include "rg2Device.h"
#include "rg2PluginProcessor.h"

#include <algorithm>

namespace rg2JucePlugin
{
	Controller::Controller(AudioPluginAudioProcessor& _processor) :
		pluginLib::Controller(_processor, "parameterDescriptions_rg2.json")
	{
		registerParams(_processor,
					   [](const uint8_t _part, const bool _isNonPartExclusive)
					   {
						   if (_isNonPartExclusive)
							   return juce::String();
						   const char temp[2] = {static_cast<char>('A' + _part), 0};
						   return juce::String(temp);
					   });

		for (uint8_t i = 0; i < 8; ++i)
		{
			const std::string name = "Encoder" + std::to_string(i + 1);
			if (auto* param = getParameter(name, 0))
			{
				m_encoderListeners.push_back(std::make_unique<baseLib::EventListener<pluginLib::Parameter*>>(
					param->onValueChanged,
					[this, i](pluginLib::Parameter* _p)
					{
						if (_p)
							handleEncoderChange(i, static_cast<pluginLib::ParamValue>(_p->getUnnormalizedValue()));
					}));
			}
		}

		Controller::onStateLoaded();
	}

	Controller::~Controller() = default;

	void Controller::onStateLoaded()
	{
		for (uint8_t i = 0; i < 8; ++i)
		{
			const std::string name = "Encoder" + std::to_string(i + 1);
			if (auto* param = getParameter(name, 0))
				m_encoderValues[i] = param->getUnnormalizedValue();
		}
	}

	void Controller::handleEncoderChange(const uint8_t _encoderIndex, const pluginLib::ParamValue _value)
	{
		if (_encoderIndex >= 8)
			return;

		const int prev = m_encoderValues[_encoderIndex];
		const int delta = static_cast<int>(_value) - prev;
		m_encoderValues[_encoderIndex] = static_cast<int>(_value);

		if (delta != 0)
		{
			auto* g2Proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
			if (!g2Proc)
				return;
			auto* dev = g2Proc->getG2Device();
			if (!dev)
				return;
			if (auto* board = dev->board())
				board->panel().setEncoderDelta(_encoderIndex, static_cast<int8_t>(std::clamp(delta, -127, 127)));
		}
	}

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
			dev->setPanelControl(rg2::PanelControl::MasterVolume, norm);
			break;
		case 1:
			dev->setPanelControl(rg2::PanelControl::ModWheel, norm);
			break;
		case 2:
			dev->setPanelControl(rg2::PanelControl::PitchStick, norm);
			break;
		case 3:
			dev->setPanelControl(rg2::PanelControl::Aftertouch, norm);
			break;
		case 4:
			dev->setPanelControl(rg2::PanelControl::ControlPedal, norm);
			break;
		case 5:
		case 6:
		case 7:
		case 8:
		case 9:
		case 10:
		case 11:
		case 12:
			handleEncoderChange(static_cast<uint8_t>(idx - 5), _value);
			break;
		default:
			break;
		}
	}

	bool Controller::parseSysexMessage(const pluginLib::SysEx& /*_msg*/, synthLib::MidiEventSource /*_source*/)
	{
		return false;
	}

	bool Controller::parseMidiMessage(const synthLib::SMidiEvent& _e)
	{
		if (!_e.sysex.empty())
			return parseSysexMessage(_e.sysex, _e.source);

		auto* g2Proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		auto* dev = g2Proc ? g2Proc->getG2Device() : nullptr;

		const uint8_t status = _e.a & 0xf0;
		if (status == synthLib::M_PITCHBEND)
		{
			const int pb = static_cast<int>((_e.c << 7) | _e.b);
			const float norm = std::clamp(static_cast<float>(pb) / 16383.0f, 0.0f, 1.0f);
			if (dev)
				dev->setPanelControl(rg2::PanelControl::PitchStick, norm);
			if (auto* p = getParameter("PitchStick", 0))
				p->setValueFromSynth(static_cast<int>(std::round(norm * 127.0f)),
									 midiEventSourceToParameterOrigin(_e.source));
			return true;
		}
		if (status == synthLib::M_AFTERTOUCH)
		{
			const float norm = std::clamp(static_cast<float>(_e.b) / 127.0f, 0.0f, 1.0f);
			if (dev)
				dev->setPanelControl(rg2::PanelControl::Aftertouch, norm);
			if (auto* p = getParameter("Aftertouch", 0))
				p->setValueFromSynth(_e.b, midiEventSourceToParameterOrigin(_e.source));
			return true;
		}
		if (status == synthLib::M_CONTROLCHANGE)
		{
			if (_e.b >= 70 && _e.b <= 77)
			{
				const uint32_t encIdx = _e.b - 70;
				if (auto* p = getParameter(5 + encIdx, 0))
				{
					p->setValueFromSynth(_e.c, midiEventSourceToParameterOrigin(_e.source));
					return true;
				}
			}
		}

		return pluginLib::Controller::parseMidiMessage(_e);
	}

	std::vector<uint8_t> Controller::getPartsForMidiChannel(const uint8_t _channel)
	{
		if (_channel < getPartCount())
			return {_channel};
		return {};
	}
} // namespace rg2JucePlugin

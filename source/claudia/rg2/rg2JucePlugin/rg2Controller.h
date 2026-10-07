#pragma once

#include "jucePluginLib/controller.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

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
		bool parseMidiMessage(const synthLib::SMidiEvent& _e) override;

		std::vector<uint8_t> getPartsForMidiChannel(uint8_t _channel) override;

		void handleEncoderChange(uint8_t _encoderIndex, pluginLib::ParamValue _value);

	private:
		std::array<int, 8> m_encoderValues{64, 64, 64, 64, 64, 64, 64, 64};
		std::vector<std::unique_ptr<baseLib::EventListener<pluginLib::Parameter*>>> m_encoderListeners;
	};
} // namespace rg2JucePlugin

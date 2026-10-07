#pragma once

#include "jucePluginEditorLib/pluginProcessor.h"

namespace rg2
{
	class Device;
}

namespace rg2JucePlugin
{
	class AudioPluginAudioProcessor : public jucePluginEditorLib::Processor
	{
	public:
		AudioPluginAudioProcessor();
		~AudioPluginAudioProcessor() override;

		jucePluginEditorLib::PluginEditorState* createEditorState() override;
		synthLib::Device* createDevice() override;
		void getRemoteDeviceParams(synthLib::DeviceCreateParams& _params) const override;

		pluginLib::Controller* createController() override;

		rg2::Device* getG2Device() const noexcept;

		void setPort(uint16_t port);
		uint16_t getPort() const;
		uint16_t findFreePort();

		void saveChunkData(baseLib::BinaryStream& s) override;
		void loadChunkData(baseLib::ChunkReader& _cr) override;

		void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

	private:
		bool m_wasPlaying = false;
		double m_clockPhase = 0.0;
		int64_t m_lastPpqTick = -1;

		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
	};
} // namespace rg2JucePlugin

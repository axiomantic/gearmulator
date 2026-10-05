#pragma once

#include "jucePluginEditorLib/pluginProcessor.h"

namespace g2
{
	class Device;
}

namespace g2JucePlugin
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

		g2::Device* getG2Device() const noexcept { return m_g2Device; }

	private:
		g2::Device* m_g2Device = nullptr;
		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
	};
} // namespace g2JucePlugin

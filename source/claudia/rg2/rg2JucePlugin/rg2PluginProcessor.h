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

		rg2::Device* getG2Device() const noexcept { return m_g2Device; }

	private:
		rg2::Device* m_g2Device = nullptr;
		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
	};
} // namespace rg2JucePlugin

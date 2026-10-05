#include "rg2PluginProcessor.h"

#include "firmwareState.h"
#include "rg2Controller.h"
#include "rg2Device.h"
#include "rg2Plugin.h"
#include "rg2PluginEditorState.h"

#include "BinaryData.h"
#include "jucePluginLib/processorPropertiesInit.h"
#include "synthLib/deviceException.h"

namespace
{
    juce::PropertiesFile::Options getOptions()
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName = "DSP56300EmulatorNordG2";
        opts.filenameSuffix = ".settings";
        opts.folderName = "DSP56300EmulatorNordG2";
        opts.osxLibrarySubFolder = "Application Support/DSP56300EmulatorNordG2";
        return opts;
    }
} // namespace

namespace rg2JucePlugin
{
    AudioPluginAudioProcessor::AudioPluginAudioProcessor() :
        Processor(BusesProperties()
                      .withOutput("Out 1-2", juce::AudioChannelSet::stereo(), true)
                      .withInput("In 1-2", juce::AudioChannelSet::stereo(), true),
                  getOptions(), pluginLib::initProcessorProperties())
    {
        getController();
        const auto latencyBlocks =
            getConfig().getIntValue("latencyBlocks", static_cast<int>(getPlugin().getLatencyBlocks()));
        Processor::setLatencyBlocks(latencyBlocks);
    }

	AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
	{
		m_g2Device = nullptr;
		destroyEditorState();
	}

	jucePluginEditorLib::PluginEditorState* AudioPluginAudioProcessor::createEditorState()
	{
		return new PluginEditorState(*this);
	}

	synthLib::Device* AudioPluginAudioProcessor::createDevice()
	{
		synthLib::DeviceCreateParams params;
		getRemoteDeviceParams(params);
		params.homePath = getDataFolder();
		auto* d = new rg2::Device(params);
		if (d->firmwareStatus().state != rg2::FirmwareState::Present)
		{
			delete d;
			m_g2Device = nullptr;
			throw synthLib::DeviceException(synthLib::DeviceError::FirmwareMissing,
											"A firmware image is required, but was not found.");
		}

		rg2::Device::BootRequest request;
		request.config.lookaheadFrames = rg2::kLookaheadFrames;
		request.config.maxHostBlockFrames = 4096;
		const auto result = d->boot(request);
		if (!result.booted || result.faulted)
		{
			delete d;
			m_g2Device = nullptr;
			throw synthLib::DeviceException(synthLib::DeviceError::Unknown,
											"Nord Modular G2 boot failed: " + result.why);
		}

		m_g2Device = d;
		return d;
	}

    void AudioPluginAudioProcessor::getRemoteDeviceParams(synthLib::DeviceCreateParams& _params) const
    {
        Processor::getRemoteDeviceParams(_params);
        _params.homePath = getDataFolder();
    }

    pluginLib::Controller* AudioPluginAudioProcessor::createController() { return new rg2JucePlugin::Controller(*this); }
} // namespace rg2JucePlugin

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new rg2JucePlugin::AudioPluginAudioProcessor(); }

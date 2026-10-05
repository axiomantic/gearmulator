#include "g2PluginProcessor.h"

#include "firmwareState.h"
#include "g2Controller.h"
#include "g2Device.h"
#include "g2Plugin.h"
#include "g2PluginEditorState.h"

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

namespace g2JucePlugin
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

    AudioPluginAudioProcessor::~AudioPluginAudioProcessor() { destroyEditorState(); }

    g2::Device* AudioPluginAudioProcessor::getG2Device() const noexcept
    {
        return dynamic_cast<g2::Device*>(m_device.get());
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
        auto* d = new g2::Device(params);
        if (d->firmwareStatus().state != g2::FirmwareState::Present)
        {
            delete d;
            throw synthLib::DeviceException(synthLib::DeviceError::FirmwareMissing,
                                            "A firmware image is required, but was not found.");
        }

        g2::Device::BootRequest request;
        request.config.lookaheadFrames = g2::kLookaheadFrames;
        request.config.maxHostBlockFrames = 4096;
        const auto result = d->boot(request);
        if (!result.booted || result.faulted)
        {
            delete d;
            throw synthLib::DeviceException(synthLib::DeviceError::Unknown,
                                            "Nord Modular G2 boot failed: " + result.why);
        }

        return d;
    }

    void AudioPluginAudioProcessor::getRemoteDeviceParams(synthLib::DeviceCreateParams& _params) const
    {
        Processor::getRemoteDeviceParams(_params);
        _params.homePath = getDataFolder();
    }

    pluginLib::Controller* AudioPluginAudioProcessor::createController() { return new g2JucePlugin::Controller(*this); }
} // namespace g2JucePlugin

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new g2JucePlugin::AudioPluginAudioProcessor(); }

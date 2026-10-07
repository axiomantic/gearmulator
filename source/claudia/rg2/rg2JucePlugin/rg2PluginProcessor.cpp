#include "rg2PluginProcessor.h"

#include "firmwareState.h"
#include "rg2Controller.h"
#include "rg2Device.h"
#include "rg2Plugin.h"
#include "rg2PluginEditorState.h"

#include "BinaryData.h"
#include "baseLib/binarystream.h"
#include "jucePluginLib/processorPropertiesInit.h"
#include "synthLib/deviceException.h"

#include <cmath>

namespace
{
	juce::PropertiesFile::Options getOptions()
	{
		juce::PropertiesFile::Options opts;
		opts.applicationName = "DSP56300EmulatorRedGecko2";
		opts.filenameSuffix = ".settings";
		opts.folderName = "DSP56300EmulatorRedGecko2";
		opts.osxLibrarySubFolder = "Application Support/DSP56300EmulatorRedGecko2";
		return opts;
	}
} // namespace

namespace rg2JucePlugin
{
	AudioPluginAudioProcessor::AudioPluginAudioProcessor() :
		Processor(BusesProperties()
					  .withOutput("Out 1-2", juce::AudioChannelSet::stereo(), true)
					  .withOutput("Out 3-4", juce::AudioChannelSet::stereo(), true)
					  .withInput("In 1-2", juce::AudioChannelSet::stereo(), true)
					  .withInput("In 3-4", juce::AudioChannelSet::stereo(), true),
				  getOptions(), pluginLib::initProcessorProperties())
	{
		getController();
		getPlugin().setMidiClockEnabled(false);
		const auto latencyBlocks =
			getConfig().getIntValue("latencyBlocks", static_cast<int>(getPlugin().getLatencyBlocks()));
		Processor::setLatencyBlocks(latencyBlocks);
	}

	AudioPluginAudioProcessor::~AudioPluginAudioProcessor() { destroyEditorState(); }

	void AudioPluginAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
	{
		bool isPlaying = false;
		bool hasPpq = false;
		double ppqPosition = 0.0;
		double bpm = 120.0;

		if (const auto* playHead = getPlayHead())
		{
			if (auto pos = playHead->getPosition())
			{
				isPlaying = pos->getIsPlaying();
				if (pos->getBpm())
					bpm = *pos->getBpm();
				if (pos->getPpqPosition())
				{
					ppqPosition = *pos->getPpqPosition();
					hasPpq = true;
				}
			}
		}

		const int numSamples = buffer.getNumSamples();
		const double sampleRate = getSampleRate() > 0.0 ? getSampleRate() : 44100.0;

		if (isPlaying && !m_wasPlaying)
		{
			m_wasPlaying = true;
			m_lastPpqTick = -1;
			m_clockPhase = 0.0;
			midiMessages.addEvent(juce::MidiMessage::midiStart(), 0);
		}
		else if (!isPlaying && m_wasPlaying)
		{
			m_wasPlaying = false;
			m_lastPpqTick = -1;
			m_clockPhase = 0.0;
			midiMessages.addEvent(juce::MidiMessage::midiStop(), 0);
		}

		if (isPlaying && bpm > 0.0)
		{
			if (hasPpq)
			{
				const double ppqPerSample = (bpm / 60.0) / sampleRate;
				if (ppqPerSample > 0.0)
				{
					const auto startTick = static_cast<int64_t>(std::floor(ppqPosition * 24.0));
					if (m_lastPpqTick < 0 || startTick < m_lastPpqTick || startTick > m_lastPpqTick + 24)
						m_lastPpqTick = startTick - 1;

					int64_t k = m_lastPpqTick + 1;
					while (true)
					{
						const double targetPpq = static_cast<double>(k) / 24.0;
						const double sampleOffset = (targetPpq - ppqPosition) / ppqPerSample;
						int sample = static_cast<int>(std::ceil(sampleOffset - 1e-9));
						if (sample < 0)
							sample = 0;

						if (sample >= numSamples)
							break;

						midiMessages.addEvent(juce::MidiMessage::midiClock(), sample);
						m_lastPpqTick = k;
						++k;
					}
				}
			}
			else
			{
				const double samplesPerPulse = (sampleRate * 60.0) / (bpm * 24.0);
				for (int sample = 0; sample < numSamples; ++sample)
				{
					m_clockPhase += 1.0;
					if (m_clockPhase >= samplesPerPulse)
					{
						m_clockPhase -= samplesPerPulse;
						midiMessages.addEvent(juce::MidiMessage::midiClock(), sample);
					}
				}
			}
		}

		Processor::processBlock(buffer, midiMessages);
	}

	rg2::Device* AudioPluginAudioProcessor::getG2Device() const noexcept
	{
		return dynamic_cast<rg2::Device*>(m_device.get());
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
			throw synthLib::DeviceException(synthLib::DeviceError::FirmwareMissing,
											"A firmware image is required, but was not found.");
		}

		const uint16_t port = static_cast<uint16_t>(getConfig().getIntValue("port", 7777));
		d->setPort(port);

		rg2::Device::BootRequest request;
		request.config.lookaheadFrames = rg2::kLookaheadFrames;
		request.config.maxHostBlockFrames = 4096;
		const auto result = d->boot(request);
		if (!result.booted || result.faulted)
		{
			delete d;
			throw synthLib::DeviceException(synthLib::DeviceError::Unknown, "Red Gecko 2 boot failed: " + result.why);
		}

		return d;
	}

	void AudioPluginAudioProcessor::getRemoteDeviceParams(synthLib::DeviceCreateParams& _params) const
	{
		Processor::getRemoteDeviceParams(_params);
		_params.homePath = getDataFolder();
	}

	pluginLib::Controller* AudioPluginAudioProcessor::createController()
	{
		return new rg2JucePlugin::Controller(*this);
	}

	void AudioPluginAudioProcessor::setPort(const uint16_t port)
	{
		if (auto* d = getG2Device())
		{
			d->setPort(port);
			const uint16_t bound = d->port();
			if (bound > 0)
				getConfig().setValue("port", static_cast<int>(bound));
			else
				getConfig().setValue("port", static_cast<int>(port));
		}
		else
		{
			getConfig().setValue("port", static_cast<int>(port));
		}
	}

	uint16_t AudioPluginAudioProcessor::getPort() const
	{
		if (const auto* d = getG2Device())
			return d->port();
		return static_cast<uint16_t>(
			const_cast<AudioPluginAudioProcessor*>(this)->getConfig().getIntValue("port", 7777));
	}

	uint16_t AudioPluginAudioProcessor::findFreePort()
	{
		setPort(0);
		return getPort();
	}

	void AudioPluginAudioProcessor::saveChunkData(baseLib::BinaryStream& s)
	{
		Processor::saveChunkData(s);
		baseLib::ChunkWriter cw(s, "PORT", 1);
		s.write<uint16_t>(getPort());
	}

	void AudioPluginAudioProcessor::loadChunkData(baseLib::ChunkReader& _cr)
	{
		Processor::loadChunkData(_cr);
		_cr.add("PORT", 1,
				[this](baseLib::BinaryStream& _binaryStream, uint32_t /*_version*/)
				{
					const auto port = _binaryStream.read<uint16_t>();
					setPort(port);
				});
	}
} // namespace rg2JucePlugin

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new rg2JucePlugin::AudioPluginAudioProcessor(); }

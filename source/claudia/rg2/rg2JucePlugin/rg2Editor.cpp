#include "rg2Editor.h"

#include "rg2Controller.h"
#include "rg2Device.h"
#include "rg2PluginProcessor.h"

#include "transportSocket.h"

#include "juceRmlUi/rmlElemValue.h"
#include "juceRmlUi/rmlEventListener.h"

#include "RmlUi/Core/StringUtilities.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rg2JucePlugin
{
	SlotButton::SlotButton(Rml::Element* _button, Editor& _editor) : PartButton(_button, _editor), m_editor(_editor) {}

	void SlotButton::onClick(Rml::Event&) { m_editor.setCurrentPart(getPart()); }

	Editor::Editor(jucePluginEditorLib::Processor& _processor, const jucePluginEditorLib::Skin& _skin) :
		jucePluginEditorLib::Editor(_processor, _skin),
		m_controller(dynamic_cast<Controller&>(_processor.getController()))
	{
	}

	Editor::~Editor() { stopTimer(); }

	void Editor::create()
	{
		jucePluginEditorLib::Editor::create();

		const std::array<const char*, 4> slotNames = {"slot_a", "slot_b", "slot_c", "slot_d"};
		for (uint8_t p = 0; p < static_cast<uint8_t>(m_slotButtons.size()); ++p)
		{
			auto* elem = findChild(slotNames[p], false);
			if (!elem)
			{
				const std::string alt = "slot_" + std::to_string(p);
				elem = findChild(alt, false);
			}
			if (elem)
			{
				m_slotButtons[p] = std::make_unique<SlotButton>(elem, *this);
				m_slotButtons[p]->initalize(p);
			}
		}

		const std::array<const char*, 4> slotLedNames = {"slot_led_a", "slot_led_b", "slot_led_c", "slot_led_d"};
		for (uint8_t p = 0; p < static_cast<uint8_t>(m_slotLeds.size()); ++p)
		{
			m_slotLeds[p] = findChild(slotLedNames[p], false);
			if (!m_slotLeds[p])
			{
				const std::string alt = "slot_led_" + std::to_string(p);
				m_slotLeds[p] = findChild(alt, false);
			}
		}

		bindVolumeControl();

		onCurrentPartChanged(m_controller.getCurrentPart());

		updateSocketStatus();
		startTimer(250);
	}

	void Editor::bindVolumeControl()
	{
		m_volumeSlider = findChild("master_volume", false);
		m_volumeDisplay = findChild("volume_display", false);
		m_volumeDbDisplay = findChild("volume_db_display", false);

		auto* g2Proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		auto* g2Dev = g2Proc ? g2Proc->getG2Device() : nullptr;
		const float currentVol = g2Dev ? g2Dev->panelControl(rg2::PanelControl::MasterVolume) : 1.0f;
		m_lastVolume = currentVol;

		if (m_volumeSlider)
		{
			juceRmlUi::ElemValue::setValue(m_volumeSlider, currentVol, false);

			juceRmlUi::EventListener::Add(m_volumeSlider, Rml::EventId::Change, [this](Rml::Event&)
			{
				if (!m_volumeSlider)
					return;
				const float val = std::clamp(juceRmlUi::ElemValue::getValue(m_volumeSlider), 0.0f, 1.0f);
				m_lastVolume = val;

				if (auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
				{
					if (auto* dev = proc->getG2Device())
						dev->setPanelControl(rg2::PanelControl::MasterVolume, val);
				}
				updateVolumeDisplay(val);
			});
		}

		updateVolumeDisplay(currentVol);
	}

	void Editor::updateVolumeDisplay(const float _volume)
	{
		const float clamped = std::clamp(_volume, 0.0f, 1.0f);

		if (m_volumeDisplay)
		{
			const int pct = static_cast<int>(std::round(clamped * 100.0f));
			m_volumeDisplay->SetInnerRML(Rml::StringUtilities::EncodeRml(std::to_string(pct) + "%"));
		}

		if (m_volumeDbDisplay)
		{
			std::string dbStr;
			if (clamped <= 0.001f)
			{
				dbStr = "-inf dB";
			}
			else
			{
				const float db = 20.0f * std::log10(clamped);
				char buf[16];
				std::snprintf(buf, sizeof(buf), "%.1f dB", db);
				dbStr = buf;
			}
			m_volumeDbDisplay->SetInnerRML(Rml::StringUtilities::EncodeRml(dbStr));
		}
	}

	void Editor::onCurrentPartChanged(const uint8_t _part)
	{
		jucePluginEditorLib::Editor::onCurrentPartChanged(_part);

		for (size_t p = 0; p < m_slotButtons.size(); ++p)
		{
			const bool active = (static_cast<uint8_t>(p) == _part);
			if (m_slotButtons[p])
				m_slotButtons[p]->setChecked(active);
			if (m_slotLeds[p])
			{
				m_slotLeds[p]->SetClass("slot_led_lit", active);
				m_slotLeds[p]->SetClass("slot_led_unlit", !active);
			}
		}

		if (auto* activeDisplay = findChild("active_slot_display", false))
		{
			const char slotLetter = static_cast<char>('A' + (_part < 4 ? _part : 0));
			const std::string text = std::string("Slot ") + slotLetter;
			activeDisplay->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
		}
	}

	void Editor::timerCallback()
	{
		updateSocketStatus();

		if (auto* g2Proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
		{
			if (auto* g2Dev = g2Proc->getG2Device())
			{
				const float vol = g2Dev->panelControl(rg2::PanelControl::MasterVolume);
				if (std::abs(vol - m_lastVolume) > 0.005f)
				{
					m_lastVolume = vol;
					if (m_volumeSlider)
						juceRmlUi::ElemValue::setValue(m_volumeSlider, vol, false);
					updateVolumeDisplay(vol);
				}
			}
		}
	}

	void Editor::updateSocketStatus()
	{
		auto* g2Proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		auto* g2Dev = g2Proc ? g2Proc->getG2Device() : nullptr;
		const auto* socketServer = g2Dev ? g2Dev->socketServer() : nullptr;

		int state = 0;
		std::string text = "Socket: Inactive";

		if (socketServer && socketServer->port() > 0)
		{
			const uint16_t port = socketServer->port();
			if (socketServer->hasClient())
			{
				state = 2;
				const uint64_t in = socketServer->framesIn();
				const uint64_t out = socketServer->framesOut();
				const uint64_t dropped = socketServer->droppedFrames();
				text = "127.0.0.1:" + std::to_string(port) + " (Connected) [Rx: " + std::to_string(in) +
					" Tx: " + std::to_string(out);
				if (dropped > 0)
					text += " Drop: " + std::to_string(dropped);
				text += "]";
			}
			else
			{
				state = 1;
				text = "127.0.0.1:" + std::to_string(port) + " (Listening on TCP)";
			}
		}

		if (state != m_lastSocketState)
		{
			m_lastSocketState = state;
			if (auto* statusElem = findChild("socket_status", false))
			{
				statusElem->SetClass("socket_connected", state == 2);
				statusElem->SetClass("socket_listening", state == 1);
				statusElem->SetClass("socket_offline", state == 0);
			}
			if (auto* badgeElem = findChild("socket_badge", false))
			{
				const char* badgeText = (state == 2 ? "ONLINE" : (state == 1 ? "LISTENING" : "OFFLINE"));
				badgeElem->SetInnerRML(badgeText);
			}
		}

		if (text != m_lastSocketStatusText)
		{
			m_lastSocketStatusText = text;
			if (auto* textElem = findChild("socket_text", false))
			{
				textElem->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
			}
			else if (auto* statusElem = findChild("socket_status", false))
			{
				statusElem->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
			}
		}
	}

	std::pair<std::string, std::string> Editor::getDemoRestrictionText() const { return {}; }
} // namespace rg2JucePlugin

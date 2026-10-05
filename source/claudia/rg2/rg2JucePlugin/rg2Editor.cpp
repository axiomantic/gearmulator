#include "rg2Editor.h"

#include "rg2Controller.h"
#include "rg2Device.h"
#include "rg2PluginProcessor.h"

#include "board.h"
#include "latches.h"
#include "panel.h"
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

		bindFrontPanel();

		onCurrentPartChanged(m_controller.getCurrentPart());

		updateSocketStatus();
		startTimer(100);
	}

	void Editor::bindFrontPanel()
	{
		bindVolumeControl();
		bindPerformanceControls();
		bindPages();
		bindVariations();
		bindOctaveShift();
		bindEncoders();
		updateParamLcds();
		updateLedRings();
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

	void Editor::bindPerformanceControls()
	{
		m_pitchStick = findChild("pitch_stick", false);
		if (m_pitchStick)
		{
			juceRmlUi::ElemValue::setValue(m_pitchStick, 0.5f, false);
			juceRmlUi::EventListener::Add(m_pitchStick, Rml::EventId::Change, [this](Rml::Event&)
			{
				if (!m_pitchStick)
					return;
				const float val = std::clamp(juceRmlUi::ElemValue::getValue(m_pitchStick), 0.0f, 1.0f);
				if (auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
				{
					if (auto* dev = proc->getG2Device())
						dev->setPanelControl(rg2::PanelControl::PitchStick, val);
				}
			});
		}

		m_modWheel = findChild("mod_wheel", false);
		if (m_modWheel)
		{
			juceRmlUi::ElemValue::setValue(m_modWheel, 0.0f, false);
			juceRmlUi::EventListener::Add(m_modWheel, Rml::EventId::Change, [this](Rml::Event&)
			{
				if (!m_modWheel)
					return;
				const float val = std::clamp(juceRmlUi::ElemValue::getValue(m_modWheel), 0.0f, 1.0f);
				if (auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
				{
					if (auto* dev = proc->getG2Device())
						dev->setPanelControl(rg2::PanelControl::ModWheel, val);
				}
			});
		}

		const std::array<const char*, 8> utilityButtons = {
			"btn_system", "btn_patch", "btn_store", "btn_panic",
			"btn_shift", "btn_kb_split", "btn_patch_load", "btn_focus_copy"
		};
		for (size_t i = 0; i < utilityButtons.size(); ++i)
		{
			if (auto* btn = findChild(utilityButtons[i], false))
			{
				juceRmlUi::EventListener::Add(btn, Rml::EventId::Click, [this, i](Rml::Event&)
				{
					if (auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
					{
						if (auto* dev = proc->getG2Device())
						{
							if (auto* board = dev->board())
							{
								board->panel().setButtonPressed(static_cast<uint8_t>(i % 8), 0, true);
							}
						}
					}
				});
			}
		}

		if (auto* wheel = findChild("data_wheel", false))
		{
			juceRmlUi::EventListener::Add(wheel, Rml::EventId::Click, [this](Rml::Event&)
			{
				if (auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
				{
					if (auto* dev = proc->getG2Device())
					{
						if (auto* board = dev->board())
							board->panel().setEncoderDelta(0, 1);
					}
				}
			});
		}
	}

	void Editor::bindPages()
	{
		const std::array<const char*, 5> btnNames = {
			"page_btn_a", "page_btn_b", "page_btn_c", "page_btn_d", "page_btn_e"
		};
		const std::array<const char*, 5> ledNames = {
			"page_led_a", "page_led_b", "page_led_c", "page_led_d", "page_led_e"
		};

		for (size_t p = 0; p < 5; ++p)
		{
			m_pageButtons[p] = findChild(btnNames[p], false);
			m_pageLeds[p] = findChild(ledNames[p], false);

			if (m_pageButtons[p])
			{
				juceRmlUi::EventListener::Add(m_pageButtons[p], Rml::EventId::Click, [this, p](Rml::Event&)
				{
					m_activePage = static_cast<uint8_t>(p);
					for (size_t i = 0; i < 5; ++i)
					{
						if (m_pageLeds[i])
						{
							m_pageLeds[i]->SetClass("led_lit", i == m_activePage);
							m_pageLeds[i]->SetClass("led_unlit", i != m_activePage);
						}
						if (m_pageButtons[i])
							m_pageButtons[i]->SetClass("checked", i == m_activePage);
					}
					updateParamLcds();
				});
			}
		}
	}

	void Editor::bindVariations()
	{
		for (size_t v = 0; v < 8; ++v)
		{
			const std::string btnName = "var_btn_" + std::to_string(v + 1);
			const std::string ledName = "var_led_" + std::to_string(v + 1);

			m_varButtons[v] = findChild(btnName, false);
			m_varLeds[v] = findChild(ledName, false);

			if (m_varButtons[v])
			{
				juceRmlUi::EventListener::Add(m_varButtons[v], Rml::EventId::Click, [this, v](Rml::Event&)
				{
					m_activeVariation = static_cast<uint8_t>(v);
					for (size_t i = 0; i < 8; ++i)
					{
						if (m_varLeds[i])
						{
							m_varLeds[i]->SetClass("led_lit", i == m_activeVariation);
							m_varLeds[i]->SetClass("led_unlit", i != m_activeVariation);
						}
					}
				});
			}
		}
	}

	void Editor::bindOctaveShift()
	{
		m_octaveDownBtn = findChild("octave_down", false);
		m_octaveUpBtn = findChild("octave_up", false);

		for (size_t o = 0; o < 4; ++o)
		{
			const std::string ledName = "octave_led_" + std::to_string(o + 1);
			m_octaveLeds[o] = findChild(ledName, false);
		}

		if (m_octaveDownBtn)
		{
			juceRmlUi::EventListener::Add(m_octaveDownBtn, Rml::EventId::Click, [this](Rml::Event&)
			{
				if (m_octaveShift > -2)
					--m_octaveShift;
				const int activeLed = m_octaveShift + 2;
				for (size_t i = 0; i < 4; ++i)
				{
					if (m_octaveLeds[i])
					{
						m_octaveLeds[i]->SetClass("led_lit", static_cast<int>(i) == activeLed);
						m_octaveLeds[i]->SetClass("led_unlit", static_cast<int>(i) != activeLed);
					}
				}
			});
		}

		if (m_octaveUpBtn)
		{
			juceRmlUi::EventListener::Add(m_octaveUpBtn, Rml::EventId::Click, [this](Rml::Event&)
			{
				if (m_octaveShift < 1)
					++m_octaveShift;
				const int activeLed = m_octaveShift + 2;
				for (size_t i = 0; i < 4; ++i)
				{
					if (m_octaveLeds[i])
					{
						m_octaveLeds[i]->SetClass("led_lit", static_cast<int>(i) == activeLed);
						m_octaveLeds[i]->SetClass("led_unlit", static_cast<int>(i) != activeLed);
					}
				}
			});
		}
	}

	void Editor::bindEncoders()
	{
		for (size_t i = 0; i < 8; ++i)
		{
			const std::string knobName = "encoder_" + std::to_string(i);
			const std::string colBtnName = "col_btn_" + std::to_string(i);

			m_encoderKnobs[i] = findChild(knobName, false);
			m_colButtons[i] = findChild(colBtnName, false);

			for (size_t j = 0; j < 15; ++j)
			{
				const std::string ringLedName = "r" + std::to_string(i) + "_led" + std::to_string(j);
				m_ringLeds[i][j] = findChild(ringLedName, false);
			}

			if (m_encoderKnobs[i])
			{
				juceRmlUi::EventListener::Add(m_encoderKnobs[i], Rml::EventId::Click, [this, i](Rml::Event&)
				{
					m_encoderValues[i] = std::fmod(m_encoderValues[i] + 0.08f, 1.0f);
					if (auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
					{
						if (auto* dev = proc->getG2Device())
						{
							if (auto* board = dev->board())
								board->panel().setEncoderDelta(static_cast<uint8_t>(i), 1);
						}
					}
					updateLedRings();
					updateParamLcds();
				});
			}

			if (m_colButtons[i])
			{
				juceRmlUi::EventListener::Add(m_colButtons[i], Rml::EventId::Click, [this, i](Rml::Event&)
				{
					if (auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
					{
						if (auto* dev = proc->getG2Device())
						{
							if (auto* board = dev->board())
								board->panel().setButtonPressed(0, static_cast<uint8_t>(i), true);
						}
					}
				});
			}
		}

		for (size_t i = 0; i < 8; ++i)
		{
			const std::string titleName = "param_title_" + std::to_string(i);
			const std::string valName = "param_val_" + std::to_string(i);
			m_paramTitles[i] = findChild(titleName, false);
			m_paramVals[i] = findChild(valName, false);
		}
	}

	void Editor::updateParamLcds()
	{
		struct ParamDesc
		{
			const char* title;
			const char* val;
		};

		static const std::array<std::array<ParamDesc, 8>, 5> pageParams = {{
			// Page A: Osc
			{{{ "Freq", "1.05kHz" }, { "Res", "1.15" }, { "Amount", "64.1" }, { "Amount", "77.3" },
			  { "Amount", "0" }, { "Amount", "0" }, { "Type", "LP" }, { "XFade", "2" }}},
			// Page B: LFO
			{{{ "Rate", "4.20Hz" }, { "Wave", "Tri" }, { "Poly", "On" }, { "Sync", "Off" },
			  { "KBT", "50%" }, { "Env", "Off" }, { "Amount", "64.0" }, { "Dest", "Pitch" }}},
			// Page C: Env
			{{{ "Attack", "12ms" }, { "Decay", "350ms" }, { "Sustain", "70%" }, { "Release", "450ms" },
			  { "Vel", "85%" }, { "Curve", "Lin" }, { "Invert", "Off" }, { "Retrig", "On" }}},
			// Page D: Filter
			{{{ "Type", "LP24" }, { "Freq", "2.40kHz" }, { "Res", "3.20" }, { "Drive", "15%" },
			  { "EnvAmt", "+45" }, { "KBT", "100%" }, { "Attack", "5ms" }, { "Decay", "200ms" }}},
			// Page E: Effect
			{{{ "Type", "Delay" }, { "Time", "250ms" }, { "Fdbk", "45%" }, { "Tone", "Warm" },
			  { "Depth", "30%" }, { "Speed", "1.2Hz" }, { "Mix", "35%" }, { "Level", "0.0dB" }}}
		}};

		const auto& currentParams = pageParams[m_activePage < 5 ? m_activePage : 0];
		for (size_t i = 0; i < 8; ++i)
		{
			if (m_paramTitles[i])
				m_paramTitles[i]->SetInnerRML(Rml::StringUtilities::EncodeRml(currentParams[i].title));
			if (m_paramVals[i])
				m_paramVals[i]->SetInnerRML(Rml::StringUtilities::EncodeRml(currentParams[i].val));
		}
	}

	void Editor::updateLedRings()
	{
		auto* g2Proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		auto* g2Dev = g2Proc ? g2Proc->getG2Device() : nullptr;
		auto* board = g2Dev ? g2Dev->board() : nullptr;

		for (size_t i = 0; i < 8; ++i)
		{
			const uint16_t ringState = board ? board->latches().getLedRingState(static_cast<uint8_t>(i)) : 0;
			const int activeLedMax = static_cast<int>(std::round(m_encoderValues[i] * 14.0f));

			for (size_t j = 0; j < 15; ++j)
			{
				if (!m_ringLeds[i][j])
					continue;

				bool isLit = false;
				if (ringState != 0)
					isLit = (ringState & (1u << j)) != 0;
				else
					isLit = (static_cast<int>(j) <= activeLedMax);

				m_ringLeds[i][j]->SetClass("lit", isLit);
				m_ringLeds[i][j]->SetClass("unlit", !isLit);
			}
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
		updateLedRings();

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

#include "rg2Editor.h"

#include "rg2Controller.h"
#include "rg2Device.h"
#include "rg2PluginProcessor.h"

#include "board.h"
#include "latches.h"
#include "panel.h"
#include "transportWebSocket.h"

#include <juce_core/juce_core.h>

#include "juceRmlUi/rmlElemValue.h"
#include "juceRmlUi/rmlEventListener.h"
#include "juceRmlUi/rmlHelper.h"

#include "RmlUi/Core/Elements/ElementFormControlInput.h"
#include "RmlUi/Core/StringUtilities.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
	std::string trimString(const std::string& str)
	{
		size_t start = 0;
		while (start < str.size() &&
			   (str[start] == ' ' || str[start] == '\t' || str[start] == '\r' || str[start] == '\n'))
			++start;
		if (start == str.size())
			return {};
		size_t end = str.size();
		while (end > start &&
			   (str[end - 1] == ' ' || str[end - 1] == '\t' || str[end - 1] == '\r' || str[end - 1] == '\n'))
			--end;
		return str.substr(start, end - start);
	}

	bool extractLcdText(const std::string& raw, std::string& out)
	{
		out.clear();

		bool allZero = true;
		for (const unsigned char c : raw)
		{
			if (c != 0)
			{
				allZero = false;
				break;
			}
		}
		if (allZero)
			return false;

		static constexpr const char* kMeterGlyphs[8] = {
			" ", // 0x00: lower 1/8 block
			"▂", // 0x01: lower 1/4 block
			"▃", // 0x02: lower 3/8 block
			"▄", // 0x03: lower 1/2 block
			"▅", // 0x04: lower 5/8 block
			"▆", // 0x05: lower 3/4 block
			"▇", // 0x06: lower 7/8 block
			"█", // 0x07: full block
		};

		for (const unsigned char c : raw)
		{
			if (c <= 7)
			{
				out += kMeterGlyphs[c];
			}
			else if (c >= 8 && c < 16)
			{
				out += kMeterGlyphs[c - 8];
			}
			else if (c >= 32 && c <= 126)
			{
				out.push_back(static_cast<char>(c));
			}
			else
			{
				out.push_back(' ');
			}
		}
		out = trimString(out);
		return !out.empty();
	}
} // namespace

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

		const std::array<const char*, 4> slotLedLetters = {"a", "b", "c", "d"};
		for (uint8_t p = 0; p < static_cast<uint8_t>(m_slotLeds.size()); ++p)
		{
			const std::string name1 = std::string("slot_led_") + slotLedLetters[p];
			m_slotLeds[p] = findChild(name1, false);
			if (!m_slotLeds[p])
			{
				const std::string name2 = std::string("slot_") + slotLedLetters[p] + "_led";
				m_slotLeds[p] = findChild(name2, false);
			}
			if (!m_slotLeds[p])
			{
				const std::string name3 = "slot_led_" + std::to_string(p);
				m_slotLeds[p] = findChild(name3, false);
			}
			if (!m_slotLeds[p])
			{
				const std::string name4 = "slot_" + std::to_string(p) + "_led";
				m_slotLeds[p] = findChild(name4, false);
			}
		}

		bindFrontPanel();
		bindPortControls();
		bindParameterListeners();

		onCurrentPartChanged(m_controller.getCurrentPart());


		updateSocketStatus();
		startTimer(30);
	}

	void Editor::triggerButtonPress(const uint8_t _row, const uint8_t _col)
	{
		auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		auto* dev = proc ? proc->getG2Device() : nullptr;
		auto* board = dev ? dev->board() : nullptr;
		if (board)
		{
			board->panel().setButtonPressed(_row, _col, true);
			m_pressedButtons.push_back({_row, _col, 3});
		}
	}

	void Editor::setButtonState(const uint8_t _col, const uint8_t _row, const bool _isDown)
	{
		auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		auto* dev = proc ? proc->getG2Device() : nullptr;
		if (dev && dev->board())
			dev->board()->panel().setButtonState(_col, _row, _isDown);
	}

	void Editor::bindFrontPanel()
	{
		m_mainLcdLine1 = findChild("main_lcd_line1", false);
		m_mainLcdLine2 = findChild("main_lcd_line2", false);

		bindVolumeControl();
		bindPerformanceControls();
		bindNavigationAndModeButtons();
		bindPages();
		bindVariations();
		bindOctaveShift();
		bindEncoders();
		updateParamLcds();
		updateLedRings();
	}

	void Editor::bindPortControls()
	{
		auto* g2Proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		if (!g2Proc)
			return;

		m_inputPort = findChild("input_port", false);
		m_btnSetPort = findChild("btn_set_port", false);
		m_btnFindPort = findChild("btn_find_port", false);

		if (m_inputPort)
		{
			const uint16_t curPort = g2Proc->getPort();
			if (auto* textInput = dynamic_cast<Rml::ElementFormControlInput*>(m_inputPort))
				textInput->SetValue(std::to_string(curPort));

			auto submitPort = [this, g2Proc](Rml::Event&)
			{
				if (!m_inputPort)
					return;
				std::string val;
				if (auto* textInput = dynamic_cast<Rml::ElementFormControlInput*>(m_inputPort))
					val = textInput->GetValue();
				else
					val = m_inputPort->GetInnerRML();

				try
				{
					const int p = std::stoi(val);
					if (p >= 1 && p <= 65535)
					{
						g2Proc->setPort(static_cast<uint16_t>(p));
						updateSocketStatus();
						const uint16_t activePort = g2Proc->getPort();
						if (auto* textInput = dynamic_cast<Rml::ElementFormControlInput*>(m_inputPort))
							textInput->SetValue(std::to_string(activePort > 0 ? activePort : p));
					}
				}
				catch (...)
				{
				}
			};

			juceRmlUi::EventListener::Add(m_inputPort, Rml::EventId::Change, submitPort);
			juceRmlUi::EventListener::Add(m_inputPort, Rml::EventId::Submit, submitPort);

			if (m_btnSetPort)
				juceRmlUi::EventListener::Add(m_btnSetPort, Rml::EventId::Click, submitPort);
		}

		if (m_btnFindPort)
		{
			juceRmlUi::EventListener::Add(m_btnFindPort, Rml::EventId::Click,
										  [this, g2Proc](Rml::Event&)
										  {
											  const uint16_t freePort = g2Proc->findFreePort();
											  updateSocketStatus();
											  if (m_inputPort)
											  {
												  if (auto* textInput =
														  dynamic_cast<Rml::ElementFormControlInput*>(m_inputPort))
													  textInput->SetValue(std::to_string(freePort));
											  }
										  });
		}
	}

	void Editor::bindVolumeControl()
	{
		m_volumeSlider = findChild("master_volume", false);
		m_volumeKnobFrame = findChild("rotary_knob_frame", false);
		if (!m_volumeKnobFrame && m_volumeSlider)
			m_volumeKnobFrame = m_volumeSlider->GetParentNode();

		m_volumeDisplay = findChild("volume_display", false);
		m_volumeDbDisplay = findChild("volume_db_display", false);

		auto* g2Proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		auto* g2Dev = g2Proc ? g2Proc->getG2Device() : nullptr;
		const float currentVol = g2Dev ? g2Dev->panelControl(rg2::PanelControl::MasterVolume) : 1.0f;
		m_lastVolume = currentVol;

		auto applyVolume = [this, g2Dev](const float _val)
		{
			const float clamped = std::clamp(_val, 0.0f, 1.0f);
			m_lastVolume = clamped;
			if (m_volumeSlider)
				juceRmlUi::ElemValue::setValue(m_volumeSlider, clamped, false);
			if (g2Dev)
				g2Dev->setPanelControl(rg2::PanelControl::MasterVolume, clamped);
			if (auto* p = m_controller.getParameter("MasterVolume", 0))
				p->setValueNotifyingHost(clamped, pluginLib::Parameter::Origin::Ui);
			updateVolumeDisplay(clamped);
		};

		auto attachVolumeDrag = [this, applyVolume](Rml::Element* _elem)
		{
			if (!_elem)
				return;
			_elem->SetProperty(Rml::PropertyId::Drag, Rml::Style::Drag::Drag);

			juceRmlUi::EventListener::Add(_elem, Rml::EventId::Mousedown,
										  [this](Rml::Event& _e)
										  {
											  m_isDraggingVolume = true;
											  m_volumeDragStartY = _e.GetUnprojectedMouseScreenPos().y;
											  m_volumeDragStartVal = m_lastVolume;
										  });

			juceRmlUi::EventListener::Add(_elem, Rml::EventId::Drag,
										  [this, applyVolume](Rml::Event& _e)
										  {
											  if (!m_isDraggingVolume)
												  return;
											  const float curY = _e.GetUnprojectedMouseScreenPos().y;
											  const float dy = m_volumeDragStartY - curY;
											  const float newVol = m_volumeDragStartVal + (dy / 150.0f);
											  applyVolume(newVol);
										  });

			auto stopDrag = [this](Rml::Event&) { m_isDraggingVolume = false; };
			juceRmlUi::EventListener::Add(_elem, Rml::EventId::Mouseup, stopDrag);
			juceRmlUi::EventListener::Add(_elem, Rml::EventId::Dragend, stopDrag);
		};

		if (m_volumeSlider)
		{
			juceRmlUi::ElemValue::setValue(m_volumeSlider, currentVol, false);
			attachVolumeDrag(m_volumeSlider);

			juceRmlUi::EventListener::Add(m_volumeSlider, Rml::EventId::Change,
										  [this, applyVolume](Rml::Event&)
										  {
											  if (!m_volumeSlider || m_isDraggingVolume)
												  return;
											  const float val = std::clamp(
												  juceRmlUi::ElemValue::getValue(m_volumeSlider), 0.0f, 1.0f);
											  applyVolume(val);
										  });
		}

		if (m_volumeKnobFrame && m_volumeKnobFrame != m_volumeSlider)
			attachVolumeDrag(m_volumeKnobFrame);

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
			m_pitchStick->SetProperty(Rml::PropertyId::Drag, Rml::Style::Drag::Drag);

			auto applyPitch = [this](const float _val)
			{
				const float clamped = std::clamp(_val, 0.0f, 1.0f);
				m_lastPitchStick = clamped;
				juceRmlUi::ElemValue::setValue(m_pitchStick, clamped, false);
				if (auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
				{
					if (auto* dev = proc->getG2Device())
						dev->setPanelControl(rg2::PanelControl::PitchStick, clamped);
				}
				if (auto* p = m_controller.getParameter("PitchStick", 0))
					p->setValueNotifyingHost(clamped, pluginLib::Parameter::Origin::Ui);
			};

			juceRmlUi::EventListener::Add(m_pitchStick, Rml::EventId::Mousedown,
										  [this](Rml::Event& _e)
										  {
											  m_isDraggingPitchStick = true;
											  m_pitchStickDragLastY = _e.GetUnprojectedMouseScreenPos().y;
										  });

			juceRmlUi::EventListener::Add(m_pitchStick, Rml::EventId::Drag,
										  [this, applyPitch](Rml::Event& _e)
										  {
											  if (!m_isDraggingPitchStick)
												  return;
											  const float curY = _e.GetUnprojectedMouseScreenPos().y;
											  const float dy = m_pitchStickDragLastY - curY;
											  m_pitchStickDragLastY = curY;
											  applyPitch(m_lastPitchStick + dy / 100.0f);
										  });

			auto releasePitch = [this, applyPitch](Rml::Event&)
			{
				m_isDraggingPitchStick = false;
				applyPitch(0.5f);
			};
			juceRmlUi::EventListener::Add(m_pitchStick, Rml::EventId::Mouseup, releasePitch);
			juceRmlUi::EventListener::Add(m_pitchStick, Rml::EventId::Dragend, releasePitch);

			juceRmlUi::EventListener::Add(m_pitchStick, Rml::EventId::Change,
										  [this, applyPitch](Rml::Event&)
										  {
											  if (!m_pitchStick || m_isDraggingPitchStick)
												  return;
											  applyPitch(juceRmlUi::ElemValue::getValue(m_pitchStick));
										  });
		}

		m_modWheel = findChild("mod_wheel", false);
		if (m_modWheel)
		{
			juceRmlUi::ElemValue::setValue(m_modWheel, 0.0f, false);
			m_modWheel->SetProperty(Rml::PropertyId::Drag, Rml::Style::Drag::Drag);

			auto applyMod = [this](const float _val)
			{
				const float clamped = std::clamp(_val, 0.0f, 1.0f);
				m_lastModWheel = clamped;
				juceRmlUi::ElemValue::setValue(m_modWheel, clamped, false);
				if (auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
				{
					if (auto* dev = proc->getG2Device())
						dev->setPanelControl(rg2::PanelControl::ModWheel, clamped);
				}
				if (auto* p = m_controller.getParameter("ModWheel", 0))
					p->setValueNotifyingHost(clamped, pluginLib::Parameter::Origin::Ui);
			};

			juceRmlUi::EventListener::Add(m_modWheel, Rml::EventId::Mousedown,
										  [this](Rml::Event& _e)
										  {
											  m_isDraggingModWheel = true;
											  m_modWheelDragLastY = _e.GetUnprojectedMouseScreenPos().y;
										  });

			juceRmlUi::EventListener::Add(m_modWheel, Rml::EventId::Drag,
										  [this, applyMod](Rml::Event& _e)
										  {
											  if (!m_isDraggingModWheel)
												  return;
											  const float curY = _e.GetUnprojectedMouseScreenPos().y;
											  const float dy = m_modWheelDragLastY - curY;
											  m_modWheelDragLastY = curY;
											  applyMod(m_lastModWheel + dy / 120.0f);
										  });

			auto stopMod = [this](Rml::Event&) { m_isDraggingModWheel = false; };
			juceRmlUi::EventListener::Add(m_modWheel, Rml::EventId::Mouseup, stopMod);
			juceRmlUi::EventListener::Add(m_modWheel, Rml::EventId::Dragend, stopMod);

			juceRmlUi::EventListener::Add(m_modWheel, Rml::EventId::Change,
										  [this, applyMod](Rml::Event&)
										  {
											  if (!m_modWheel || m_isDraggingModWheel)
												  return;
											  applyMod(juceRmlUi::ElemValue::getValue(m_modWheel));
										  });
		}

		m_dataWheel = findChild("data_wheel", false);
		if (m_dataWheel)
		{
			m_dataWheel->SetProperty(Rml::PropertyId::Drag, Rml::Style::Drag::Drag);

			juceRmlUi::EventListener::Add(m_dataWheel, Rml::EventId::Mousedown,
										  [this](Rml::Event& _e)
										  {
											  m_isDraggingDataWheel = true;
											  m_dataWheelDragLastY = _e.GetUnprojectedMouseScreenPos().y;
											  m_dataWheelAccum = 0.0f;
										  });

			juceRmlUi::EventListener::Add(
				m_dataWheel, Rml::EventId::Drag,
				[this](Rml::Event& _e)
				{
					if (!m_isDraggingDataWheel)
						return;
					const float curY = _e.GetUnprojectedMouseScreenPos().y;
					const float dy = m_dataWheelDragLastY - curY;
					m_dataWheelDragLastY = curY;
					m_dataWheelAccum += dy;

					constexpr float kWheelPixelsPerTick = 6.0f;
					if (std::abs(m_dataWheelAccum) >= kWheelPixelsPerTick)
					{
						const int ticks = static_cast<int>(m_dataWheelAccum / kWheelPixelsPerTick);
						m_dataWheelAccum -= static_cast<float>(ticks) * kWheelPixelsPerTick;

						if (auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
						{
							if (auto* dev = proc->getG2Device())
							{
								if (auto* board = dev->board())
									board->panel().setEncoderDelta(0,
																   static_cast<int8_t>(std::clamp(ticks, -127, 127)));
							}
						}

						m_dataWheelAngle += static_cast<float>(ticks) * 15.0f;
						if (m_dataWheel)
							m_dataWheel->SetProperty("transform",
													 "rotate(" + std::to_string(m_dataWheelAngle) + "deg)");
					}
				});

			auto stopWheel = [this](Rml::Event&) { m_isDraggingDataWheel = false; };
			juceRmlUi::EventListener::Add(m_dataWheel, Rml::EventId::Mouseup, stopWheel);
			juceRmlUi::EventListener::Add(m_dataWheel, Rml::EventId::Dragend, stopWheel);

			juceRmlUi::EventListener::Add(
				m_dataWheel, Rml::EventId::Click,
				[this](Rml::Event&)
				{
					if (auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
					{
						if (auto* dev = proc->getG2Device())
						{
							if (auto* board = dev->board())
								board->panel().setEncoderDelta(0, 1);
						}
					}
					m_dataWheelAngle += 15.0f;
					if (m_dataWheel)
						m_dataWheel->SetProperty("transform", "rotate(" + std::to_string(m_dataWheelAngle) + "deg)");
				});
		}

		struct UtilBtnDef
		{
			const char* id;
			const char* ledId;
			uint8_t col;
		};

		const std::array<UtilBtnDef, 7> utilityButtons = {{{"btn_patch", "led_patch", 1},
														   {"btn_store", nullptr, 2},
														   {"btn_panic", nullptr, 3},
														   {"btn_shift", nullptr, 4},
														   {"btn_kb_split", "led_kb_split", 5},
														   {"btn_patch_load", "led_patch_load", 6},
														   {"btn_focus_copy", nullptr, 7}}};

		for (const auto& ub : utilityButtons)
		{
			auto* btn = findChild(ub.id, false);

			if (btn)
			{
				auto* led = ub.ledId ? findChild(ub.ledId, false) : nullptr;
				juceRmlUi::EventListener::Add(btn, Rml::EventId::Click,
											  [this, btn, led, ub](Rml::Event&)
											  {
												  triggerButtonPress(0, ub.col);

												  if (std::string(ub.id) == "btn_patch")
												  {
													  m_patchActive = !m_patchActive;
													  if (led)
													  {
														  led->SetClass("led_lit", m_patchActive);
														  led->SetClass("led_unlit", !m_patchActive);
													  }
													  btn->SetClass("checked", m_patchActive);
												  }
												  else if (std::string(ub.id) == "btn_shift")
												  {
													  m_shiftActive = !m_shiftActive;
													  btn->SetClass("checked", m_shiftActive);
												  }
												  else if (std::string(ub.id) == "btn_kb_split")
												  {
													  m_kbSplitActive = !m_kbSplitActive;
													  if (led)
													  {
														  led->SetClass("led_lit", m_kbSplitActive);
														  led->SetClass("led_unlit", !m_kbSplitActive);
													  }
													  btn->SetClass("checked", m_kbSplitActive);
												  }
												  else if (std::string(ub.id) == "btn_patch_load")
												  {
													  m_patchLoadActive = !m_patchLoadActive;
													  if (led)
													  {
														  led->SetClass("led_lit", m_patchLoadActive);
														  led->SetClass("led_unlit", !m_patchLoadActive);
													  }
													  btn->SetClass("checked", m_patchLoadActive);
												  }
												  else
												  {
													  btn->SetClass("active_pulse", true);
												  }
											  });
			}
		}
	}

	void Editor::bindNavigationAndModeButtons()
	{
		struct NavModeBtnDef
		{
			const char* id;
			const char* altId;
			uint8_t col;
			uint8_t row;
		};

		const std::array<NavModeBtnDef, 8> navModeButtons = {{
			{"nav_up", nullptr, rg2::Panel::kNavUpCol, rg2::Panel::kNavUpRow},
			{"nav_down", nullptr, rg2::Panel::kNavDownCol, rg2::Panel::kNavDownRow},
			{"nav_left", nullptr, rg2::Panel::kNavLeftCol, rg2::Panel::kNavLeftRow},
			{"nav_right", nullptr, rg2::Panel::kNavRightCol, rg2::Panel::kNavRightRow},
			{"btn_display_mode", nullptr, rg2::Panel::kDisplayModeCol, rg2::Panel::kDisplayModeRow},
			{"btn_patch_settings", nullptr, rg2::Panel::kPatchSettingsCol, rg2::Panel::kPatchSettingsRow},
			{"btn_morph", nullptr, rg2::Panel::kMorphCol, rg2::Panel::kMorphRow},
			{"btn_sys", "btn_system", rg2::Panel::kSysCol, rg2::Panel::kSysRow},
		}};

		for (const auto& nb : navModeButtons)
		{
			auto* btn = findChild(nb.id, false);
			if (!btn && nb.altId)
				btn = findChild(nb.altId, false);

			if (btn)
			{
				juceRmlUi::EventListener::Add(btn, Rml::EventId::Mousedown,
											  [this, nb](Rml::Event&) { setButtonState(nb.col, nb.row, true); });

				auto releaseBtn = [this, nb](Rml::Event&) { setButtonState(nb.col, nb.row, false); };
				juceRmlUi::EventListener::Add(btn, Rml::EventId::Mouseup, releaseBtn);
				juceRmlUi::EventListener::Add(btn, Rml::EventId::Mouseout, releaseBtn);
				juceRmlUi::EventListener::Add(btn, Rml::EventId::Dragend, releaseBtn);

				juceRmlUi::EventListener::Add(btn, Rml::EventId::Click,
											  [this, btn, nb](Rml::Event&)
											  {
												  triggerButtonPress(nb.row, nb.col);

												  if (std::string(nb.id) == "btn_display_mode")
												  {
													  const bool isChecked = btn->IsClassSet("checked");
													  btn->SetClass("checked", !isChecked);
												  }
												  else if (std::string(nb.id) == "btn_morph")
												  {
													  const bool isChecked = btn->IsClassSet("checked");
													  btn->SetClass("checked", !isChecked);
												  }
												  else if (std::string(nb.id) == "btn_sys")
												  {
													  m_systemActive = !m_systemActive;
													  if (auto* led = findChild("led_system", false))
													  {
														  led->SetClass("led_lit", m_systemActive);
														  led->SetClass("led_unlit", !m_systemActive);
													  }
													  btn->SetClass("checked", m_systemActive);
												  }
												  else
												  {
													  btn->SetClass("active_pulse", true);
												  }
											  });
			}
		}
	}

	void Editor::bindPages()
	{
		const std::array<const char*, 5> btnNames = {"page_btn_a", "page_btn_b", "page_btn_c", "page_btn_d",
													 "page_btn_e"};
		const std::array<const char*, 5> ledNames = {"page_led_a", "page_led_b", "page_led_c", "page_led_d",
													 "page_led_e"};

		for (size_t p = 0; p < 5; ++p)
		{
			m_pageButtons[p] = findChild(btnNames[p], false);
			m_pageLeds[p] = findChild(ledNames[p], false);

			if (m_pageButtons[p])
			{
				juceRmlUi::EventListener::Add(m_pageButtons[p], Rml::EventId::Click,
											  [this, p](Rml::Event&)
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
												  triggerButtonPress(3, static_cast<uint8_t>(p));
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
				juceRmlUi::EventListener::Add(m_varButtons[v], Rml::EventId::Click,
											  [this, v](Rml::Event&)
											  {
												  m_activeVariation = static_cast<uint8_t>(v);
												  for (size_t i = 0; i < 8; ++i)
												  {
													  if (m_varLeds[i])
													  {
														  m_varLeds[i]->SetClass("led_lit", i == m_activeVariation);
														  m_varLeds[i]->SetClass("led_unlit", i != m_activeVariation);
													  }
													  if (m_varButtons[i])
														  m_varButtons[i]->SetClass("checked", i == m_activeVariation);
												  }
												  triggerButtonPress(2, static_cast<uint8_t>(v));
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
			juceRmlUi::EventListener::Add(
				m_octaveDownBtn, Rml::EventId::Click,
				[this](Rml::Event&)
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
					triggerButtonPress(4, 0);
				});
		}

		if (m_octaveUpBtn)
		{
			juceRmlUi::EventListener::Add(
				m_octaveUpBtn, Rml::EventId::Click,
				[this](Rml::Event&)
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
					triggerButtonPress(4, 1);
				});
		}
	}

	void Editor::bindEncoders()
	{
		for (size_t i = 0; i < 8; ++i)
		{
			const std::string knobName = "encoder_" + std::to_string(i);
			const std::string ringName = "ring_" + std::to_string(i);
			const std::string colBtnName = "col_btn_" + std::to_string(i);

			m_encoderKnobs[i] = findChild(knobName, false);
			m_encoderRings[i] = findChild(ringName, false);
			m_colButtons[i] = findChild(colBtnName, false);

			for (size_t j = 0; j < 15; ++j)
			{
				const std::string ringLedName = "r" + std::to_string(i) + "_led" + std::to_string(j);
				m_ringLeds[i][j] = findChild(ringLedName, false);
			}

			auto attachEncoderDrag = [this, i](Rml::Element* _elem)
			{
				if (!_elem)
					return;
				_elem->SetProperty(Rml::PropertyId::Drag, Rml::Style::Drag::Drag);

				juceRmlUi::EventListener::Add(_elem, Rml::EventId::Mousedown,
											  [this, i](Rml::Event& _e)
											  {
												  m_draggingEncoder = static_cast<int>(i);
												  m_encoderDragLastY[i] = _e.GetUnprojectedMouseScreenPos().y;
												  m_encoderAccum[i] = 0.0f;
											  });

				juceRmlUi::EventListener::Add(
					_elem, Rml::EventId::Drag,
					[this, i](Rml::Event& _e)
					{
						if (m_draggingEncoder != static_cast<int>(i))
							return;
						const float curY = _e.GetUnprojectedMouseScreenPos().y;
						const float dy = m_encoderDragLastY[i] - curY;
						m_encoderDragLastY[i] = curY;
						m_encoderAccum[i] += dy;

						constexpr float kPixelsPerTick = 5.0f;
						if (std::abs(m_encoderAccum[i]) >= kPixelsPerTick)
						{
							const int ticks = static_cast<int>(m_encoderAccum[i] / kPixelsPerTick);
							m_encoderAccum[i] -= static_cast<float>(ticks) * kPixelsPerTick;

							if (auto* proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor()))
							{
								if (auto* dev = proc->getG2Device())
								{
									if (auto* board = dev->board())
										board->panel().setEncoderDelta(
											static_cast<uint8_t>(i), static_cast<int8_t>(std::clamp(ticks, -127, 127)));
								}
							}

							m_encoderValues[i] =
								std::clamp(m_encoderValues[i] + static_cast<float>(ticks) * 0.015f, 0.0f, 1.0f);

							if (m_encoderKnobs[i])
							{
								const float angle = m_encoderValues[i] * 300.0f - 150.0f;
								m_encoderKnobs[i]->SetProperty("transform", "rotate(" + std::to_string(angle) + "deg)");
							}

							updateLedRings();
							updateParamLcds();
						}
					});

				auto stopDrag = [this, i](Rml::Event&)
				{
					if (m_draggingEncoder == static_cast<int>(i))
						m_draggingEncoder = -1;
				};
				juceRmlUi::EventListener::Add(_elem, Rml::EventId::Mouseup, stopDrag);
				juceRmlUi::EventListener::Add(_elem, Rml::EventId::Dragend, stopDrag);
			};

			if (m_encoderRings[i])
				attachEncoderDrag(m_encoderRings[i]);

			if (m_colButtons[i])
			{
				juceRmlUi::EventListener::Add(m_colButtons[i], Rml::EventId::Click,
											  [this, i](Rml::Event&)
											  {
												  m_activeCol = static_cast<int>(i);
												  for (size_t c = 0; c < 8; ++c)
												  {
													  if (m_colButtons[c])
														  m_colButtons[c]->SetClass("btn_col_active", c == i);
												  }
												  triggerButtonPress(1, static_cast<uint8_t>(i));
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

		static const std::array<std::array<ParamDesc, 8>, 5> pageParams = {{// Page A: Osc
																			{{{"Freq", "1.05kHz"},
																			  {"Res", "1.15"},
																			  {"Amount", "64.1"},
																			  {"Amount", "77.3"},
																			  {"Amount", "0"},
																			  {"Amount", "0"},
																			  {"Type", "LP"},
																			  {"XFade", "2"}}},
																			// Page B: LFO
																			{{{"Rate", "4.20Hz"},
																			  {"Wave", "Tri"},
																			  {"Poly", "On"},
																			  {"Sync", "Off"},
																			  {"KBT", "50%"},
																			  {"Env", "Off"},
																			  {"Amount", "64.0"},
																			  {"Dest", "Pitch"}}},
																			// Page C: Env
																			{{{"Attack", "12ms"},
																			  {"Decay", "350ms"},
																			  {"Sustain", "70%"},
																			  {"Release", "450ms"},
																			  {"Vel", "85%"},
																			  {"Curve", "Lin"},
																			  {"Invert", "Off"},
																			  {"Retrig", "On"}}},
																			// Page D: Filter
																			{{{"Type", "LP24"},
																			  {"Freq", "2.40kHz"},
																			  {"Res", "3.20"},
																			  {"Drive", "15%"},
																			  {"EnvAmt", "+45"},
																			  {"KBT", "100%"},
																			  {"Attack", "5ms"},
																			  {"Decay", "200ms"}}},
																			// Page E: Effect
																			{{{"Type", "Delay"},
																			  {"Time", "250ms"},
																			  {"Fdbk", "45%"},
																			  {"Tone", "Warm"},
																			  {"Depth", "30%"},
																			  {"Speed", "1.2Hz"},
																			  {"Mix", "35%"},
																			  {"Level", "0.0dB"}}}}};

		const auto& currentParams = pageParams[m_activePage < 5 ? m_activePage : 0];
		for (size_t i = 0; i < 8; ++i)
		{
			if (m_paramTitles[i])
				m_paramTitles[i]->SetInnerRML(Rml::StringUtilities::EncodeRml(currentParams[i].title));
			if (m_paramVals[i])
			{
				const float v = m_encoderValues[i];
				std::string displayVal;
				const std::string title = currentParams[i].title;
				if (title == "Freq")
				{
					const float freq = 20.0f * std::pow(1000.0f, v);
					if (freq < 1000.0f)
					{
						char buf[16];
						std::snprintf(buf, sizeof(buf), "%.0fHz", freq);
						displayVal = buf;
					}
					else
					{
						char buf[16];
						std::snprintf(buf, sizeof(buf), "%.2fkHz", freq / 1000.0f);
						displayVal = buf;
					}
				}
				else if (title == "Res")
				{
					char buf[16];
					std::snprintf(buf, sizeof(buf), "%.2f", 1.0f + v * 9.0f);
					displayVal = buf;
				}
				else if (title == "Rate" || title == "Speed")
				{
					char buf[16];
					std::snprintf(buf, sizeof(buf), "%.2fHz", 0.1f + v * 20.0f);
					displayVal = buf;
				}
				else if (title == "Attack" || title == "Decay" || title == "Release" || title == "Time")
				{
					char buf[16];
					std::snprintf(buf, sizeof(buf), "%.0fms", v * 1000.0f);
					displayVal = buf;
				}
				else if (title == "Level")
				{
					char buf[16];
					std::snprintf(buf, sizeof(buf), "%.1fdB", (v - 1.0f) * 40.0f);
					displayVal = buf;
				}
				else if (title == "Type")
				{
					static const char* types[] = {"LP", "HP", "BP", "Notch"};
					const int idx = std::clamp(static_cast<int>(v * 4.0f), 0, 3);
					displayVal = types[idx];
				}
				else if (title == "Wave")
				{
					static const char* waves[] = {"Tri", "Saw", "Squ", "Sin"};
					const int idx = std::clamp(static_cast<int>(v * 4.0f), 0, 3);
					displayVal = waves[idx];
				}
				else if (title == "Poly" || title == "Sync" || title == "Env" || title == "Invert" || title == "Retrig")
				{
					displayVal = (v >= 0.5f) ? "On" : "Off";
				}
				else
				{
					char buf[16];
					std::snprintf(buf, sizeof(buf), "%.1f", v * 100.0f);
					displayVal = buf;
				}
				m_paramVals[i]->SetInnerRML(Rml::StringUtilities::EncodeRml(displayVal));
			}
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
			const float encoderVal = m_encoderValues[i];

			if (ringState == m_lastRingStates[i] && std::abs(encoderVal - m_lastRingEncoderVals[i]) < 0.001f)
				continue;

			m_lastRingStates[i] = ringState;
			m_lastRingEncoderVals[i] = encoderVal;

			const int activeLedMax = static_cast<int>(std::round(encoderVal * 14.0f));

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

	void Editor::bindParameterListeners()
	{
		const std::array<const char*, 5> paramNames = {"MasterVolume", "ModWheel", "PitchStick", "Aftertouch",
													   "ControlPedal"};

		for (const auto* name : paramNames)
		{
			if (auto* param = m_controller.getParameter(name, 0))
			{
				m_paramListeners.push_back(std::make_unique<baseLib::EventListener<pluginLib::Parameter*>>(
					param->onValueChanged,
					[this](pluginLib::Parameter* _p)
					{
						if (!_p)
							return;
						const auto& desc = _p->getDescription();
						const float norm = _p->getValue();

						if (desc.name == "MasterVolume")
						{
							if (!m_isDraggingVolume)
							{
								m_lastVolume = norm;
								if (m_volumeSlider)
									juceRmlUi::ElemValue::setValue(m_volumeSlider, norm, false);
								updateVolumeDisplay(norm);
							}
						}
						else if (desc.name == "PitchStick")
						{
							if (!m_isDraggingPitchStick && m_pitchStick)
							{
								m_lastPitchStick = norm;
								juceRmlUi::ElemValue::setValue(m_pitchStick, norm, false);
							}
						}
						else if (desc.name == "ModWheel")
						{
							if (!m_isDraggingModWheel && m_modWheel)
							{
								m_lastModWheel = norm;
								juceRmlUi::ElemValue::setValue(m_modWheel, norm, false);
							}
						}
					}));
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
				m_slotLeds[p]->SetClass("lit", active);
				m_slotLeds[p]->SetClass("unlit", !active);
				m_slotLeds[p]->SetClass("led_lit", active);
				m_slotLeds[p]->SetClass("led_unlit", !active);
			}
		}

		if (auto* activeDisplay = findChild("active_slot_display", false))
		{
			const char slotLetter = static_cast<char>('A' + (_part < 4 ? _part : 0));
			const std::string text = std::string("Slot ") + slotLetter;
			activeDisplay->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
		}

		triggerButtonPress(5, _part < 4 ? _part : 0);
	}

	void Editor::timerCallback()
	{
		++m_timerTicks;

		// Decimate socket status update to ~2 Hz (every 16 ticks at 30ms = ~480ms)
		if ((m_timerTicks & 0x0Fu) == 0)
			updateSocketStatus();

		updateLedRings();

		auto* g2Proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		auto* g2Dev = g2Proc ? g2Proc->getG2Device() : nullptr;

		if (g2Dev)
		{
			const float vol = g2Dev->panelControl(rg2::PanelControl::MasterVolume);
			if (!m_isDraggingVolume && std::abs(vol - m_lastVolume) > 0.005f)
			{
				m_lastVolume = vol;
				if (m_volumeSlider)
					juceRmlUi::ElemValue::setValue(m_volumeSlider, vol, false);
				updateVolumeDisplay(vol);
			}

			const float pitch = g2Dev->panelControl(rg2::PanelControl::PitchStick);
			if (m_pitchStick && !m_isDraggingPitchStick && std::abs(pitch - m_lastPitchStick) > 0.005f)
			{
				m_lastPitchStick = pitch;
				juceRmlUi::ElemValue::setValue(m_pitchStick, pitch, false);
			}

			const float mod = g2Dev->panelControl(rg2::PanelControl::ModWheel);
			if (m_modWheel && !m_isDraggingModWheel && std::abs(mod - m_lastModWheel) > 0.005f)
			{
				m_lastModWheel = mod;
				juceRmlUi::ElemValue::setValue(m_modWheel, mod, false);
			}

			if (auto* board = g2Dev->board())
			{
				// Decimate LCD text reading to ~10 Hz (every 3 ticks at 30ms = 90ms)
				// combined with dirty displayVersion tracking in Panel
				if ((m_timerTicks % 3u) == 0)
					updateLcdDisplays(board->panel());

				updatePanelLeds(board->latches());

				for (auto it = m_pressedButtons.begin(); it != m_pressedButtons.end();)
				{
					if (--(it->ticks) <= 0)
					{
						board->panel().setButtonPressed(it->row, it->col, false);
						it = m_pressedButtons.erase(it);
					}
					else
					{
						++it;
					}
				}
			}
		}
	}

	void Editor::updateLcdDisplays(const rg2::Panel& _panel)
	{
		const uint32_t currentVersion = _panel.displayVersion();
		if (currentVersion == m_lastDisplayVersion)
			return;
		m_lastDisplayVersion = currentVersion;

		// Main Patch Display: 2 lines of 16 characters at CS4 offsets 0x00 and 0x10
		const std::string line1Raw = _panel.readDisplayText(0x00u, 16u);
		const std::string line2Raw = _panel.readDisplayText(0x10u, 16u);

		std::string line1;
		if (extractLcdText(line1Raw, line1))
		{
			if (line1 != m_lastMainLcd1)
			{
				m_lastMainLcd1 = line1;
				if (m_mainLcdLine1)
					m_mainLcdLine1->SetInnerRML(Rml::StringUtilities::EncodeRml(line1));
			}
		}

		std::string line2;
		if (extractLcdText(line2Raw, line2))
		{
			if (line2 != m_lastMainLcd2)
			{
				m_lastMainLcd2 = line2;
				if (m_mainLcdLine2)
					m_mainLcdLine2->SetInnerRML(Rml::StringUtilities::EncodeRml(line2));
			}
		}

		// Parameter LCDs 1..4: 4 LCDs across 8 encoders (2 encoders per LCD)
		// Offset map: LCD (i / 2) sits at 0x20 + (i / 2) * 0x20
		// Line 1: Title (8 chars at base + (i % 2) * 8)
		// Line 2: Value (8 chars at base + 16 + (i % 2) * 8)
		for (size_t i = 0; i < 8; ++i)
		{
			const uint32_t lcdBase = 0x20u + static_cast<uint32_t>((i / 2) * 0x20u);
			const uint32_t titleOffset = lcdBase + static_cast<uint32_t>((i % 2) * 8u);
			const uint32_t valOffset = lcdBase + 16u + static_cast<uint32_t>((i % 2) * 8u);

			const std::string titleRaw = _panel.readDisplayText(titleOffset, 8u);
			const std::string valRaw = _panel.readDisplayText(valOffset, 8u);

			std::string title;
			if (extractLcdText(titleRaw, title))
			{
				if (title != m_lastParamTitles[i])
				{
					m_lastParamTitles[i] = title;
					if (m_paramTitles[i])
						m_paramTitles[i]->SetInnerRML(Rml::StringUtilities::EncodeRml(title));
				}
			}

			std::string val;
			if (extractLcdText(valRaw, val))
			{
				if (val != m_lastParamVals[i])
				{
					m_lastParamVals[i] = val;
					if (m_paramVals[i])
						m_paramVals[i]->SetInnerRML(Rml::StringUtilities::EncodeRml(val));
				}
			}
		}
	}

	void Editor::updatePanelLeds(const rg2::Latches& _latches)
	{
		const uint8_t latch0 = _latches.getLatch(0);
		if (latch0 == m_lastLatch0)
			return;

		const uint8_t ledBits = latch0 & 0xCFu;
		if (m_lastLatch0 == 0xFF && ledBits == 0)
			return;

		m_lastLatch0 = latch0;

		const uint8_t slotMask = latch0 & 0x0Fu;
		for (size_t p = 0; p < m_slotLeds.size(); ++p)
		{
			const bool lit = (slotMask & (1u << p)) != 0;
			if (m_slotLeds[p])
			{
				m_slotLeds[p]->SetClass("slot_led_lit", lit);
				m_slotLeds[p]->SetClass("slot_led_unlit", !lit);
				m_slotLeds[p]->SetClass("lit", lit);
				m_slotLeds[p]->SetClass("unlit", !lit);
				m_slotLeds[p]->SetClass("led_lit", lit);
				m_slotLeds[p]->SetClass("led_unlit", !lit);
			}
			if (m_slotButtons[p])
				m_slotButtons[p]->setChecked(lit);
		}

		if (slotMask != 0)
		{
			if (auto* activeDisplay = findChild("active_slot_display", false))
			{
				for (size_t p = 0; p < 4; ++p)
				{
					if (slotMask & (1u << p))
					{
						const char slotLetter = static_cast<char>('A' + p);
						const std::string text = std::string("Slot ") + slotLetter;
						activeDisplay->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
						break;
					}
				}
			}
		}

		const bool var1Lit = (latch0 & 0x40u) != 0;
		const bool var2Lit = (latch0 & 0x80u) != 0;

		if (var1Lit || var2Lit)
		{
			const uint8_t activeVar = var1Lit ? 0 : 1;
			m_activeVariation = activeVar;
			for (size_t i = 0; i < 8; ++i)
			{
				const bool lit = (i == activeVar);
				if (m_varLeds[i])
				{
					m_varLeds[i]->SetClass("led_lit", lit);
					m_varLeds[i]->SetClass("led_unlit", !lit);
					m_varLeds[i]->SetClass("lit", lit);
					m_varLeds[i]->SetClass("unlit", !lit);
				}
				if (m_varButtons[i])
					m_varButtons[i]->SetClass("checked", lit);
			}
		}
		else
		{
			for (size_t i = 0; i < 2; ++i)
			{
				if (m_varLeds[i])
				{
					m_varLeds[i]->SetClass("led_lit", false);
					m_varLeds[i]->SetClass("led_unlit", true);
					m_varLeds[i]->SetClass("lit", false);
					m_varLeds[i]->SetClass("unlit", true);
				}
				if (m_varButtons[i])
					m_varButtons[i]->SetClass("checked", false);
			}
		}
	}

	void Editor::updateSocketStatus()
	{
		auto* g2Proc = dynamic_cast<AudioPluginAudioProcessor*>(&getProcessor());
		auto* g2Dev = g2Proc ? g2Proc->getG2Device() : nullptr;
		const auto* wsServer = g2Dev ? g2Dev->webSocketServer() : nullptr;

		const bool wsListening = (wsServer && wsServer->port() > 0);
		const bool wsConnected = (wsServer && wsServer->hasClient());

		int state = 0;
		if (wsConnected)
			state = 2;
		else if (wsListening)
			state = 1;

		std::string text;
		if (wsConnected)
		{
			text = "127.0.0.1:" + std::to_string(wsServer->port()) +
				" (Connected) [Rx: " + std::to_string(wsServer->framesIn()) +
				" Tx: " + std::to_string(wsServer->framesOut()) + "]";
		}
		else if (wsListening)
		{
			text = "127.0.0.1:" + std::to_string(wsServer->port()) + " (Listening)";
		}
		else
		{
			text = "Bridge: Off (Port busy/unavailable)";
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
				const char* badgeText = (state == 2 ? "ONLINE" : (state == 1 ? "LISTENING" : "PORT BUSY"));
				badgeElem->SetInnerRML(badgeText);
			}
		}

		if (text != m_lastSocketStatusText)
		{
			m_lastSocketStatusText = text;
			if (auto* textElem = findChild("socket_text", false))
				textElem->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
			else if (auto* statusElem = findChild("socket_status", false))
				statusElem->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
		}
	}

	std::pair<std::string, std::string> Editor::getDemoRestrictionText() const { return {}; }
} // namespace rg2JucePlugin

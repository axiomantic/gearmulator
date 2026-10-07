#pragma once

#include "jucePluginEditorLib/partbutton.h"
#include "jucePluginEditorLib/pluginEditor.h"

#include <juce_events/juce_events.h>

#include "baseLib/event.h"
#include "jucePluginLib/parameter.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace rg2
{
	class Panel;
	class Latches;
} // namespace rg2

namespace rg2JucePlugin
{
	class Controller;
	class Editor;

	class SlotButton : public jucePluginEditorLib::PartButton
	{
	public:
		SlotButton(Rml::Element* _button, Editor& _editor);
		void onClick(Rml::Event& _event) override;

	private:
		Editor& m_editor;
	};

	class Editor final : public jucePluginEditorLib::Editor, private juce::Timer
	{
	public:
		Editor(jucePluginEditorLib::Processor& _processor, const jucePluginEditorLib::Skin& _skin);
		~Editor() override;

		Editor(Editor&&) = delete;
		Editor(const Editor&) = delete;
		Editor& operator=(Editor&&) = delete;
		Editor& operator=(const Editor&) = delete;

		void create() override;

		std::pair<std::string, std::string> getDemoRestrictionText() const override;

		void triggerButtonPress(uint8_t _row, uint8_t _col);
		void setButtonState(uint8_t _col, uint8_t _row, bool _isDown);

	protected:
		void onCurrentPartChanged(uint8_t _part) override;

	private:
		void timerCallback() override;
		void updateSocketStatus();
		void bindPortControls();
		void bindVolumeControl();
		void updateVolumeDisplay(float _volume);

		void bindFrontPanel();
		void bindNavigationAndModeButtons();
		void bindEncoders();
		void bindPages();
		void bindVariations();
		void bindOctaveShift();
		void bindPerformanceControls();
		void bindParameterListeners();
		void updateLedRings();
		void updateParamLcds();
		void updateLcdDisplays(const rg2::Panel& _panel);
		void updatePanelLeds(const rg2::Latches& _latches);

		struct PressedButton
		{
			uint8_t row;
			uint8_t col;
			int ticks;
		};

		Controller& m_controller;
		std::vector<PressedButton> m_pressedButtons;

		std::array<std::unique_ptr<SlotButton>, 4> m_slotButtons{};
		std::array<Rml::Element*, 4> m_slotLeds{};

		Rml::Element* m_volumeSlider = nullptr;
		Rml::Element* m_volumeKnobFrame = nullptr;
		Rml::Element* m_volumeDisplay = nullptr;
		Rml::Element* m_volumeDbDisplay = nullptr;
		float m_lastVolume = -1.0f;
		bool m_isDraggingVolume = false;
		float m_volumeDragStartY = 0.0f;
		float m_volumeDragStartVal = 1.0f;

		std::string m_lastSocketStatusText;
		int m_lastSocketState = -1;

		Rml::Element* m_inputPort = nullptr;
		Rml::Element* m_btnSetPort = nullptr;
		Rml::Element* m_btnFindPort = nullptr;

		std::array<Rml::Element*, 8> m_encoderKnobs{};
		std::array<Rml::Element*, 8> m_encoderRings{};
		std::array<Rml::Element*, 8> m_colButtons{};
		std::array<std::array<Rml::Element*, 15>, 8> m_ringLeds{};
		std::array<float, 8> m_encoderValues{0.35f, 0.45f, 0.50f, 0.60f, 0.20f, 0.20f, 0.40f, 0.70f};
		std::array<float, 8> m_encoderDragLastY{};
		std::array<float, 8> m_encoderAccum{};
		int m_draggingEncoder = -1;

		std::array<Rml::Element*, 5> m_pageButtons{};
		std::array<Rml::Element*, 5> m_pageLeds{};
		uint8_t m_activePage = 0;

		std::array<Rml::Element*, 8> m_varButtons{};
		std::array<Rml::Element*, 8> m_varLeds{};
		uint8_t m_activeVariation = 0;

		Rml::Element* m_octaveDownBtn = nullptr;
		Rml::Element* m_octaveUpBtn = nullptr;
		std::array<Rml::Element*, 4> m_octaveLeds{};
		int m_octaveShift = 0;

		std::array<Rml::Element*, 8> m_paramTitles{};
		std::array<Rml::Element*, 8> m_paramVals{};
		Rml::Element* m_mainLcdLine1 = nullptr;
		Rml::Element* m_mainLcdLine2 = nullptr;

		std::string m_lastMainLcd1;
		std::string m_lastMainLcd2;
		std::array<std::string, 8> m_lastParamTitles{};
		std::array<std::string, 8> m_lastParamVals{};
		uint32_t m_lastDisplayVersion = 0xFFFFFFFFu;
		uint32_t m_timerTicks = 0;
		std::array<uint16_t, 8> m_lastRingStates{0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu,
												 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
		std::array<float, 8> m_lastRingEncoderVals{-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
		uint8_t m_lastLatch0 = 0xFF;

		Rml::Element* m_pitchStick = nullptr;
		bool m_isDraggingPitchStick = false;
		float m_pitchStickDragLastY = 0.0f;
		float m_lastPitchStick = 0.5f;

		Rml::Element* m_modWheel = nullptr;
		bool m_isDraggingModWheel = false;
		float m_modWheelDragLastY = 0.0f;
		float m_lastModWheel = 0.0f;

		Rml::Element* m_dataWheel = nullptr;
		bool m_isDraggingDataWheel = false;
		float m_dataWheelDragLastY = 0.0f;
		float m_dataWheelAccum = 0.0f;
		float m_dataWheelAngle = 0.0f;

		bool m_systemActive = false;
		bool m_patchActive = true;
		bool m_shiftActive = false;
		bool m_kbSplitActive = false;
		bool m_patchLoadActive = false;
		int m_activeCol = 0;

		std::vector<std::unique_ptr<baseLib::EventListener<pluginLib::Parameter*>>> m_paramListeners;
	};
} // namespace rg2JucePlugin

#pragma once

#include "jucePluginEditorLib/partbutton.h"
#include "jucePluginEditorLib/pluginEditor.h"

#include <juce_events/juce_events.h>

#include <array>
#include <memory>
#include <string>

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

	protected:
		void onCurrentPartChanged(uint8_t _part) override;

	private:
		void timerCallback() override;
		void updateSocketStatus();
		void bindVolumeControl();
		void updateVolumeDisplay(float _volume);

		void bindFrontPanel();
		void bindEncoders();
		void bindPages();
		void bindVariations();
		void bindOctaveShift();
		void bindPerformanceControls();
		void updateLedRings();
		void updateParamLcds();

		Controller& m_controller;
		std::array<std::unique_ptr<SlotButton>, 4> m_slotButtons{};
		std::array<Rml::Element*, 4> m_slotLeds{};
		Rml::Element* m_volumeSlider = nullptr;
		Rml::Element* m_volumeDisplay = nullptr;
		Rml::Element* m_volumeDbDisplay = nullptr;
		float m_lastVolume = -1.0f;
		std::string m_lastSocketStatusText;
		int m_lastSocketState = -1;

		std::array<Rml::Element*, 8> m_encoderKnobs{};
		std::array<Rml::Element*, 8> m_colButtons{};
		std::array<std::array<Rml::Element*, 15>, 8> m_ringLeds{};
		std::array<float, 8> m_encoderValues{0.35f, 0.45f, 0.50f, 0.60f, 0.20f, 0.20f, 0.40f, 0.70f};

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

		Rml::Element* m_pitchStick = nullptr;
		Rml::Element* m_modWheel = nullptr;
	};
} // namespace rg2JucePlugin

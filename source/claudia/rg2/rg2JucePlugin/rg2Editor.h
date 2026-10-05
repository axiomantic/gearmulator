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

		Controller& m_controller;
		std::array<std::unique_ptr<SlotButton>, 4> m_slotButtons{};
		std::string m_lastSocketStatusText;
		int m_lastSocketState = -1;
	};
} // namespace rg2JucePlugin

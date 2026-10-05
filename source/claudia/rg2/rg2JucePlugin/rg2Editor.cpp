#include "rg2Editor.h"

#include "rg2Controller.h"
#include "rg2Device.h"
#include "rg2PluginProcessor.h"

#include "transportSocket.h"

#include "RmlUi/Core/StringUtilities.h"

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

		onCurrentPartChanged(m_controller.getCurrentPart());

		updateSocketStatus();
		startTimer(250);
	}

	void Editor::onCurrentPartChanged(const uint8_t _part)
	{
		jucePluginEditorLib::Editor::onCurrentPartChanged(_part);

		for (size_t p = 0; p < m_slotButtons.size(); ++p)
		{
			if (m_slotButtons[p])
				m_slotButtons[p]->setChecked(static_cast<uint8_t>(p) == _part);
		}

		if (auto* activeDisplay = findChild("active_slot_display", false))
		{
			const char slotLetter = static_cast<char>('A' + (_part < 4 ? _part : 0));
			const std::string text = std::string("Slot ") + slotLetter;
			activeDisplay->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
		}
	}

	void Editor::timerCallback() { updateSocketStatus(); }

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
				text = "Socket: 127.0.0.1:" + std::to_string(port) + " (Connected) [Rx: " + std::to_string(in) +
					" Tx: " + std::to_string(out) + "]";
			}
			else
			{
				state = 1;
				text = "Socket: 127.0.0.1:" + std::to_string(port) + " (Listening)";
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

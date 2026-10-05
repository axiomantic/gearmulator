#pragma once

#include "jucePluginEditorLib/pluginEditor.h"

namespace g2JucePlugin
{
    class Controller;

    class Editor final : public jucePluginEditorLib::Editor
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

    private:
        Controller& m_controller;
    };
} // namespace g2JucePlugin

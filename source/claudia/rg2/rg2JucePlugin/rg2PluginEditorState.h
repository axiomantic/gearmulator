#pragma once

#include "jucePluginEditorLib/pluginEditorState.h"

namespace rg2JucePlugin
{
    class AudioPluginAudioProcessor;

    class PluginEditorState : public jucePluginEditorLib::PluginEditorState
    {
    public:
        explicit PluginEditorState(AudioPluginAudioProcessor& _processor);

    private:
        jucePluginEditorLib::Editor* createEditor(const jucePluginEditorLib::Skin& _skin) override;
    };
} // namespace rg2JucePlugin

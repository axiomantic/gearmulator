#pragma once

#include "jucePluginEditorLib/pluginEditorState.h"

namespace g2JucePlugin
{
    class AudioPluginAudioProcessor;

    class PluginEditorState : public jucePluginEditorLib::PluginEditorState
    {
    public:
        explicit PluginEditorState(AudioPluginAudioProcessor& _processor);

    private:
        jucePluginEditorLib::Editor* createEditor(const jucePluginEditorLib::Skin& _skin) override;
    };
} // namespace g2JucePlugin

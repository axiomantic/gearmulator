#include "g2PluginEditorState.h"

#include "g2Editor.h"
#include "g2PluginProcessor.h"

#include "skins.h"

namespace g2JucePlugin
{
    PluginEditorState::PluginEditorState(AudioPluginAudioProcessor& _processor) :
        jucePluginEditorLib::PluginEditorState(_processor, _processor.getController(), g_includedSkins)
    {
        loadDefaultSkin();
    }

    jucePluginEditorLib::Editor* PluginEditorState::createEditor(const jucePluginEditorLib::Skin& _skin)
    {
        return new Editor(m_processor, _skin);
    }
} // namespace g2JucePlugin

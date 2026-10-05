#include "rg2PluginEditorState.h"

#include "rg2Editor.h"
#include "rg2PluginProcessor.h"

#include "skins.h"

namespace rg2JucePlugin
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
} // namespace rg2JucePlugin

#include "rg2Editor.h"

#include "rg2Controller.h"

#include "jucePluginEditorLib/pluginProcessor.h"

namespace rg2JucePlugin
{
    Editor::Editor(jucePluginEditorLib::Processor& _processor, const jucePluginEditorLib::Skin& _skin) :
        jucePluginEditorLib::Editor(_processor, _skin),
        m_controller(dynamic_cast<Controller&>(_processor.getController()))
    {
    }

    Editor::~Editor() = default;

    void Editor::create() { jucePluginEditorLib::Editor::create(); }

    std::pair<std::string, std::string> Editor::getDemoRestrictionText() const { return {}; }
} // namespace rg2JucePlugin

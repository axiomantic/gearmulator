cmake_minimum_required(VERSION 3.15)

project(rg2JucePlugin VERSION ${CMAKE_PROJECT_VERSION})

set(SOURCES
	rg2Controller.cpp rg2Controller.h
	rg2Device.cpp rg2Device.h
	rg2Editor.cpp rg2Editor.h
	rg2Parameters.cpp rg2Parameters.h
	rg2PatchLoad.cpp rg2PatchLoad.h
	rg2Plugin.cpp rg2Plugin.h
	rg2PluginEditorState.cpp rg2PluginEditorState.h
	rg2PluginProcessor.cpp rg2PluginProcessor.h
	rg2State.cpp rg2State.h
	parameterDescriptions_rg2.json

	skins/rg2Default/rg2Default.json
	skins/rg2Default/rg2Default.rml
	skins/rg2Default/rg2Default.rcss
)

set(ASSETS "parameterDescriptions_rg2.json")

addSkin("RedGecko2" "rg2Default" "skins/rg2Default" "rg2Default.rml")

buildSkinHeader()

juce_add_binary_data(rg2JucePlugin_BinaryData SOURCES ${ASSETS} ${ASSETS_SKINS})

createJucePlugin(rg2JucePlugin "RedGecko2" TRUE "Trg2" rg2JucePlugin_BinaryData rg2Lib)
target_sources(RedGecko2ServerPlugin PRIVATE rg2Device.cpp rg2Parameters.cpp rg2PatchLoad.cpp rg2State.cpp)

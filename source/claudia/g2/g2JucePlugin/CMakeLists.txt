cmake_minimum_required(VERSION 3.15)

project(g2JucePlugin VERSION ${CMAKE_PROJECT_VERSION})

set(SOURCES
	g2Controller.cpp g2Controller.h
	g2Device.cpp g2Device.h
	g2Editor.cpp g2Editor.h
	g2Parameters.cpp g2Parameters.h
	g2PatchLoad.cpp g2PatchLoad.h
	g2Plugin.cpp g2Plugin.h
	g2PluginEditorState.cpp g2PluginEditorState.h
	g2PluginProcessor.cpp g2PluginProcessor.h
	g2State.cpp g2State.h
	parameterDescriptions_g2.json

	skins/g2Default/g2Default.json
	skins/g2Default/g2Default.rml
	skins/g2Default/g2Default.rcss
)

set(ASSETS "parameterDescriptions_g2.json")

addSkin("RedGecko2" "g2Default" "skins/g2Default" "g2Default.rml")

buildSkinHeader()

juce_add_binary_data(g2JucePlugin_BinaryData SOURCES ${ASSETS} ${ASSETS_SKINS})

createJucePlugin(g2JucePlugin "RedGecko2" TRUE "Trg2" g2JucePlugin_BinaryData g2Lib)

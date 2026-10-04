cmake_minimum_required(VERSION 3.15)

project(rg2WineProxy)

add_library(rg2WineProxy_core STATIC
	rg2usb.h
	rg2usb.cpp
	setupapi_proxy.h
	setupapi_proxy.cpp)

target_include_directories(rg2WineProxy_core PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/../rg2Lib)

target_link_libraries(rg2WineProxy_core PUBLIC rg2Lib)
set_property(TARGET rg2WineProxy_core PROPERTY FOLDER "RG2")

if(WIN32 OR MINGW)
	add_library(rg2usb SHARED
		rg2usb.h
		rg2usb.cpp
		setupapi_proxy.h
		setupapi_proxy.cpp)

	target_include_directories(g2usb PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/../rg2Lib)

	target_link_libraries(g2usb PRIVATE
		ws2_32
		setupapi)

	set_property(TARGET g2usb PROPERTY FOLDER "RG2")

	add_library(setupapi_wine SHARED
		rg2usb.h
		rg2usb.cpp
		setupapi_wine.cpp
		setupapi.def)

	target_include_directories(setupapi_wine PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/../rg2Lib)

	target_link_libraries(setupapi_wine PRIVATE
		ws2_32
		setupapi)

	set_target_properties(setupapi_wine PROPERTIES
		OUTPUT_NAME "setupapi"
		PREFIX ""
		FOLDER "RG2")
endif()

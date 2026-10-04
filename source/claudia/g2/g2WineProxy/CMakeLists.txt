cmake_minimum_required(VERSION 3.15)

project(g2WineProxy)

add_library(g2WineProxy_core STATIC
	g2usb.h
	g2usb.cpp
	setupapi_proxy.h
	setupapi_proxy.cpp)

target_include_directories(g2WineProxy_core PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/../g2Lib)

target_link_libraries(g2WineProxy_core PUBLIC g2Lib)
set_property(TARGET g2WineProxy_core PROPERTY FOLDER "G2")

if(WIN32 OR MINGW)
	add_library(g2usb SHARED
		g2usb.h
		g2usb.cpp
		setupapi_proxy.h
		setupapi_proxy.cpp)

	target_include_directories(g2usb PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/../g2Lib)

	target_link_libraries(g2usb PRIVATE
		ws2_32
		setupapi)

	set_property(TARGET g2usb PROPERTY FOLDER "G2")
endif()

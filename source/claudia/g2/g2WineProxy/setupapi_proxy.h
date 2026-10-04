#pragma once

#include <cstdint>
#include <string>

namespace g2::wine
{
	/* Checks whether the specified GUID string matches the Clavia G2 interface GUID. */
	bool isClaviaDeviceGuid(const char* guidString) noexcept;

	/* Checks whether raw GUID bytes match {CB3ED981-6125-4047-BC2A-292E370CC89A}. */
	bool isClaviaDeviceGuid(const void* guidBytes) noexcept;

	/* Formats and returns the virtual device path. */
	const char* getVirtualDevicePath() noexcept;
} // namespace g2::wine

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <setupapi.h>

extern "C" {
__declspec(dllexport) HDEVINFO WINAPI Hook_SetupDiGetClassDevsA(const GUID* ClassGuid, PCSTR Enumerator,
																HWND hwndParent, DWORD Flags);

__declspec(dllexport) BOOL WINAPI Hook_SetupDiEnumDeviceInterfaces(HDEVINFO DeviceInfoSet,
																   PSP_DEVINFO_DATA DeviceInfoData,
																   const GUID* InterfaceClassGuid, DWORD MemberIndex,
																   PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData);

__declspec(dllexport) BOOL WINAPI Hook_SetupDiGetDeviceInterfaceDetailA(
	HDEVINFO DeviceInfoSet, PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
	PSP_DEVICE_INTERFACE_DETAIL_DATA_A DeviceInterfaceDetailData, DWORD DeviceInterfaceDetailDataSize,
	PDWORD RequiredSize, PSP_DEVINFO_DATA DeviceInfoData);

__declspec(dllexport) BOOL WINAPI Hook_SetupDiDestroyDeviceInfoList(HDEVINFO DeviceInfoSet);
}
#endif

#include "setupapi_proxy.h"
#include "rg2usb.h"

#include <cctype>
#include <cstring>

namespace rg2::wine
{
	namespace
	{
		// {CB3ED981-6125-4047-BC2A-292E370CC89A}
		constexpr uint8_t kClaviaGuidBytes[16] = {
			0x81, 0xD9, 0x3E, 0xCB, // Data1 (LE)
			0x25, 0x61, // Data2 (LE)
			0x47, 0x40, // Data3 (LE)
			0xBC, 0x2A, 0x29, 0x2E, 0x37, 0x0C, 0xC8, 0x9A // Data4
		};

		bool strCaseEqual(const char* a, const char* b) noexcept
		{
			if (a == nullptr || b == nullptr)
				return false;
			while (*a && *b)
			{
				if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
					return false;
				++a;
				++b;
			}
			return *a == *b;
		}
	} // namespace

	bool isClaviaDeviceGuid(const char* const guidString) noexcept
	{
		if (guidString == nullptr)
			return false;
		return strCaseEqual(guidString, kClaviaDeviceGuidString);
	}

	bool isClaviaDeviceGuid(const void* const guidBytes) noexcept
	{
		if (guidBytes == nullptr)
			return false;
		return std::memcmp(guidBytes, kClaviaGuidBytes, 16) == 0;
	}

	const char* getVirtualDevicePath() noexcept { return kVirtualDevicePath; }
} // namespace rg2::wine

#ifdef _WIN32
namespace
{
	constexpr uintptr_t kVirtualHdevInfo = 0x47324445; // 'G2DE'

	HMODULE getRealSetupApi()
	{
		static HMODULE s_mod = []()
		{
			char sysPath[MAX_PATH];
			::GetSystemDirectoryA(sysPath, MAX_PATH);
			std::strcat(sysPath, "\\setupapi.dll");
			return ::LoadLibraryA(sysPath);
		}();
		return s_mod;
	}
} // namespace

extern "C" {
HDEVINFO WINAPI Hook_SetupDiGetClassDevsA(const GUID* ClassGuid, PCSTR Enumerator, HWND hwndParent, DWORD Flags)
{
	if (ClassGuid != nullptr && rg2::wine::isClaviaDeviceGuid(ClassGuid))
	{
		return reinterpret_cast<HDEVINFO>(kVirtualHdevInfo);
	}

	HMODULE h = getRealSetupApi();
	if (h)
	{
		using PFN = HDEVINFO(WINAPI*)(const GUID*, PCSTR, HWND, DWORD);
		auto pfn = reinterpret_cast<PFN>(::GetProcAddress(h, "SetupDiGetClassDevsA"));
		if (pfn)
			return pfn(ClassGuid, Enumerator, hwndParent, Flags);
	}
	return INVALID_HANDLE_VALUE;
}

BOOL WINAPI Hook_SetupDiEnumDeviceInterfaces(HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData,
											 const GUID* InterfaceClassGuid, DWORD MemberIndex,
											 PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData)
{
	if (reinterpret_cast<uintptr_t>(DeviceInfoSet) == kVirtualHdevInfo)
	{
		if (MemberIndex != 0 || DeviceInterfaceData == nullptr)
		{
			::SetLastError(ERROR_NO_MORE_ITEMS);
			return FALSE;
		}

		DeviceInterfaceData->cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
		std::memcpy(&DeviceInterfaceData->InterfaceClassGuid, InterfaceClassGuid, sizeof(GUID));
		DeviceInterfaceData->Flags = SPINT_ACTIVE;
		DeviceInterfaceData->Reserved = 0;
		return TRUE;
	}

	HMODULE h = getRealSetupApi();
	if (h)
	{
		using PFN = BOOL(WINAPI*)(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD, PSP_DEVICE_INTERFACE_DATA);
		auto pfn = reinterpret_cast<PFN>(::GetProcAddress(h, "SetupDiEnumDeviceInterfaces"));
		if (pfn)
			return pfn(DeviceInfoSet, DeviceInfoData, InterfaceClassGuid, MemberIndex, DeviceInterfaceData);
	}
	return FALSE;
}

BOOL WINAPI Hook_SetupDiGetDeviceInterfaceDetailA(HDEVINFO DeviceInfoSet, PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
												  PSP_DEVICE_INTERFACE_DETAIL_DATA_A DeviceInterfaceDetailData,
												  DWORD DeviceInterfaceDetailDataSize, PDWORD RequiredSize,
												  PSP_DEVINFO_DATA DeviceInfoData)
{
	if (reinterpret_cast<uintptr_t>(DeviceInfoSet) == kVirtualHdevInfo)
	{
		const size_t pathLen = std::strlen(rg2::wine::kVirtualDevicePath) + 1;
		const DWORD needed = static_cast<DWORD>(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) + pathLen);

		if (RequiredSize)
			*RequiredSize = needed;

		if (DeviceInterfaceDetailData == nullptr || DeviceInterfaceDetailDataSize < needed)
		{
			::SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}

		std::memcpy(DeviceInterfaceDetailData->DevicePath, rg2::wine::kVirtualDevicePath, pathLen);
		return TRUE;
	}

	HMODULE h = getRealSetupApi();
	if (h)
	{
		using PFN = BOOL(WINAPI*)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD,
								  PDWORD, PSP_DEVINFO_DATA);
		auto pfn = reinterpret_cast<PFN>(::GetProcAddress(h, "SetupDiGetDeviceInterfaceDetailA"));
		if (pfn)
			return pfn(DeviceInfoSet, DeviceInterfaceData, DeviceInterfaceDetailData, DeviceInterfaceDetailDataSize,
					   RequiredSize, DeviceInfoData);
	}
	return FALSE;
}

BOOL WINAPI Hook_SetupDiDestroyDeviceInfoList(HDEVINFO DeviceInfoSet)
{
	if (reinterpret_cast<uintptr_t>(DeviceInfoSet) == kVirtualHdevInfo)
	{
		return TRUE;
	}

	HMODULE h = getRealSetupApi();
	if (h)
	{
		using PFN = BOOL(WINAPI*)(HDEVINFO);
		auto pfn = reinterpret_cast<PFN>(::GetProcAddress(h, "SetupDiDestroyDeviceInfoList"));
		if (pfn)
			return pfn(DeviceInfoSet);
	}
	return FALSE;
}
}
#endif

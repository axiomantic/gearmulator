#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <setupapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "g2usb.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <string>
#include <vector>

namespace
{
	constexpr uintptr_t kVirtualHdevInfo = 0x47324445; // 'G2DE'
	const HANDLE kVirtualG2Handle = (HANDLE)(uintptr_t)0x47325553; // 'G2US'

	// {CB3ED981-6125-4047-BC2A-292E370CC89A}
	constexpr uint8_t kClaviaGuidBytes[16] = {
		0x81, 0xD9, 0x3E, 0xCB, 0x25, 0x61, 0x47, 0x40,
		0xBC, 0x2A, 0x29, 0x2E, 0x37, 0x0C, 0xC8, 0x9A
	};

	constexpr const char* kVirtualDevicePath =
		"\\\\?\\usb#vid_0ffc&pid_0002#clavia_g2_virtual#{cb3ed981-6125-4047-bc2a-292e370cc89a}";

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

	bool isClaviaGuid(const GUID* guid)
	{
		if (!guid) return false;
		return std::memcmp(guid, kClaviaGuidBytes, 16) == 0;
	}

	bool isClaviaPath(LPCSTR path)
	{
		if (!path) return false;
		std::string s(path);
		for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return (s.find("vid_0ffc&pid_0002") != std::string::npos ||
		        s.find("cb3ed981-6125-4047-bc2a-292e370cc89a") != std::string::npos);
	}

	using PfnCreateFileA = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
	using PfnDeviceIoControl = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
	using PfnReadFile = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
	using PfnWriteFile = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
	using PfnCloseHandle = BOOL(WINAPI*)(HANDLE);

	PfnCreateFileA g_realCreateFileA = nullptr;
	PfnDeviceIoControl g_realDeviceIoControl = nullptr;
	PfnReadFile g_realReadFile = nullptr;
	PfnWriteFile g_realWriteFile = nullptr;
	PfnCloseHandle g_realCloseHandle = nullptr;

	g2::wine::G2UsbDeviceSession g_session;
	std::atomic<bool> g_pumpRunning{false};
	HANDLE g_pumpThread = NULL;

	DWORD WINAPI pumpThreadFunc(LPVOID)
	{
		while (g_pumpRunning.load())
		{
			if (g_session.isConnected())
			{
				g_session.pumpSocket(20);
			}
			else
			{
				Sleep(25);
			}
		}
		return 0;
	}

	HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
	                              LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
	                              DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
	{
		if (isClaviaPath(lpFileName))
		{
			if (!g_session.isConnected())
			{
				if (!g_session.connectToHub("127.0.0.1", 7777))
				{
					SetLastError(ERROR_DEV_NOT_EXIST);
					return INVALID_HANDLE_VALUE;
				}
				if (!g_pumpRunning.load())
				{
					g_pumpRunning.store(true);
					g_pumpThread = CreateThread(NULL, 0, pumpThreadFunc, NULL, 0, NULL);
				}
			}
			return kVirtualG2Handle;
		}

		if (g_realCreateFileA)
			return g_realCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
			                         dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
		return CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
		                   dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
	}

	BOOL WINAPI Hook_DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer,
	                                DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize,
	                                LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped)
	{
		if (hDevice == kVirtualG2Handle)
		{
			if (dwIoControlCode == 0x222000) // Register event
			{
				if (lpInBuffer && nInBufferSize >= sizeof(HANDLE))
				{
					HANDLE hEvent = *reinterpret_cast<HANDLE*>(lpInBuffer);
					g_session.registerEvent(reinterpret_cast<uintptr_t>(hEvent));
					if (lpBytesReturned) *lpBytesReturned = 0;
					return TRUE;
				}
				return FALSE;
			}
			if (dwIoControlCode == 0x222004) // Unregister event
			{
				g_session.unregisterEvent();
				if (lpBytesReturned) *lpBytesReturned = 0;
				return TRUE;
			}
			if (dwIoControlCode == 0x222008) // Query event
			{
				bool reg = false;
				g_session.queryEvent(reg);
				if (lpOutBuffer && nOutBufferSize >= sizeof(DWORD))
					*reinterpret_cast<DWORD*>(lpOutBuffer) = reg ? 1 : 0;
				if (lpBytesReturned) *lpBytesReturned = sizeof(DWORD);
				return TRUE;
			}
			if (dwIoControlCode == 0x22200C) // Get notification (16-byte record)
			{
				if (lpOutBuffer && nOutBufferSize >= 16)
				{
					size_t n = g_session.dequeueNotification(reinterpret_cast<uint8_t*>(lpOutBuffer));
					if (lpBytesReturned) *lpBytesReturned = static_cast<DWORD>(n);
					return n == 16 ? TRUE : FALSE;
				}
				return FALSE;
			}
			return FALSE;
		}

		if (g_realDeviceIoControl)
			return g_realDeviceIoControl(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize,
			                             lpOutBuffer, nOutBufferSize, lpBytesReturned, lpOverlapped);
		return FALSE;
	}

	BOOL WINAPI Hook_ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
	                         LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
	{
		if (hFile == kVirtualG2Handle)
		{
			size_t bytesRead = 0;
			bool ok = g_session.readFile(reinterpret_cast<uint8_t*>(lpBuffer), nNumberOfBytesToRead, bytesRead);
			if (lpNumberOfBytesRead)
				*lpNumberOfBytesRead = static_cast<DWORD>(bytesRead);
			return ok ? TRUE : FALSE;
		}

		if (g_realReadFile)
			return g_realReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
		return FALSE;
	}

	BOOL WINAPI Hook_WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
	                          LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
	{
		if (hFile == kVirtualG2Handle)
		{
			size_t bytesWritten = 0;
			bool ok = g_session.writeFile(reinterpret_cast<const uint8_t*>(lpBuffer), nNumberOfBytesToWrite, bytesWritten);
			if (lpNumberOfBytesWritten)
				*lpNumberOfBytesWritten = static_cast<DWORD>(bytesWritten);
			return ok ? TRUE : FALSE;
		}

		if (g_realWriteFile)
			return g_realWriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
		return FALSE;
	}

	BOOL WINAPI Hook_CloseHandle(HANDLE hObject)
	{
		if (hObject == kVirtualG2Handle)
		{
			g_session.disconnect();
			return TRUE;
		}

		if (g_realCloseHandle)
			return g_realCloseHandle(hObject);
		return CloseHandle(hObject);
	}

	void installIatHooks()
	{
		HMODULE hExe = GetModuleHandleA(NULL);
		if (!hExe) return;

		BYTE* base = reinterpret_cast<BYTE*>(hExe);
		PIMAGE_DOS_HEADER dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
		PIMAGE_NT_HEADERS nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) return;

		IMAGE_DATA_DIRECTORY importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (importDir.VirtualAddress == 0) return;

		PIMAGE_IMPORT_DESCRIPTOR desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + importDir.VirtualAddress);
		for (; desc->Name != 0; ++desc)
		{
			const char* name = reinterpret_cast<const char*>(base + desc->Name);
			if (_stricmp(name, "KERNEL32.dll") == 0 || _stricmp(name, "kernel32.dll") == 0)
			{
				PIMAGE_THUNK_DATA thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + desc->FirstThunk);
				PIMAGE_THUNK_DATA origThunk = desc->OriginalFirstThunk
					? reinterpret_cast<PIMAGE_THUNK_DATA>(base + desc->OriginalFirstThunk)
					: thunk;

				for (; origThunk->u1.AddressOfData != 0; ++thunk, ++origThunk)
				{
					if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
					PIMAGE_IMPORT_BY_NAME ibn = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + origThunk->u1.AddressOfData);
					const char* fn = reinterpret_cast<const char*>(ibn->Name);

					auto hook = [&](const char* target, void* hookFn, void** oldFn) {
						if (std::strcmp(fn, target) == 0)
						{
							DWORD oldProt = 0;
							if (VirtualProtect(&thunk->u1.Function, sizeof(DWORD), PAGE_READWRITE, &oldProt))
							{
								if (oldFn && !*oldFn)
									*oldFn = reinterpret_cast<void*>(thunk->u1.Function);
								thunk->u1.Function = static_cast<DWORD>(reinterpret_cast<uintptr_t>(hookFn));
								VirtualProtect(&thunk->u1.Function, sizeof(DWORD), oldProt, &oldProt);
							}
						}
					};

					hook("CreateFileA", (void*)Hook_CreateFileA, (void**)&g_realCreateFileA);
					hook("DeviceIoControl", (void*)Hook_DeviceIoControl, (void**)&g_realDeviceIoControl);
					hook("ReadFile", (void*)Hook_ReadFile, (void**)&g_realReadFile);
					hook("WriteFile", (void*)Hook_WriteFile, (void**)&g_realWriteFile);
					hook("CloseHandle", (void*)Hook_CloseHandle, (void**)&g_realCloseHandle);
				}
			}
		}
	}
} // namespace

extern "C" {
__declspec(dllexport) HDEVINFO WINAPI SetupDiGetClassDevsA(const GUID* ClassGuid, PCSTR Enumerator, HWND hwndParent, DWORD Flags)
{
	if (isClaviaGuid(ClassGuid))
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

__declspec(dllexport) BOOL WINAPI SetupDiEnumDeviceInterfaces(HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData,
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
		if (InterfaceClassGuid)
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

__declspec(dllexport) BOOL WINAPI SetupDiGetDeviceInterfaceDetailA(HDEVINFO DeviceInfoSet, PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
																  PSP_DEVICE_INTERFACE_DETAIL_DATA_A DeviceInterfaceDetailData,
																  DWORD DeviceInterfaceDetailDataSize, PDWORD RequiredSize,
																  PSP_DEVINFO_DATA DeviceInfoData)
{
	if (reinterpret_cast<uintptr_t>(DeviceInfoSet) == kVirtualHdevInfo)
	{
		const size_t pathLen = std::strlen(kVirtualDevicePath) + 1;
		const DWORD needed = static_cast<DWORD>(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) + pathLen);

		if (RequiredSize)
			*RequiredSize = needed;

		if (DeviceInterfaceDetailData == nullptr || DeviceInterfaceDetailDataSize < needed)
		{
			::SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}

		std::memcpy(DeviceInterfaceDetailData->DevicePath, kVirtualDevicePath, pathLen);
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

__declspec(dllexport) BOOL WINAPI SetupDiDestroyDeviceInfoList(HDEVINFO DeviceInfoSet)
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

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hinstDLL);
		installIatHooks();
	}
	else if (fdwReason == DLL_PROCESS_DETACH)
	{
		g_pumpRunning.store(false);
		if (g_pumpThread)
		{
			WaitForSingleObject(g_pumpThread, 1000);
			CloseHandle(g_pumpThread);
			g_pumpThread = NULL;
		}
		g_session.disconnect();
	}
	return TRUE;
}
}

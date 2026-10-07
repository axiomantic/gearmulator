#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <setupapi.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "instanceRegistry.h"
#include "rg2usb.h"

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	constexpr uintptr_t kVirtualHdevInfo = 0x47324445; // 'G2DE'
	const HANDLE kVirtualG2Handle = (HANDLE)(uintptr_t)0x47325553; // 'G2US'
	constexpr uintptr_t kVirtualHandleMask = 0xFFFF0000;
	constexpr uintptr_t kVirtualHandleBase = 0x47320000;

	// {CB3ED981-6125-4047-BC2A-292E370CC89A}
	constexpr uint8_t kClaviaGuidBytes[16] = {0x81, 0xD9, 0x3E, 0xCB, 0x25, 0x61, 0x47, 0x40,
											  0xBC, 0x2A, 0x29, 0x2E, 0x37, 0x0C, 0xC8, 0x9A};

	void proxyLog(const char* fmt, ...)
	{
		FILE* f = fopen("C:\\setupapi_debug.log", "a");
		if (f)
		{
			va_list args;
			va_start(args, fmt);
			vfprintf(f, fmt, args);
			va_end(args);
			fclose(f);
		}
	}

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
		if (!guid)
			return false;
		return std::memcmp(guid, kClaviaGuidBytes, 16) == 0;
	}

	bool isClaviaPath(LPCSTR path)
	{
		if (!path)
			return false;
		std::string s(path);
		for (auto& c : s)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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

	struct SessionSlot
	{
		std::unique_ptr<rg2::wine::G2UsbDeviceSession> session;
		std::string host{"127.0.0.1"};
		uint16_t port{7777};
		std::atomic<bool> pumpRunning{false};
		HANDLE pumpThread{NULL};
		bool inUse{false};
	};

	std::vector<std::unique_ptr<SessionSlot>> g_slots;
	std::mutex g_slotsMutex;


	rg2::InstanceRegistry g_registry;
	std::mutex g_registryMutex;

	void initRegistryPaths()
	{
		static bool s_inited = false;
		if (!s_inited)
		{
			s_inited = true;
			char modPath[MAX_PATH];
			if (::GetModuleFileNameA(NULL, modPath, MAX_PATH) > 0)
			{
				char* slash = std::strrchr(modPath, '\\');
				if (slash)
				{
					*(slash + 1) = '\0';
					g_registry.setIniPath(std::string(modPath) + "setupapi.ini");
				}
			}
			g_registry.addDiscoveryPath("setupapi.ini");
			g_registry.addDiscoveryPath("Z:\\tmp\\redgecko2_instances.json");
			g_registry.addDiscoveryPath("\\\\??\\unix\\tmp\\redgecko2_instances.json");
			g_registry.addDiscoveryPath("Z:\\private\\tmp\\redgecko2_instances.json");
			g_registry.addDiscoveryPath("Y:\\.config\\redgecko2\\instances.json");
			g_registry.addDiscoveryPath("C:\\users\\public\\redgecko2_instances.json");
		}
	}

	DWORD WINAPI pumpThreadFunc(LPVOID param)
	{
		const size_t slotIdx = reinterpret_cast<size_t>(param);
		while (true)
		{
			rg2::wine::G2UsbDeviceSession* sess = nullptr;
			{
				std::lock_guard<std::mutex> lock(g_slotsMutex);
				if (slotIdx >= g_slots.size() || !g_slots[slotIdx]->pumpRunning.load())
					break;
				sess = g_slots[slotIdx]->session.get();
			}

			if (sess && sess->isConnected())
			{
				sess->pumpSocket(20);
			}
			else
			{
				Sleep(25);
			}
		}
		return 0;
	}

	rg2::wine::G2UsbDeviceSession* getSessionFromHandle(const HANDLE hDevice)
	{
		const uintptr_t val = reinterpret_cast<uintptr_t>(hDevice);
		if (hDevice == kVirtualG2Handle)
		{
			std::lock_guard<std::mutex> lock(g_slotsMutex);
			return (!g_slots.empty() && g_slots[0]->inUse) ? g_slots[0]->session.get() : nullptr;
		}

		if ((val & kVirtualHandleMask) == kVirtualHandleBase)
		{
			const size_t slotIdx = val & 0xFFFF;
			std::lock_guard<std::mutex> lock(g_slotsMutex);
			if (slotIdx < g_slots.size() && g_slots[slotIdx]->inUse)
				return g_slots[slotIdx]->session.get();
		}
		return nullptr;
	}

	HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
								   LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
								   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
	{
		proxyLog("Hook_CreateFileA: %s\n", lpFileName ? lpFileName : "<null>");
		if (isClaviaPath(lpFileName))
		{
			std::string host = "127.0.0.1";
			uint16_t port = 7777;
			rg2::parseDevicePath(lpFileName, host, port);

			std::lock_guard<std::mutex> lock(g_slotsMutex);
			size_t slotIdx = g_slots.size();
			for (size_t i = 0; i < g_slots.size(); ++i)
			{
				if (!g_slots[i]->inUse)
				{
					slotIdx = i;
					break;
				}
			}

			if (slotIdx == g_slots.size())
			{
				g_slots.push_back(std::make_unique<SessionSlot>());
			}

			auto& slot = *g_slots[slotIdx];
			if (!slot.session)
				slot.session = std::make_unique<rg2::wine::G2UsbDeviceSession>();

			slot.host = host;
			slot.port = port;
			slot.inUse = true;

			if (!slot.session->isConnected())
			{
				const bool ok = slot.session->connectToHub(host.c_str(), port);
				proxyLog("connectToHub (%s:%u): %s\n", host.c_str(), port, ok ? "OK" : "FAILED");
				if (!ok)
				{
					slot.inUse = false;
					SetLastError(ERROR_DEV_NOT_EXIST);
					return INVALID_HANDLE_VALUE;
				}
				if (!slot.pumpRunning.load())
				{
					slot.pumpRunning.store(true);
					slot.pumpThread = CreateThread(NULL, 0, pumpThreadFunc, reinterpret_cast<LPVOID>(slotIdx), 0, NULL);
				}
			}

			const HANDLE vHandle = (HANDLE)(uintptr_t)(kVirtualHandleBase | (slotIdx & 0xFFFF));
			return vHandle;
		}


		if (g_realCreateFileA)
			return g_realCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
									 dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
		return CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition,
						   dwFlagsAndAttributes, hTemplateFile);
	}

	BOOL WINAPI Hook_DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer, DWORD nInBufferSize,
									 LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned,
									 LPOVERLAPPED lpOverlapped)
	{
		auto* session = getSessionFromHandle(hDevice);
		if (session)
		{
			if (dwIoControlCode != 0x22200C)
				proxyLog("DeviceIoControl: 0x%08lx inSz=%lu outSz=%lu\n", dwIoControlCode, nInBufferSize,
						 nOutBufferSize);

			if (dwIoControlCode == 0x222000) // Register event
			{
				if (lpInBuffer && nInBufferSize >= sizeof(HANDLE))
				{
					HANDLE hEvent = *reinterpret_cast<HANDLE*>(lpInBuffer);
					session->registerEvent(reinterpret_cast<uintptr_t>(hEvent));
					if (lpBytesReturned)
						*lpBytesReturned = 0;
					return TRUE;
				}
				return FALSE;
			}
			if (dwIoControlCode == 0x222004) // Unregister event
			{
				session->unregisterEvent();
				if (lpBytesReturned)
					*lpBytesReturned = 0;
				return TRUE;
			}
			if (dwIoControlCode == 0x222008) // Query event
			{
				bool reg = false;
				session->queryEvent(reg);
				if (lpOutBuffer && nOutBufferSize >= sizeof(DWORD))
					*reinterpret_cast<DWORD*>(lpOutBuffer) = reg ? 1 : 0;
				if (lpBytesReturned)
					*lpBytesReturned = sizeof(DWORD);
				return TRUE;
			}
			if (dwIoControlCode == 0x22200C) // Get notification (16-byte record)
			{
				if (lpOutBuffer && nOutBufferSize >= 16)
				{
					size_t n = session->dequeueNotification(reinterpret_cast<uint8_t*>(lpOutBuffer));
					if (lpBytesReturned)
						*lpBytesReturned = static_cast<DWORD>(n);
					if (n > 0)
					{
						char hexBuf[128] = {0};
						for (size_t i = 0; i < 16; ++i)
							std::sprintf(hexBuf + i * 3, "%02x ", (reinterpret_cast<const uint8_t*>(lpOutBuffer))[i]);
						proxyLog("DeviceIoControl 0x22200C: popped notification [%s]\n", hexBuf);
					}
					return TRUE;
				}
				return FALSE;
			}
			return FALSE;
		}

		if (g_realDeviceIoControl)
			return g_realDeviceIoControl(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer,
										 nOutBufferSize, lpBytesReturned, lpOverlapped);
		return FALSE;
	}

	BOOL WINAPI Hook_ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead,
							  LPOVERLAPPED lpOverlapped)
	{
		auto* session = getSessionFromHandle(hFile);
		if (session)
		{
			size_t bytesRead = 0;
			bool ok = session->readFile(reinterpret_cast<uint8_t*>(lpBuffer), nNumberOfBytesToRead, bytesRead);
			if (lpNumberOfBytesRead)
				*lpNumberOfBytesRead = static_cast<DWORD>(bytesRead);
			if (lpOverlapped)
			{
				lpOverlapped->Internal = 0;
				lpOverlapped->InternalHigh = static_cast<DWORD>(bytesRead);
				if (lpOverlapped->hEvent)
					SetEvent(lpOverlapped->hEvent);
			}
			return ok ? TRUE : FALSE;
		}

		if (g_realReadFile)
			return g_realReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
		return FALSE;
	}

	BOOL WINAPI Hook_WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
							   LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
	{
		auto* session = getSessionFromHandle(hFile);
		if (session)
		{
			size_t bytesWritten = 0;
			bool ok =
				session->writeFile(reinterpret_cast<const uint8_t*>(lpBuffer), nNumberOfBytesToWrite, bytesWritten);
			if (lpNumberOfBytesWritten)
				*lpNumberOfBytesWritten = static_cast<DWORD>(bytesWritten);
			if (lpOverlapped)
			{
				lpOverlapped->Internal = 0;
				lpOverlapped->InternalHigh = static_cast<DWORD>(bytesWritten);
				if (lpOverlapped->hEvent)
					SetEvent(lpOverlapped->hEvent);
			}
			return ok ? TRUE : FALSE;
		}

		if (g_realWriteFile)
			return g_realWriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
		return FALSE;
	}

	BOOL WINAPI Hook_CloseHandle(HANDLE hObject)
	{
		const uintptr_t val = reinterpret_cast<uintptr_t>(hObject);
		if ((val & kVirtualHandleMask) == kVirtualHandleBase || hObject == kVirtualG2Handle)
		{
			const size_t slotIdx = (hObject == kVirtualG2Handle) ? 0 : (val & 0xFFFF);
			HANDLE threadToJoin = NULL;
			{
				std::lock_guard<std::mutex> lock(g_slotsMutex);
				if (slotIdx < g_slots.size() && g_slots[slotIdx]->inUse)
				{
					g_slots[slotIdx]->pumpRunning.store(false);
					threadToJoin = g_slots[slotIdx]->pumpThread;
					g_slots[slotIdx]->pumpThread = NULL;
					if (g_slots[slotIdx]->session)
						g_slots[slotIdx]->session->disconnect();
					g_slots[slotIdx]->inUse = false;
				}
			}

			if (threadToJoin)
			{
				WaitForSingleObject(threadToJoin, 1000);
				CloseHandle(threadToJoin);
			}
			return TRUE;
		}

		if (g_realCloseHandle)
			return g_realCloseHandle(hObject);
		return CloseHandle(hObject);
	}

	void installIatHooks()
	{
		HMODULE hExe = GetModuleHandleA(NULL);
		if (!hExe)
			return;

		BYTE* base = reinterpret_cast<BYTE*>(hExe);
		PIMAGE_DOS_HEADER dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return;
		PIMAGE_NT_HEADERS nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return;

		IMAGE_DATA_DIRECTORY importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (importDir.VirtualAddress == 0)
			return;

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
					if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
						continue;
					PIMAGE_IMPORT_BY_NAME ibn =
						reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + origThunk->u1.AddressOfData);
					const char* fn = reinterpret_cast<const char*>(ibn->Name);

					auto hook = [&](const char* target, void* hookFn, void** oldFn)
					{
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
__declspec(dllexport) HDEVINFO WINAPI SetupDiGetClassDevsA(const GUID* ClassGuid, PCSTR Enumerator, HWND hwndParent,
														   DWORD Flags)
{
	proxyLog("SetupDiGetClassDevsA: isClavia=%d enum=%s\n", isClaviaGuid(ClassGuid),
			 Enumerator ? Enumerator : "<null>");
	if (isClaviaGuid(ClassGuid))
	{
		initRegistryPaths();
		std::lock_guard<std::mutex> lock(g_registryMutex);
		g_registry.refresh(false);
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
	proxyLog("SetupDiEnumDeviceInterfaces: isVirtual=%d member=%lu\n",
			 reinterpret_cast<uintptr_t>(DeviceInfoSet) == kVirtualHdevInfo, MemberIndex);
	if (reinterpret_cast<uintptr_t>(DeviceInfoSet) == kVirtualHdevInfo)
	{
		if (DeviceInterfaceData == nullptr)
		{
			::SetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}

		initRegistryPaths();
		std::lock_guard<std::mutex> lock(g_registryMutex);
		g_registry.refresh(false);

		if (MemberIndex >= g_registry.count())
		{
			::SetLastError(ERROR_NO_MORE_ITEMS);
			return FALSE;
		}

		DeviceInterfaceData->cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
		if (InterfaceClassGuid)
			std::memcpy(&DeviceInterfaceData->InterfaceClassGuid, InterfaceClassGuid, sizeof(GUID));
		DeviceInterfaceData->Flags = SPINT_ACTIVE;
		DeviceInterfaceData->Reserved = static_cast<ULONG_PTR>(MemberIndex);
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

__declspec(dllexport) BOOL WINAPI SetupDiGetDeviceInterfaceDetailA(
	HDEVINFO DeviceInfoSet, PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
	PSP_DEVICE_INTERFACE_DETAIL_DATA_A DeviceInterfaceDetailData, DWORD DeviceInterfaceDetailDataSize,
	PDWORD RequiredSize, PSP_DEVINFO_DATA DeviceInfoData)
{
	proxyLog("SetupDiGetDeviceInterfaceDetailA: isVirtual=%d sz=%lu\n",
			 reinterpret_cast<uintptr_t>(DeviceInfoSet) == kVirtualHdevInfo, DeviceInterfaceDetailDataSize);
	if (reinterpret_cast<uintptr_t>(DeviceInfoSet) == kVirtualHdevInfo)
	{
		initRegistryPaths();
		std::string devPath;
		{
			std::lock_guard<std::mutex> lock(g_registryMutex);
			const size_t idx = DeviceInterfaceData ? static_cast<size_t>(DeviceInterfaceData->Reserved) : 0;
			devPath = g_registry.getDevicePath(idx);
		}

		const size_t pathLen = devPath.size() + 1;
		const DWORD needed = static_cast<DWORD>(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) + pathLen);

		if (RequiredSize)
			*RequiredSize = needed;

		if (DeviceInterfaceDetailData == nullptr || DeviceInterfaceDetailDataSize < needed)
		{
			::SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}

		std::memcpy(DeviceInterfaceDetailData->DevicePath, devPath.c_str(), pathLen);
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
		std::vector<HANDLE> threadsToJoin;
		{
			std::lock_guard<std::mutex> lock(g_slotsMutex);
			for (auto& slot : g_slots)
			{
				slot->pumpRunning.store(false);
				if (slot->pumpThread)
				{
					threadsToJoin.push_back(slot->pumpThread);
					slot->pumpThread = NULL;
				}
				if (slot->session)
					slot->session->disconnect();
				slot->inUse = false;
			}
		}
		for (HANDLE h : threadsToJoin)
		{
			WaitForSingleObject(h, 1000);
			CloseHandle(h);
		}
	}
	return TRUE;
}
}

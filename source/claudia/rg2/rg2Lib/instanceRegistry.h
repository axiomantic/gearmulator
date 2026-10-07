#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rg2
{
	struct InstanceEntry
	{
		std::string host{"127.0.0.1"};
		uint16_t port{7777};
		std::string name;
		uint32_t pid{0};
	};

	/* Formats a USB virtual device path containing host and port:
	 * \\?\usb#vid_0ffc&pid_0002#rg2_<host>_<port>#{cb3ed981-6125-4047-bc2a-292e370cc89a} */
	std::string formatDevicePath(const std::string& host, uint16_t port);

	/* Parses host and port out of a virtual device path. Supports #rg2_<host>_<port>#
	 * as well as legacy #clavia_g2_virtual# falling back to 127.0.0.1:7777. */
	bool parseDevicePath(const char* path, std::string& outHost, uint16_t& outPort);

	/* Parses setupapi.ini format supporting:
	 * 1. [InstanceX] or [G2_X] sections with Host, Port, Name keys
	 * 2. [Instances] section with lines or keys of Host:Port
	 * 3. [Default] section with Host and Port */
	bool parseSetupApiIni(const std::string& content, std::vector<InstanceEntry>& outEntries);

	/* Parses JSON format:
	 * { "instances": [ { "pid": 1234, "port": 7777, "host": "127.0.0.1", "name": "..." }, ... ] } */
	bool parseInstancesJson(const std::string& content, std::vector<InstanceEntry>& outEntries);

	/* Serializes an instance list to formatted JSON for atomic persistence. */
	std::string serializeInstancesJson(const std::vector<InstanceEntry>& entries);

	/* Atomic registration on host filesystem:
	 * Reads existing registry file, prunes inactive PIDs on POSIX, adds/updates entry,
	 * and atomically replaces the file. */
	bool registerInstance(const std::string& filePath, uint32_t pid, uint16_t port, const std::string& name = "");

	/* Atomic unregistration on host filesystem:
	 * Reads existing registry file, removes entry matching pid and port, and updates file. */
	bool unregisterInstance(const std::string& filePath, uint32_t pid, uint16_t port);

	/* Default shared registry file paths. */
	const char* getDefaultRegistryFilePath();

	/* High-level registry manager querying setupapi.ini and discovery JSON paths. */
	class InstanceRegistry
	{
	public:
		InstanceRegistry();

		void setIniPath(const std::string& path);
		void addDiscoveryPath(const std::string& path);

		/* Refreshes instance list if any watched file modification timestamps changed.
		 * Falls back to single default instance on 127.0.0.1:7777 when no configs exist. */
		bool refresh(bool force = false);

		size_t count() const;
		InstanceEntry get(size_t index) const;
		std::string getDevicePath(size_t index) const;

	private:
		std::string m_iniPath;
		std::vector<std::string> m_discoveryPaths;
		std::vector<InstanceEntry> m_instances;
		uint64_t m_lastIniModTime{0};
		uint64_t m_lastJsonModTime{0};
	};
} // namespace rg2

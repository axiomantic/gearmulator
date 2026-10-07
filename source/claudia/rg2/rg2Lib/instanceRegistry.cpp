#include "instanceRegistry.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace rg2
{
	namespace
	{
		std::string trim(const std::string& str)
		{
			const auto start = str.find_first_not_of(" \t\r\n");
			if (start == std::string::npos)
				return "";
			const auto end = str.find_last_not_of(" \t\r\n");
			return str.substr(start, end - start + 1);
		}

		std::string toLower(const std::string& str)
		{
			std::string out = str;
			for (char& c : out)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return out;
		}

		uint64_t getFileModTime(const std::string& path)
		{
			if (path.empty())
				return 0;
			struct stat st{};
			if (stat(path.c_str(), &st) == 0)
			{
				return static_cast<uint64_t>(st.st_mtime);
			}
			return 0;
		}

		bool isProcessAlive(const uint32_t pid)
		{
			if (pid == 0)
				return true;
#if !defined(_WIN32)
			if (kill(static_cast<pid_t>(pid), 0) == 0)
				return true;
			return errno != ESRCH;
#else
			HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
			if (h != NULL)
			{
				DWORD exitCode = 0;
				const BOOL ok = GetExitCodeProcess(h, &exitCode);
				CloseHandle(h);
				return ok && (exitCode == STILL_ACTIVE);
			}
			return false;
#endif
		}
	} // namespace

	std::string formatDevicePath(const std::string& host, const uint16_t port)
	{
		return std::string("\\\\?\\usb#vid_0ffc&pid_0002#rg2_") + host + "_" + std::to_string(port) +
			"#{cb3ed981-6125-4047-bc2a-292e370cc89a}";
	}

	bool parseDevicePath(const char* const path, std::string& outHost, uint16_t& outPort)
	{
		if (path == nullptr)
			return false;

		const std::string s(path);
		const std::string lower = toLower(s);

		if (lower.find("vid_0ffc&pid_0002") == std::string::npos)
			return false;

		const auto p1 = lower.find("#rg2_");
		if (p1 != std::string::npos)
		{
			const size_t start = p1 + 5;
			const auto p2 = lower.find('#', start);
			if (p2 != std::string::npos)
			{
				const std::string sub = s.substr(start, p2 - start);
				const auto us = sub.rfind('_');
				if (us != std::string::npos && us + 1 < sub.size())
				{
					outHost = sub.substr(0, us);
					outPort = static_cast<uint16_t>(std::strtoul(sub.c_str() + us + 1, nullptr, 10));
					if (outPort > 0)
						return true;
				}
			}
		}

		if (lower.find("cb3ed981-6125-4047-bc2a-292e370cc89a") != std::string::npos)
		{
			outHost = "127.0.0.1";
			outPort = 7777;
			return true;
		}

		return false;
	}

	bool parseSetupApiIni(const std::string& content, std::vector<InstanceEntry>& outEntries)
	{
		outEntries.clear();
		std::istringstream stream(content);
		std::string line;

		std::string currentSection;
		InstanceEntry currentEntry;
		bool inSection = false;

		auto flushEntry = [&]()
		{
			if (inSection && currentEntry.port > 0)
			{
				if (currentEntry.host.empty())
					currentEntry.host = "127.0.0.1";
				outEntries.push_back(currentEntry);
			}
			currentEntry = InstanceEntry{};
		};

		while (std::getline(stream, line))
		{
			const auto commentPos = line.find_first_of(";#");
			if (commentPos != std::string::npos)
				line = line.substr(0, commentPos);
			line = trim(line);
			if (line.empty())
				continue;

			if (line.front() == '[' && line.back() == ']')
			{
				flushEntry();
				currentSection = toLower(line.substr(1, line.size() - 2));
				inSection = (currentSection != "instances");
				continue;
			}

			if (currentSection == "instances")
			{
				std::string target = line;
				const auto eq = line.find('=');
				if (eq != std::string::npos)
					target = trim(line.substr(eq + 1));

				const auto colon = target.find(':');
				if (colon != std::string::npos)
				{
					InstanceEntry entry;
					entry.host = trim(target.substr(0, colon));
					entry.port = static_cast<uint16_t>(std::strtoul(target.c_str() + colon + 1, nullptr, 10));
					if (entry.port > 0)
					{
						if (entry.host.empty())
							entry.host = "127.0.0.1";
						outEntries.push_back(entry);
					}
				}
				continue;
			}

			const auto eq = line.find('=');
			if (eq != std::string::npos)
			{
				const std::string key = toLower(trim(line.substr(0, eq)));
				const std::string val = trim(line.substr(eq + 1));

				if (key == "host")
					currentEntry.host = val;
				else if (key == "port")
					currentEntry.port = static_cast<uint16_t>(std::strtoul(val.c_str(), nullptr, 10));
				else if (key == "name")
					currentEntry.name = val;
			}
		}

		flushEntry();
		return !outEntries.empty();
	}

	bool parseInstancesJson(const std::string& content, std::vector<InstanceEntry>& outEntries)
	{
		outEntries.clear();
		size_t pos = 0;

		while ((pos = content.find('{', pos)) != std::string::npos)
		{
			const size_t end = content.find('}', pos);
			if (end == std::string::npos)
				break;

			const std::string obj = content.substr(pos, end - pos + 1);
			pos = end + 1;

			if (obj.find("\"port\"") == std::string::npos && obj.find("port") == std::string::npos)
				continue;

			InstanceEntry entry;
			auto extractNum = [&](const char* const key) -> uint32_t
			{
				const auto kpos = obj.find(key);
				if (kpos == std::string::npos)
					return 0;
				const auto colon = obj.find(':', kpos);
				if (colon == std::string::npos)
					return 0;
				const char* start = obj.c_str() + colon + 1;
				while (*start == ' ' || *start == '\t')
					++start;
				return static_cast<uint32_t>(std::strtoul(start, nullptr, 10));
			};

			auto extractStr = [&](const char* const key) -> std::string
			{
				const auto kpos = obj.find(key);
				if (kpos == std::string::npos)
					return "";
				const auto colon = obj.find(':', kpos);
				if (colon == std::string::npos)
					return "";
				const auto q1 = obj.find('"', colon);
				if (q1 == std::string::npos)
					return "";
				const auto q2 = obj.find('"', q1 + 1);
				if (q2 == std::string::npos)
					return "";
				return obj.substr(q1 + 1, q2 - q1 - 1);
			};

			entry.port = static_cast<uint16_t>(extractNum("port"));
			entry.pid = extractNum("pid");
			entry.host = extractStr("host");
			if (entry.host.empty())
				entry.host = "127.0.0.1";
			entry.name = extractStr("name");

			if (entry.port > 0)
				outEntries.push_back(entry);
		}

		return !outEntries.empty();
	}

	std::string serializeInstancesJson(const std::vector<InstanceEntry>& entries)
	{
		std::ostringstream ss;
		ss << "{\n\t\"instances\": [\n";
		for (size_t i = 0; i < entries.size(); ++i)
		{
			const auto& e = entries[i];
			ss << "\t\t{\n";
			ss << "\t\t\t\"pid\": " << e.pid << ",\n";
			ss << "\t\t\t\"port\": " << e.port << ",\n";
			ss << "\t\t\t\"host\": \"" << (e.host.empty() ? "127.0.0.1" : e.host) << "\",\n";
			ss << "\t\t\t\"name\": \"" << e.name << "\"\n";
			ss << "\t\t}" << (i + 1 < entries.size() ? ",\n" : "\n");
		}
		ss << "\t]\n}\n";
		return ss.str();
	}

	bool registerInstance(const std::string& filePath, const uint32_t pid, const uint16_t port, const std::string& name)
	{
		if (filePath.empty() || port == 0)
			return false;

		std::vector<InstanceEntry> entries;
		{
			std::ifstream ifs(filePath);
			if (ifs.is_open())
			{
				std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
				parseInstancesJson(content, entries);
			}
		}

		// Prune inactive PIDs
		entries.erase(std::remove_if(entries.begin(), entries.end(),
									 [](const InstanceEntry& e) { return e.pid > 0 && !isProcessAlive(e.pid); }),
					  entries.end());

		// Update or append
		bool found = false;
		for (auto& e : entries)
		{
			if (e.port == port)
			{
				e.pid = pid;
				if (!name.empty())
					e.name = name;
				found = true;
				break;
			}
		}

		if (!found)
		{
			InstanceEntry newEntry;
			newEntry.pid = pid;
			newEntry.port = port;
			newEntry.host = "127.0.0.1";
			newEntry.name = name.empty() ? ("RedGecko2 Port " + std::to_string(port)) : name;
			entries.push_back(newEntry);
		}

		const std::string serialized = serializeInstancesJson(entries);
		const std::string tmpPath = filePath + ".tmp." + std::to_string(pid) + "." + std::to_string(port);

		{
			std::ofstream ofs(tmpPath, std::ios::out | std::ios::trunc);
			if (!ofs.is_open())
				return false;
			ofs << serialized;
		}

		std::remove(filePath.c_str());
		return std::rename(tmpPath.c_str(), filePath.c_str()) == 0;
	}

	bool unregisterInstance(const std::string& filePath, const uint32_t pid, const uint16_t port)
	{
		if (filePath.empty() || port == 0)
			return false;

		std::vector<InstanceEntry> entries;
		{
			std::ifstream ifs(filePath);
			if (!ifs.is_open())
				return true;
			std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
			parseInstancesJson(content, entries);
		}

		entries.erase(std::remove_if(entries.begin(), entries.end(),
									 [pid, port](const InstanceEntry& e)
									 {
										 if (e.port != port)
											 return false;
										 return (pid == 0 || e.pid == 0 || e.pid == pid);
									 }),
					  entries.end());

		if (entries.empty())
		{
			std::remove(filePath.c_str());
			return true;
		}

		const std::string serialized = serializeInstancesJson(entries);
		const std::string tmpPath = filePath + ".tmp." + std::to_string(pid) + "." + std::to_string(port);

		{
			std::ofstream ofs(tmpPath, std::ios::out | std::ios::trunc);
			if (!ofs.is_open())
				return false;
			ofs << serialized;
		}

		std::remove(filePath.c_str());
		return std::rename(tmpPath.c_str(), filePath.c_str()) == 0;
	}

	const char* getDefaultRegistryFilePath()
	{
#if defined(_WIN32)
		struct stat st{};
		if (stat("Z:\\tmp\\redgecko2_instances.json", &st) == 0)
			return "Z:\\tmp\\redgecko2_instances.json";
		return "\\\\??\\unix\\tmp\\redgecko2_instances.json";
#else
		return "/tmp/redgecko2_instances.json";
#endif
	}

	InstanceRegistry::InstanceRegistry()
	{
		m_instances.push_back(InstanceEntry{"127.0.0.1", 7777, "Red Gecko 2 Default", 0});
	}

	void InstanceRegistry::setIniPath(const std::string& path) { m_iniPath = path; }

	void InstanceRegistry::addDiscoveryPath(const std::string& path)
	{
		if (!path.empty())
			m_discoveryPaths.push_back(path);
	}

	bool InstanceRegistry::refresh(const bool force)
	{
		const uint64_t iniMod = getFileModTime(m_iniPath);
		uint64_t jsonMod = 0;
		std::string activeJsonPath;

		for (const auto& p : m_discoveryPaths)
		{
			const uint64_t m = getFileModTime(p);
			if (m > 0)
			{
				jsonMod = m;
				activeJsonPath = p;
				break;
			}
		}

		if (!force && iniMod == m_lastIniModTime && jsonMod == m_lastJsonModTime && !m_instances.empty())
			return true;

		m_lastIniModTime = iniMod;
		m_lastJsonModTime = jsonMod;

		std::vector<InstanceEntry> parsed;

		if (!m_iniPath.empty())
		{
			std::ifstream ifs(m_iniPath);
			if (ifs.is_open())
			{
				std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
				parseSetupApiIni(content, parsed);
			}
		}

		if (parsed.empty() && !activeJsonPath.empty())
		{
			std::ifstream ifs(activeJsonPath);
			if (ifs.is_open())
			{
				std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
				parseInstancesJson(content, parsed);
			}
		}

		if (parsed.empty())
		{
			m_instances.clear();
			m_instances.push_back(InstanceEntry{"127.0.0.1", 7777, "Red Gecko 2 Default", 0});
		}
		else
		{
			m_instances = std::move(parsed);
		}

		return true;
	}

	size_t InstanceRegistry::count() const { return m_instances.size(); }

	InstanceEntry InstanceRegistry::get(const size_t index) const
	{
		if (index < m_instances.size())
			return m_instances[index];
		return InstanceEntry{"127.0.0.1", 7777, "Red Gecko 2 Default", 0};
	}

	std::string InstanceRegistry::getDevicePath(const size_t index) const
	{
		const auto entry = get(index);
		return formatDevicePath(entry.host, entry.port);
	}
} // namespace rg2

/* t0_instance_registry.cpp -- Multi-instance discovery and SetupAPI registry.
 * Tier T0: no artifact, no firmware, no external file.
 *
 * The property this file exists to hold: instances register active port and
 * PID metadata into a shared discovery file; setupapi.dll discovers all running
 * instances or reads explicit setupapi.ini configurations, mapping each to a
 * distinct virtual USB device path.
 *
 * Nothing in this file is an assert() and nothing catches an exception.
 * Every run-time verdict reports through failures; every compile-time
 * verdict is a static_assert.
 */

#include "../instanceRegistry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
	int failures = 0;

	void check(const bool condition, const char* const what)
	{
		if (!condition)
		{
			printf("FAIL %s\n", what);
			++failures;
		}
	}

	void checkEqual(const uint64_t observed, const uint64_t expected, const char* const what)
	{
		if (observed != expected)
		{
			printf("FAIL %s: observed %llu, expected %llu\n", what, static_cast<unsigned long long>(observed),
				   static_cast<unsigned long long>(expected));
			++failures;
		}
	}

	void checkStrEqual(const std::string& observed, const std::string& expected, const char* const what)
	{
		if (observed != expected)
		{
			printf("FAIL %s: observed '%s', expected '%s'\n", what, observed.c_str(), expected.c_str());
			++failures;
		}
	}
} // namespace

int main()
{
	/* ------------------------------------------------------------- case 1.
	 * Device path formatting and bidirectional parsing. */
	const std::string path1 = rg2::formatDevicePath("127.0.0.1", 7777);
	check(path1.find("#rg2_127.0.0.1_7777#") != std::string::npos, "Device path contains host and port identifier");
	check(path1.find("vid_0ffc&pid_0002") != std::string::npos, "Device path contains Clavia VID/PID");
	check(path1.find("cb3ed981-6125-4047-bc2a-292e370cc89a") != std::string::npos, "Device path contains Clavia GUID");

	std::string parsedHost;
	uint16_t parsedPort = 0;
	check(rg2::parseDevicePath(path1.c_str(), parsedHost, parsedPort), "parseDevicePath succeeds on formatted path");
	checkStrEqual(parsedHost, "127.0.0.1", "parseDevicePath extracted matching host");
	checkEqual(parsedPort, 7777, "parseDevicePath extracted matching port");

	// Non-local host and distinct port
	const std::string path2 = rg2::formatDevicePath("192.168.1.100", 7800);
	check(rg2::parseDevicePath(path2.c_str(), parsedHost, parsedPort), "parseDevicePath succeeds on custom host/port");
	checkStrEqual(parsedHost, "192.168.1.100", "parseDevicePath extracted custom host");
	checkEqual(parsedPort, 7800, "parseDevicePath extracted custom port");

	// Legacy path fallback to 127.0.0.1:7777
	const char* legacyPath = "\\\\?\\usb#vid_0ffc&pid_0002#clavia_g2_virtual#{cb3ed981-6125-4047-bc2a-292e370cc89a}";
	check(rg2::parseDevicePath(legacyPath, parsedHost, parsedPort), "parseDevicePath succeeds on legacy path");
	checkStrEqual(parsedHost, "127.0.0.1", "Legacy path defaults to 127.0.0.1");
	checkEqual(parsedPort, 7777, "Legacy path defaults to port 7777");

	// Non-Clavia path rejected
	check(!rg2::parseDevicePath("\\\\?\\usb#vid_1234&pid_5678#xyz#{...}", parsedHost, parsedPort),
		  "Invalid device path rejected");

	/* ------------------------------------------------------------- case 2.
	 * INI parsing: section-based multi-instance configuration. */
	const std::string iniSections = "[Instance1]\n"
									"Host = 127.0.0.1\n"
									"Port = 7777\n"
									"Name = Red Gecko Track 1\n"
									"\n"
									"[Instance2]\n"
									"Host = 127.0.0.1\n"
									"Port = 7778\n"
									"Name = Red Gecko Track 2\n";

	std::vector<rg2::InstanceEntry> iniEntries;
	check(rg2::parseSetupApiIni(iniSections, iniEntries), "parseSetupApiIni parses multi-section ini");
	checkEqual(iniEntries.size(), 2u, "Parsed exactly 2 instances from ini sections");
	if (iniEntries.size() == 2)
	{
		checkStrEqual(iniEntries[0].host, "127.0.0.1", "Entry 0 host matches");
		checkEqual(iniEntries[0].port, 7777, "Entry 0 port matches");
		checkStrEqual(iniEntries[0].name, "Red Gecko Track 1", "Entry 0 name matches");

		checkStrEqual(iniEntries[1].host, "127.0.0.1", "Entry 1 host matches");
		checkEqual(iniEntries[1].port, 7778, "Entry 1 port matches");
		checkStrEqual(iniEntries[1].name, "Red Gecko Track 2", "Entry 1 name matches");
	}

	/* ------------------------------------------------------------- case 3.
	 * INI parsing: [Instances] list format. */
	const std::string iniList = "[Instances]\n"
								"127.0.0.1:7777\n"
								"127.0.0.1:7779\n";

	std::vector<rg2::InstanceEntry> listEntries;
	check(rg2::parseSetupApiIni(iniList, listEntries), "parseSetupApiIni parses list format");
	checkEqual(listEntries.size(), 2u, "Parsed 2 instances from list format");
	if (listEntries.size() == 2)
	{
		checkEqual(listEntries[0].port, 7777, "List entry 0 port matches");
		checkEqual(listEntries[1].port, 7779, "List entry 1 port matches");
	}

	/* ------------------------------------------------------------- case 4.
	 * JSON parsing and serialization roundtrip. */
	std::vector<rg2::InstanceEntry> originalEntries = {{"127.0.0.1", 7777, "RG2 Instance 1", 1001},
													   {"127.0.0.1", 7778, "RG2 Instance 2", 1002}};

	const std::string jsonOut = rg2::serializeInstancesJson(originalEntries);
	check(jsonOut.find("7777") != std::string::npos, "JSON output contains port 7777");
	check(jsonOut.find("7778") != std::string::npos, "JSON output contains port 7778");
	check(jsonOut.find("1001") != std::string::npos, "JSON output contains pid 1001");

	std::vector<rg2::InstanceEntry> parsedJsonEntries;
	check(rg2::parseInstancesJson(jsonOut, parsedJsonEntries), "parseInstancesJson parses serialized JSON");
	checkEqual(parsedJsonEntries.size(), 2u, "Parsed 2 instances from JSON");
	if (parsedJsonEntries.size() == 2)
	{
		checkEqual(parsedJsonEntries[0].port, 7777, "JSON entry 0 port matches");
		checkEqual(parsedJsonEntries[0].pid, 1001, "JSON entry 0 pid matches");
		checkStrEqual(parsedJsonEntries[0].name, "RG2 Instance 1", "JSON entry 0 name matches");

		checkEqual(parsedJsonEntries[1].port, 7778, "JSON entry 1 port matches");
		checkEqual(parsedJsonEntries[1].pid, 1002, "JSON entry 1 pid matches");
		checkStrEqual(parsedJsonEntries[1].name, "RG2 Instance 2", "JSON entry 1 name matches");
	}

	/* ------------------------------------------------------------- case 5.
	 * Atomic register and unregister on filesystem. */
	const uint32_t myPid = static_cast<uint32_t>(getpid());
	const std::string tempRegistryFile = "/tmp/t0_rg2_instances_test_" + std::to_string(myPid) + ".json";
	std::remove(tempRegistryFile.c_str());

	check(rg2::registerInstance(tempRegistryFile, myPid, 7777, "Test Plugin A"), "Register instance 1 succeeds");
	check(rg2::registerInstance(tempRegistryFile, myPid, 7778, "Test Plugin B"), "Register instance 2 succeeds");

	std::vector<rg2::InstanceEntry> fileEntries;
	{
		std::ifstream ifs(tempRegistryFile);
		std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
		check(rg2::parseInstancesJson(content, fileEntries), "Read back written registry file");
		checkEqual(fileEntries.size(), 2u, "Registry file has 2 entries");
	}

	check(rg2::unregisterInstance(tempRegistryFile, myPid, 7777), "Unregister instance 1 succeeds");
	{
		std::ifstream ifs(tempRegistryFile);
		std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
		fileEntries.clear();
		check(rg2::parseInstancesJson(content, fileEntries), "Read back registry after unregister");
		checkEqual(fileEntries.size(), 1u, "Registry file has 1 remaining entry");
		if (!fileEntries.empty())
		{
			checkEqual(fileEntries[0].port, 7778, "Remaining entry is port 7778");
		}
	}

	check(rg2::unregisterInstance(tempRegistryFile, myPid, 7778), "Unregister instance 2 succeeds");
	std::remove(tempRegistryFile.c_str());


	/* ------------------------------------------------------------- case 6.
	 * InstanceRegistry manager with dynamic fallback. */
	rg2::InstanceRegistry registry;
	registry.setIniPath("/nonexistent/setupapi.ini");
	registry.addDiscoveryPath("/nonexistent/instances.json");

	check(registry.refresh(true), "Registry refresh succeeds on nonexistent paths");
	checkEqual(registry.count(), 1u, "Falls back to 1 default instance when no config exists");
	checkEqual(registry.get(0).port, 7777, "Default fallback instance is port 7777");
	checkStrEqual(registry.get(0).host, "127.0.0.1", "Default fallback instance is 127.0.0.1");

	const std::string devPath = registry.getDevicePath(0);
	check(devPath.find("#rg2_127.0.0.1_7777#") != std::string::npos, "Default device path formatted properly");

	if (failures == 0)
		printf("PASS t0_instance_registry: all cases passed\n");
	return failures == 0 ? 0 : 1;
}

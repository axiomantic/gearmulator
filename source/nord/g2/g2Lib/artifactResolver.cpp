// Task REPO-5. Design section 4.2.

#include "artifactResolver.h"

#include <cstdlib>

// <filesystem> IS NOT AVAILABLE HERE and this is not a style choice.
//
// The root CMakeLists.txt sets CMAKE_OSX_DEPLOYMENT_TARGET to 10.13 or 10.12,
// and std::filesystem was introduced in macOS 10.15. Compiling
// std::filesystem::status against this tree's deployment target is a hard
// error: "'path' is unavailable: introduced in macOS 10.15". baseLib takes the
// same split for the same reason and states it at baseLib/filesystem.cpp:8.
//
// baseLib::filesystem::isDirectory() is the house function for this question
// and this file DELIBERATELY does not call it. Its USE_DIRENT branch discards
// stat()'s return value and then reads statbuf.st_mode, so a path that does not
// exist -- which is precisely the case REPO-5's check drives -- reads an
// uninitialised struct. The behaviour is undefined and the gate would be
// unreliable in exactly the case it exists to assert. The defect is reported
// upstream; it is not silently inherited here.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <sys/stat.h>
#endif

namespace g2
{
	namespace
	{
		// Returns true only when _path names an EXISTING DIRECTORY. Every other
		// answer -- absent, unreadable, a file, a broken symlink -- is false.
		// Never throws.
		bool isExistingDirectory(const char* _path)
		{
#ifdef _WIN32
			const DWORD attributes = GetFileAttributesA(_path);
			if(attributes == INVALID_FILE_ATTRIBUTES)
				return false;
			return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
			struct stat statBuffer;
			// The return value is CHECKED. Reading st_mode after a failed stat()
			// reads an uninitialised struct.
			if(stat(_path, &statBuffer) != 0)
				return false;
			return S_ISDIR(statBuffer.st_mode);
#endif
		}
	}

	std::string EnvArtifactResolver::resolve(std::string& _why)
	{
		// Nothing below can throw. std::getenv does not throw, isExistingDirectory
		// does not throw, and the only remaining escape is std::bad_alloc from a
		// std::string assignment, which no caller could handle.

		_why.clear();

		const char* const value = std::getenv("NMG2_ARTIFACTS");

		// An empty value counts as unset. Windows removes a variable by
		// assigning it the empty string through _putenv_s, so a build that
		// treated "" as a path would mean something different on Windows than
		// it means on Linux and macOS, and the Python half of this task would
		// then disagree with this one on the same input.
		if(!value || value[0] == '\0')
		{
			_why = g_artifactUnavailableMessage;
			return {};
		}

		// A path that does not exist, a path the process cannot stat, and a path
		// that exists but is not a directory all land here, and all three give
		// the SAME result the plan's check requires of the unset case.
		if(!isExistingDirectory(value))
		{
			_why = g_artifactUnavailableMessage;
			return {};
		}

		return std::string(value);
	}
}

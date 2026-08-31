// Task REPO-5. Design section 4.2.

#include "artifactResolver.h"

#include <cstdlib>
#include <cstdio>

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

		// Returns true only when _path names an EXISTING FILE. Every other
		// answer -- absent, unreadable, a directory, a broken symlink -- is
		// false. Never throws.
		//
		// Mirrors the Python half's os.path.isfile() call: a path that is not
		// there returns False rather than raising, so neither half needs an
		// exception handler to satisfy the never-raises contract.
		bool isExistingFile(const char* _path)
		{
#ifdef _WIN32
			const DWORD attributes = GetFileAttributesA(_path);
			if(attributes == INVALID_FILE_ATTRIBUTES)
				return false;
			// A directory is not a file. Anything else that exists -- regular
			// file, symlink, junction -- counts as a file for this check.
			return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
			struct stat statBuffer;
			if(stat(_path, &statBuffer) != 0)
				return false;
			return S_ISREG(statBuffer.st_mode);
#endif
		}

		// Concatenates a string literal with a runtime value to form one of
		// the THREE messages design section 4.2 names. The wording is fixed by
		// the design and must be word-for-word identical to the Python half in
		// nmg2_tools/artifacts.py.
		//
		// The buffer must be large enough for the longest message. The
		// longest is message 3 with a 4096-byte path: 79 bytes of fixed text
		// plus a name and a path. 8 KiB is well above that and matches the
		// PATH_MAX convention without depending on it.
		void writeNoDirectoryMessage(char* _out, const char* _value)
		{
			std::snprintf(_out, 8192,
				"firmware artifact not available (NMG2_ARTIFACTS names no directory: %s)",
				_value);
		}

		void writeNotFoundMessage(char* _out, const char* _name, const char* _value)
		{
			std::snprintf(_out, 8192,
				"firmware artifact not available (%s not found under NMG2_ARTIFACTS: %s)",
				_name, _value);
		}
	}

	std::string EnvArtifactResolver::resolve(std::string& _why, const char* _name)
	{
		// Nothing below can throw. std::getenv does not throw, the isExisting*
		// helpers do not throw, std::snprintf does not throw, and the only
		// remaining escape is std::bad_alloc from a std::string assignment,
		// which no caller could handle.

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

		// A path that does not exist, a path the process cannot stat, and a
		// path that exists but is not a directory all land here, and all three
		// give message 2. The message echoes the variable's value unchanged,
		// which is the wording the Python half in nmg2_tools/artifacts.py
		// spells as `_message_no_directory(value)`.
		if(!isExistingDirectory(value))
		{
			char buffer[8192];
			writeNoDirectoryMessage(buffer, value);
			_why = buffer;
			return {};
		}

		// The directory exists. If a name was asked for and the file is not in
		// the directory, message 3 fires. The caller chose _name and we echo
		// both it and the variable's value, which is the wording the Python
		// half spells as `_message_not_found(name, value)`.
		//
		// A null _name means the caller did not ask for a file, which is the
		// shape firmwareState.h and gatedFixture.h use today. The directory
		// alone is then enough to succeed and message 3 is unreachable.
		if(_name)
		{
			// Build the candidate path "<value>/<name>". The path joins with a
			// single forward slash on every platform -- POSIX path semantics
			// accept it on macOS and Linux, and Windows accepts forward
			// slashes in the path part as well as backslashes. The Python
			// half uses os.path.join() which does the same.
			char candidate[8192];
			std::snprintf(candidate, sizeof(candidate), "%s/%s", value, _name);

			if(!isExistingFile(candidate))
			{
				char buffer[8192];
				writeNotFoundMessage(buffer, _name, value);
				_why = buffer;
				return {};
			}
		}

		return std::string(value);
	}
}

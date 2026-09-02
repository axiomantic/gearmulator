/* t1_patch_load_accepted.cpp -- the patch-load framing repair.
 *
 * The framing this file holds. A patch load reaches the device as
 *
 *   [2-byte BE total][message][2-byte BE CRC-16/XMODEM over the message]
 *
 * whose message is itself
 *
 *   [2-byte BE total][0x01][0x28+slot][0x53][0x37][0x00 0x00 0x00]
 *   [16-char entry name field][object chain][2-byte BE CRC over the body]
 *
 * Each total counts its own whole frame including its two prefix bytes, and
 * each CRC sits directly after its body. There is no pad: a pad-to-64 rule was
 * an artifact of a synthetic 4096-byte chunking that never produced a short USB
 * packet, and the real wire terminates on the short last packet with totals of
 * 865 and 14,664 bytes measured there.
 *
 * The conjuncts, and both bind:
 *
 *   1. The oracle. The stream the load path composes equals the Python
 *      composer's compose_patch_load_transfer over the same corpus file, byte
 *      for byte. The Python side is a different implementation of the same
 *      measured rules, written in a different language against the captures
 *      and the reference editor's own composer; a fixture this C++ had
 *      produced would agree with it by construction and would assert nothing.
 *      The oracle's input is the corpus file, its rules are the framing above,
 *      and a missing, empty or non-zero-exit oracle run is a failure of the
 *      case and never an empty report that compares equal to an empty C++ one.
 *
 *   2. The acceptance. The firmware's message worker accepts the composed
 *      load: D0 = 0 at the switch join. See the acceptance section below for
 *      what this file runs and what it does not.
 */

#include "../../g2JucePlugin/g2PatchLoad.h"

#include "../artifactResolver.h"
#include "../internalClient.h"
#include "../transportHub.h"

#include "gatedFixture.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef G2_PATCH_RELATIVE_PATH
#error "G2_PATCH_RELATIVE_PATH is not defined; tests_proto.cmake defines it"
#endif

#ifndef G2_ORACLE_PYTHON
#error "G2_ORACLE_PYTHON is not defined; tests_proto.cmake defines it"
#endif

#ifndef G2_ORACLE_TOOLS_DIR
#error "G2_ORACLE_TOOLS_DIR is not defined; tests_proto.cmake defines it"
#endif

#ifndef G2_ORACLE_WORK_DIR
#error "G2_ORACLE_WORK_DIR is not defined; tests_proto.cmake defines it"
#endif

namespace
{
	// The entry name is the file's own stem, which is what the reference
	// editor puts in the field, and it is derived from G2_PATCH_RELATIVE_PATH
	// rather than written a second time: a name spelled here and a file named
	// there could drift apart and the composed field would still look valid.
	const char* g_patchRelativePath = G2_PATCH_RELATIVE_PATH;

	// The oracle program is spilled to the build tree at run time and is not a
	// committed file.
	const char* g_oracleProgram = R"PYTHON(
import sys

sys.path.insert(0, sys.argv[1])

from nmg2_tools import wire_compose

compose = getattr(wire_compose, sys.argv[5])

with open(sys.argv[4], "wb") as handle:
    handle.write(compose(sys.argv[2], sys.argv[3]))
)PYTHON";

	std::string entryNameFromPath(const std::string& _path)
	{
		const std::string::size_type slash = _path.find_last_of("/\\");
		std::string stem = slash == std::string::npos ? _path : _path.substr(slash + 1);

		const std::string::size_type dot = stem.find_last_of('.');
		if(dot != std::string::npos)
			stem = stem.substr(0, dot);

		return stem;
	}

	std::vector<uint8_t> readFile(const std::string& _path)
	{
		std::ifstream in(_path.c_str(), std::ios::binary);
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	bool writeFile(const std::string& _path, const std::string& _text)
	{
		std::ofstream out(_path.c_str(), std::ios::binary | std::ios::trunc);
		if(!out.is_open())
			return false;
		out.write(_text.data(), static_cast<std::streamsize>(_text.size()));
		out.close();
		return out.good();
	}

	std::string quotedArgument(const std::string& _text)
	{
		return "\"" + _text + "\"";
	}

	// Runs the oracle over one corpus file and returns the transfer it composed.
	//
	// A non-zero exit status, a missing file or an empty file is a failure and
	// never an empty report: an empty expectation compares equal to an empty
	// observation, which is the shape this leg exists to refuse.
	bool oracleTransfer(const std::string& _patchPath, const std::string& _name, const std::string& _function,
		std::vector<uint8_t>& _transfer, std::string& _why)
	{
		const std::string work       = G2_ORACLE_WORK_DIR;
		const std::string scriptPath = work + "/t1_patch_load_accepted.oracle.py";
		const std::string outputPath = work + "/t1_patch_load_accepted." + _function + ".bin";

		std::remove(outputPath.c_str());

		if(!writeFile(scriptPath, g_oracleProgram))
		{
			_why = "the oracle program could not be written to " + scriptPath;
			return false;
		}

		std::string command = quotedArgument(std::string(G2_ORACLE_PYTHON)) + " " + quotedArgument(scriptPath)
			+ " " + quotedArgument(std::string(G2_ORACLE_TOOLS_DIR))
			+ " " + quotedArgument(_patchPath)
			+ " " + quotedArgument(_name)
			+ " " + quotedArgument(outputPath)
			+ " " + quotedArgument(_function);

#ifdef _WIN32
		// cmd.exe strips the outer pair of quotes of the whole command line, so
		// a command whose first token is quoted needs one more pair.
		command = "\"" + command + "\"";
#endif

		const int status = std::system(command.c_str());
		if(status != 0)
		{
			_why = "the oracle exited with status " + std::to_string(status) + ": " + command;
			return false;
		}

		_transfer = readFile(outputPath);

		if(_transfer.empty())
		{
			_why = "the oracle wrote no bytes at " + outputPath;
			return false;
		}

		return true;
	}

	// The stream the load path composes, as the hub drained it.
	//
	// Read out of the hub and not out of the composer's buffer. A composer that
	// built the right bytes and originated nothing is green on a buffer test
	// and red here.
	bool composedStream(const std::vector<uint8_t>& _patch, const std::string& _name, const bool _transferLevel,
		std::vector<uint8_t>& _stream, std::string& _why)
	{
		constexpr std::size_t kMaxFrameBytes = g2::g_maxPatchLoadMessageBytes + 4;

		g2::TransportHub   hub(kMaxFrameBytes, 4);
		g2::InternalClient client(hub, kMaxFrameBytes, 4);

		std::vector<uint8_t> scratch(kMaxFrameBytes);

		g2::Pch2LoadResult result = g2::Pch2LoadResult::Loaded;

		if(_transferLevel)
		{
			const std::size_t message = g2::pch2ComposePatchLoad(_patch.data(), _patch.size(), _name.c_str(), 0,
				scratch.data() + 2, scratch.size() - 4, result);

			if(result == g2::Pch2LoadResult::Loaded && !client.sendTransfer(scratch.data(), message))
				result = g2::Pch2LoadResult::SendRefused;
		}
		else
		{
			result = g2::pch2LoadFramed(_patch.data(), _patch.size(), _name.c_str(), 0,
				client, scratch.data(), scratch.size());
		}

		if(result != g2::Pch2LoadResult::Loaded)
		{
			_why = std::string("the load path answered ") + g2::pch2LoadResultName(result);
			return false;
		}

		std::vector<g2::StampedFrame> drained(4);
		const std::size_t got = hub.drainToDevice(drained.data(), drained.size());

		// One transfer, not a sequence. The repair replaces the per-object
		// framing rather than wrapping it, so a run that drained several frames
		// is still originating objects one at a time.
		if(got != 1)
		{
			_why = "the hub drained " + std::to_string(got) + " frame(s) where one transfer was expected";
			return false;
		}

		const g2::ProtocolFrame& frame = drained[0].frame;

		if(frame.data == nullptr || frame.size == 0)
		{
			_why = "the hub drained an empty frame";
			return false;
		}

		_stream.assign(frame.data, frame.data + frame.size);
		return true;
	}

	// Prints the first differing byte with both offsets, then the lengths.
	void reportDifference(const std::vector<uint8_t>& _observed, const std::vector<uint8_t>& _expected)
	{
		const std::size_t shorter = _observed.size() < _expected.size() ? _observed.size() : _expected.size();

		for(std::size_t i = 0; i < shorter; ++i)
		{
			if(_observed[i] != _expected[i])
			{
				std::printf("  first difference at offset %zu: composed 0x%02X, oracle 0x%02X\n",
					i, _observed[i], _expected[i]);
				break;
			}
		}

		std::printf("  composed %zu byte(s), oracle %zu byte(s)\n", _observed.size(), _expected.size());
	}
}

int main()
{
	g2::EnvArtifactResolver resolver;
	g2::test::GatedCounters counters;

	g2::test::runGated(resolver, std::cout, counters, [&]() -> bool
	{
		std::string why;
		const std::string directory = resolver.resolve(why);

		if(directory.empty())
		{
			std::cout << "FAIL " << why << std::endl;
			return false;
		}

		const std::string patchPath = directory + "/" + g_patchRelativePath;
		const std::string name      = entryNameFromPath(g_patchRelativePath);

		const std::vector<uint8_t> patch = readFile(patchPath);

		if(patch.empty())
		{
			std::cout << "FAIL the corpus patch is empty or unreadable at " << patchPath << std::endl;
			return false;
		}

		bool ok = true;

		// ------------------------------------------------ conjunct 1
		//
		// Both levels are compared, and the reason is that they are two
		// different byte streams and only one of them was measured reaching the
		// worker's accept path. The message is what the load path originates
		// and what the firmware was measured accepting; the transfer is the
		// same message inside a second envelope of the same shape, which the
		// endpoint builds for the callers that carry it. A run comparing only
		// one would leave the other free to drift.
		struct Case
		{
			const char* label;
			const char* oracleFunction;
			bool        transferLevel;
		};

		const Case cases[] =
		{
			{ "message",  "compose_patch_load",          false },
			{ "transfer", "compose_patch_load_transfer", true  }
		};

		for(const Case& one : cases)
		{
			std::vector<uint8_t> composed;
			std::vector<uint8_t> expected;

			if(!composedStream(patch, name, one.transferLevel, composed, why))
			{
				std::cout << "FAIL the load path composed no " << one.label << ": " << why << std::endl;
				ok = false;
				continue;
			}

			if(!oracleTransfer(patchPath, name, one.oracleFunction, expected, why))
			{
				std::cout << "FAIL the oracle produced no " << one.label << ": " << why << std::endl;
				ok = false;
				continue;
			}

			if(composed == expected)
			{
				std::cout << "PASS the composed " << one.label << " equals " << one.oracleFunction
				          << ", " << composed.size() << " bytes, entry name \"" << name << "\"" << std::endl;
			}
			else
			{
				std::cout << "FAIL the composed " << one.label << " differs from " << one.oracleFunction
				          << std::endl;
				reportDifference(composed, expected);
				ok = false;
			}
		}

		// ------------------------------------------------ conjunct 2
		//
		// The acceptance was measured, and it was measured on these bytes.
		// pch2ComposePatchLoad was run against the same corpus file under a
		// booting instrument -- the firmware under an in-process debug stub,
		// delivery through the board's own hub, the SRAM window mapped by
		// stretching CS4 over {0x14000000, 0x1C000000} with SRAM_20000800.bin
		// at absolute 0x20000800, the panel hole below 0x20000000 answering
		// zero-read and the span clear of SDRAM -- and the message worker's
		// status word read at 0x3004C1E4, the post-`move.b d2,d0` retirement
		// point. It read 0: accept. The composed bytes were byte-identical to
		// the instrument's own composition of the same patch, so the verdict is
		// a verdict on this composer and not on a second one.
		//
		// That run is not repeated here and this file does not claim it. The
		// instrument boots the machine and takes minutes; this file compares
		// bytes and takes milliseconds, which is what makes it a regression
		// gate rather than a measurement. What it holds is that the bytes the
		// verdict was taken on are still the bytes this composer emits: the
		// oracle above pins them, and a composition that drifts from them goes
		// red here long before anyone boots a machine again.
		std::cout << g2::test::g_verdictNotVerified
		          << " the firmware's accept verdict is not re-measured by this file: it compares the"
		             " composed bytes, and the D0 = 0 reading at 0x3004C1E4 was taken by a booting"
		             " instrument on bytes this comparison pins" << std::endl;

		return ok;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}

// The test console.
//
// `g2TestConsole --boot` boots the Clavia OS image directly at 0x30000400 and
// PRINTS display 0's 32 character cells as they stand in main memory. It
// asserts nothing: the assertions live in `t1_boot`, which is a ctest target.
// This program is the operator-facing window onto the same boot, and its output
// is meant to be read by a person bringing the machine up.
//
// Its absence was itself a defect. Without this file
// `g2TestConsole/CMakeLists.txt` generates a placeholder translation unit whose
// `main` returns 0 immediately, so the acceptance command exits 0 and prints
// nothing. The generator stops firing the moment this file is present.
//
// Every address and every window below has the same provenance as
// `g2Lib/test/t1_boot.cpp` and is documented there. Two of them -- CS0's base
// and CS4's base -- are INVENTED BY THIS HARNESS because no authority records
// them, and they are labelled at their site rather than presented as measured.

#include "board.h"
#include "memoryMap.h"
#include "artifactResolver.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	// The display buffer base, confirmed at 0x30057040 as
	// `addil #808062392,%d0`.
	constexpr uint32_t g_displayBase   = 0x302A0DB8u;
	constexpr uint32_t g_displayStride = 298u;
	constexpr uint32_t g_lineWidth     = 16u;

	constexpr uint32_t g_entryPc = 0x30000400u;
	constexpr uint32_t g_entrySp = 0x30400000u;
	constexpr int      g_regPc   = 17;

	// MEASURED: the loader's `movel #0x10000001,%d0` /
	// `movec %d0,%mbar` at loader offset 0x1E. The OS never writes MBAR, so a
	// direct boot of the OS image makes this the harness's job.
	constexpr uint32_t g_mbarBase = 0x10000000u;

	// MEASURED: CSAR2 = $1200 and CSMR2 = $007F0001 at
	// loader offsets 0x70 and 0x7c give 0x12000000..0x127FFFFF.
	constexpr uint32_t g_cs2Base = 0x12000000u;
	constexpr uint32_t g_cs2Size = 0x00800000u;

	// INVENTED BY THIS HARNESS. No authority records CS0's or CS4's base; plan
	// section 4.2 register row 18 is still open on both. Neither value below is
	// a measurement and neither may be copied into a shipped header.
	constexpr uint32_t g_cs0Base = 0x00000000u;
	constexpr uint32_t g_cs0Size = 0x00020000u;
	constexpr uint32_t g_cs4Base = 0x14000000u;
	constexpr uint32_t g_cs4Size = 0x00010000u;

	constexpr uint32_t g_cs1Size   = 0x00010000u;
	constexpr uint32_t g_cs5Size   = 0x00000010u;
	constexpr uint32_t g_sdramSize = 0x00800000u;

	constexpr uint32_t g_iterations        = 0xFDE8u;
	constexpr uint32_t g_cyclesPerIteration = 4096u;

	// The SDRAM the firmware executes from. board.cpp leaves Region::Sdram with
	// no target on purpose, so the store is the harness's to supply.
	// Big-endian, matching the part.
	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const size_t _size) : m_bytes(_size, 0u) {}

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			_status = MCF5307_BUS_OK;

			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
			}

			const uint32_t count = uint32_t(_size) / 8u;
			uint32_t value = 0u;

			for(uint32_t i = 0; i < count; ++i)
			{
				value <<= 8;
				const size_t index = size_t(_offset) + i;
				if(index < m_bytes.size())
					value |= m_bytes[index];
			}

			return value;
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5307_bus_status& _status) override
		{
			_status = MCF5307_BUS_OK;

			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return;
			}

			const uint32_t count = uint32_t(_size) / 8u;

			for(uint32_t i = 0; i < count; ++i)
			{
				const size_t index = size_t(_offset) + i;
				if(index >= m_bytes.size())
					continue;
				const int shift = int(8u * (count - 1u - i));
				m_bytes[index] = uint8_t((_value >> shift) & 0xffu);
			}
		}

		bool place(const uint32_t _offset, const std::vector<uint8_t>& _image)
		{
			if(size_t(_offset) + _image.size() > m_bytes.size())
				return false;
			std::memcpy(m_bytes.data() + _offset, _image.data(), _image.size());
			return true;
		}

	private:
		std::vector<uint8_t> m_bytes;
	};

	std::vector<uint8_t> readFile(const std::string& _path)
	{
		std::ifstream in(_path, std::ios::binary);
		if(!in)
			return {};
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	g2::BoardConfig makeConfig()
	{
		g2::BoardConfig config;

		config.memory.cs0   = {g_cs0Base,       g_cs0Size};
		config.memory.cs1   = {g2::g_cs1Base,   g_cs1Size};
		config.memory.cs2   = {g_cs2Base,       g_cs2Size};
		// CS3 is left ABSENT: this task boots "with a stubbed CS3", and an
		// absent window reports unmapped and logs, which is what a stub should
		// do. An answer of zero would be indistinguishable from a device.
		config.memory.cs4   = {g_cs4Base,       g_cs4Size};
		config.memory.cs5   = {g2::g_cs5Base,   g_cs5Size};
		config.memory.mbar  = {g_mbarBase,      g2::g_simSpaceSize};
		config.memory.sdram = {g2::g_sdramBase, g_sdramSize};

		return config;
	}

	// Reads through Board::onRead, which is the exact callback the Board hands
	// to mcf5307_create and therefore the path the core itself takes.
	std::string readDisplayLine(g2::Board& _board, const uint32_t _display, const uint32_t _line)
	{
		const uint32_t base = g_displayBase + _display * g_displayStride + _line * g_lineWidth;

		std::string out;
		out.reserve(g_lineWidth);

		for(uint32_t col = 0; col < g_lineWidth; ++col)
		{
			mcf5307_bus_status status = MCF5307_BUS_OK;
			// The size argument is a BYTE COUNT, not a bit width: Board::onRead
			// hands it to busWidthBits, whose switch answers 1, 2 and 4 and maps
			// everything else to the zero width the decode refuses. A byte read
			// is 1. Passing 8 here made every cell an illegal-size access that
			// returned zero, so the display printed as NULs whatever it held.
			out.push_back(char(g2::Board::onRead(&_board, base + col, 1, &status) & 0xffu));
		}

		return out;
	}

	std::string escapedLine(const std::string& _line)
	{
		std::string out = "\"";
		for(const char c : _line)
		{
			const auto u = uint8_t(c);
			if(u >= 0x20 && u < 0x7f)
			{
				out.push_back(c);
				continue;
			}
			char buf[8];
			std::snprintf(buf, sizeof buf, "\\x%02X", unsigned(u));
			out += buf;
		}
		out += "\"";
		return out;
	}

	int boot()
	{
		g2::EnvArtifactResolver resolver;
		std::string why;

		const std::string directory = resolver.resolve(why, "CODE_30000400.bin");

		if(directory.empty())
		{
			std::cout << why << std::endl;
			return 2;
		}

		const std::vector<uint8_t> code = readFile(directory + "/CODE_30000400.bin");

		if(code.empty())
		{
			std::cout << "CODE_30000400.bin is empty or unreadable under " << directory << std::endl;
			return 2;
		}

		g2::Board board(makeConfig());
		Ram ram(g_sdramSize);

		if(!ram.place(g_entryPc - g2::g_sdramBase, code))
		{
			std::cout << "the image does not fit the configured SDRAM window" << std::endl;
			return 2;
		}

		board.memory().attach(g2::Region::Sdram, &ram);

		mcf5307_ctx* mcu = mcf5307_create(&board, &g2::Board::onRead, &g2::Board::onWrite, nullptr);

		if(!mcu)
		{
			std::cout << "mcf5307_create returned no context" << std::endl;
			return 2;
		}

		mcf5307_reset(mcu, g_entrySp, g_entryPc);

		uint32_t iteration = 0;

		for(; iteration < g_iterations; ++iteration)
		{
			mcf5307_exec(mcu, g_cyclesPerIteration);

			if(mcf5307_halted(mcu))
				break;
		}

		const uint32_t pc = mcf5307_get_reg(mcu, g_regPc);
		const bool halted = mcf5307_halted(mcu) != 0;
		const bool faulted = mcf5307_faulted(mcu) != 0;

		std::cout << "iterations=" << iteration
		          << " halted=" << (halted ? 1 : 0)
		          << " faulted=" << (faulted ? 1 : 0)
		          << " pc=0x" << std::hex << pc << std::dec << std::endl;

		// The two lines, printed UNTRIMMED and byte for byte. The escaped form
		// is what makes line 0's trailing space visible to a reader; the plain
		// form is what a person actually wants to see.
		const std::string line0 = readDisplayLine(board, 0, 0);
		const std::string line1 = readDisplayLine(board, 0, 1);

		std::cout << "display0.line0=" << escapedLine(line0) << std::endl;
		std::cout << "display0.line1=" << escapedLine(line1) << std::endl;
		std::cout << line0 << std::endl;
		std::cout << line1 << std::endl;

		const auto& log = board.memory().log();
		std::cout << "buslog=" << log.size() << std::endl;
		size_t printed = 0;
		for(const auto& line : log)
		{
			if(printed++ >= 40)
			{
				std::cout << "  (truncated after 40 lines)" << std::endl;
				break;
			}
			std::cout << "  " << line << std::endl;
		}

		mcf5307_destroy(mcu);

		// A boot that produced no banner is not a success, and this program must
		// not report one: an acceptance command that exits 0 while the machine
		// did nothing says nothing. `--boot` reports success only when
		// the firmware actually composed something printable into display 0 and
		// the core is still running; a faulted or halted core is an error exit
		// whose diagnosis is the lines printed above.
		// The firmware clears every display to 0x20 SPACES before it composes
		// anything, so a predicate satisfied by 0x20 is satisfied by a blank
		// screen and reports success for a boot that composed nothing. Content
		// is a printable character the clear cannot write.
		const bool composed = std::any_of(line0.begin(), line0.end(),
			[](const char c) { return uint8_t(c) > 0x20u && uint8_t(c) < 0x7fu; });

		if(!composed || halted || faulted)
			return 1;

		return 0;
	}

	void usage()
	{
		std::cout << "usage: g2TestConsole --boot" << std::endl;
		std::cout << "  --boot  boot CODE_30000400.bin from NMG2_ARTIFACTS and print"
		             " display 0's 32 character cells" << std::endl;
	}
}

int main(int _argc, char** _argv)
{
	// NO ARGUMENT IS NOT A SUCCESS. The placeholder this file replaces exited 0
	// and printed nothing, which is exactly how a milestone check passes against
	// a program that does nothing. An unrecognised invocation is an error here.
	if(_argc < 2)
	{
		usage();
		return 2;
	}

	const std::string command = _argv[1];

	if(command == "--boot")
		return boot();

	usage();
	return 2;
}

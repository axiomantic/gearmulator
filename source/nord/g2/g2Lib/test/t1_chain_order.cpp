// This test needs the Clavia firmware artifacts and skips with a reason when
// NMG2_ARTIFACTS does not resolve.
//
// attachChainCallbacks decides which hardware port carries which chain position.
// The firmware decides the same thing, and the two must agree: chain position 0's
// callbacks belong on the DSP the firmware treats as the head of the chain, and
// chain position N - 1's on the DSP it treats as the tail. The codec edges hang
// off those two positions, so a disagreement puts the machine's audio input and
// output on the wrong DSPs.
//
// The ordering is derived here and never written down as data. This file contains
// no port sequence. It boots the firmware and reads the ordering out of the booted
// machine twice, by two paths that share no arithmetic, and requires them to agree
// before it uses either:
//
//   Derivation A -- the firmware's own table, through g2::readChainOrder, which
//   is the function the Scheduler wires the chain by. Calling the shipped one
//   rather than copying it is what makes the cross-check below a check of the
//   shipped code.
//
//   Derivation B -- the kernel's own DMA constants. The chain's two ends carry
//   the codec's stereo pair and its interior carries the eight-slot inter-DSP
//   bus. So the head is the one port whose receive channel is programmed for two
//   slots and the tail is the one port whose transmit channel is. This reads
//   registers the emulated kernel wrote and shares nothing with derivation A but
//   the machine both were taken from.
//
// Either alone is a single instrument. Two derivations that share no arithmetic
// cannot fail together in silence.
//
// The wiring probe runs on a fresh DspSet and not on the booted one. The booted
// set's ESAIs carry the kernel's own frame geometry and a DMA that moves under
// them, and a probe there would be reading the machine's traffic rather than the
// wiring. A fresh set has clean ESAIs this file programs for a one-slot frame,
// which is what lets readRX(0) report slot 0. The ordering still comes from the
// booted machine; only the ports the probe drives are fresh.
//
// Every verdict is an observable and not an assert(): an assert() is deleted by
// NDEBUG, so a predicate spelled as one is a predicate a release build does not
// have.

#include "gatedFixture.h"

#include "../board.h"
#include "../chainAdapter.h"
#include "../chainOrder.h"
#include "../dspSet.h"
#include "../esaiFrame.h"
#include "../executor.h"
#include "../frame.h"
#include "../memoryMap.h"
#include "../scheduler.h"
#include "../status.h"

#include "dsp56kBase/logging.h"
#include "dsp56kEmu/dma.h"
#include "dsp56kEmu/esai.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;

	void check(const bool _condition, const std::string& _what)
	{
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	std::string hex6(const uint32_t _value)
	{
		char buf[16];
		std::snprintf(buf, sizeof buf, "$%06X", unsigned(_value));
		return buf;
	}

	// ------------------------------------------------ the ESAI underrun log filter
	//
	// The underruns are real and expected in the boot regime. This hides the
	// repetition and nothing else.
	const char* const g_underrunMessage = "ESAI transmit underrun";

	constexpr uint64_t g_underrunLinesKept = 4;

	std::atomic<uint64_t> g_underrunLines{0};

	void filterLog(const std::string& _s)
	{
		if(_s.find(g_underrunMessage) != std::string::npos &&
		   g_underrunLines.fetch_add(1) >= g_underrunLinesKept)
			return;

		std::cout << _s << '\n';
	}

	void installLogFilter()
	{
		if(std::getenv("G2_LOG_ESAI_UNDERRUN"))
			return;

		Logging::setLogFunc(&filterLog);
	}

	// ------------------------------------------------------- the machine placement

	constexpr uint32_t g_entryPc = 0x30000400u;
	constexpr uint32_t g_entrySp = 0x30400000u;

	constexpr int g_regVbr = 18;

	constexpr uint32_t g_vectorTableBase    = 0x30000000u;
	constexpr uint32_t g_vectorTableEntries = 256u;
	constexpr uint32_t g_vectorHandler      = 0x300585CEu;

	constexpr uint32_t g_mbarBase = 0x10000000u;

	constexpr uint32_t g_cs2Base = 0x12000000u;
	constexpr uint32_t g_cs2Size = 0x00800000u;
	constexpr uint32_t g_cs3Size = 0x00010000u;
	constexpr uint32_t g_cs0Base = 0x00000000u;
	constexpr uint32_t g_cs0Size = 0x00020000u;
	constexpr uint32_t g_cs4Base = 0x14000000u;
	constexpr uint32_t g_cs4Size = 0x00010000u;
	constexpr uint32_t g_sdramSize = 0x00800000u;
	constexpr uint32_t g_cs1Size = 0x00010000u;
	constexpr uint32_t g_cs5Size = 0x00000010u;

	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const size_t _size) : m_bytes(_size, 0u) {}

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
			}

			const uint32_t count = uint32_t(_size) / 8u;

			uint32_t value = 0u;

			for(uint32_t i = 0; i < count; ++i)
			{
				const size_t index = size_t(_offset) + i;
				value = (value << 8) | (index < m_bytes.size() ? m_bytes[index] : 0u);
			}

			return value;
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value,
			mcf5307_bus_status& _status) override
		{
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

		config.memory.cs0   = {g_cs0Base,     g_cs0Size};
		config.memory.cs1   = {g2::g_cs1Base, g_cs1Size};
		config.memory.cs2   = {g_cs2Base,     g_cs2Size};
		config.memory.cs3   = {g2::g_cs3Base, g_cs3Size};
		config.memory.cs4   = {g_cs4Base,     g_cs4Size};
		config.memory.cs5   = {g2::g_cs5Base, g_cs5Size};
		config.memory.mbar  = {g_mbarBase,    g2::g_simSpaceSize};
		config.memory.sdram = {g2::g_sdramBase, g_sdramSize};

		return config;
	}

	// -------------------------------------------------------- derivation A's site
	//
	// g2::readChainOrder is called rather than copied here, so what is cross-checked
	// against derivation B is the code the machine runs on.

	// -------------------------------------------------------- derivation B's site
	//
	// The DMA count field $001001 gives two slots -- the codec's stereo pair --
	// and $007001 gives eight, the inter-DSP bus. The receive channel is 2 and
	// the transmit channel is 4.
	constexpr dsp56k::TWord g_dmaRxChannel  = 2u;
	constexpr dsp56k::TWord g_dmaTxChannel  = 4u;
	constexpr dsp56k::TWord g_dcoEndpoint   = 0x001001u;

	// The value read is the first non-zero one and not the live register. DCO is a
	// transfer counter: the emulated DMA runs it down and reloads it, so a register
	// sampled at an arbitrary instant carries wherever the transfer had got to and
	// not the count the kernel programmed. Latching the first non-zero value is what
	// makes this derivation read the kernel's writes.
	struct DcoLatch
	{
		std::vector<dsp56k::TWord> rx;
		std::vector<dsp56k::TWord> tx;

		void reset(const unsigned _count)
		{
			rx.assign(_count, 0u);
			tx.assign(_count, 0u);
		}

		bool complete() const
		{
			for(size_t i = 0; i < rx.size(); ++i)
			{
				if(rx[i] == 0u || tx[i] == 0u)
					return false;
			}

			return !rx.empty();
		}
	};

	void latchFirst(dsp56k::TWord& _slot, const dsp56k::TWord _now)
	{
		if(_slot == 0u && _now != 0u)
			_slot = _now;
	}

	void latchDco(g2::Board& _board, DcoLatch& _latch)
	{
		for(unsigned port = 0; port < _latch.rx.size(); ++port)
		{
			dsp56k::Dma& dma = _board.dspSet().peripherals(port).getDMA();

			latchFirst(_latch.rx[port], dma.getDCO(g_dmaRxChannel));
			latchFirst(_latch.tx[port], dma.getDCO(g_dmaTxChannel));
		}
	}

	// Returns the number of ports whose latched channel count is the two-slot
	// endpoint form, and writes the last such port to _port.
	unsigned endpointPorts(const std::vector<dsp56k::TWord>& _latched, unsigned& _port)
	{
		unsigned found = 0;

		for(unsigned port = 0; port < _latched.size(); ++port)
		{
			if(_latched[port] != g_dcoEndpoint)
				continue;

			_port = port;
			++found;
		}

		return found;
	}

	// ------------------------------------------------------------ the boot itself

	constexpr uint32_t g_iterationBound     = 200000u;
	constexpr uint32_t g_framesPerIteration = 1u;

	bool everyProgramLanded(g2::Board& _board, const unsigned _count)
	{
		for(unsigned port = 0; port < _count; ++port)
		{
			const bool* const landed = _board.dspSet().programLanded(port);

			if(landed == nullptr || !*landed)
				return false;
		}

		return true;
	}

	// ------------------------------------------------------------- the ESAI probe
	//
	// A one-slot frame, which is what lets readRX(0) report slot 0.
	// receiveDspFrame issues getRxWordCount() + 1 calls to execRX and each
	// latches its own slot into the read registers, so a wider frame would leave
	// the last slot there while the injected sample sits in slot 0.
	void enableTransmitter(dsp56k::Esai& _esai)
	{
		_esai.writeTransmitClockControlRegister(0);
		_esai.writeTransmitControlRegister(1u << dsp56k::Esai::M_TE0);
	}

	void enableReceiver(dsp56k::Esai& _esai)
	{
		_esai.writeReceiveClockControlRegister(0);
		_esai.writeReceiveControlRegister(1u << dsp56k::Esai::M_RE0);
	}

	// The two sentinels. Both are non-zero, because a zero compares equal
	// against an unwritten slot whether it crossed or not, and they differ from
	// each other so the ingress probe cannot be satisfied by egress traffic.
	constexpr int32_t g_ingressSentinel   = 0x0A1234;
	constexpr int32_t g_egressSentinelBase = 0x0B0000;
}

int main()
{
	installLogFilter();

	g2::EnvArtifactResolver     resolver;
	g2::test::GatedCounters     counters;

	g2::test::runGated(resolver, std::cout, counters, [&]() -> bool
	{
		std::string why;
		const std::string directory = resolver.resolve(why, "CODE_30000400.bin");

		if(directory.empty())
		{
			std::cout << "FAIL " << why << std::endl;
			return false;
		}

		const std::vector<uint8_t> code = readFile(directory + "/CODE_30000400.bin");

		if(code.empty())
		{
			std::cout << "FAIL CODE_30000400.bin is empty or unreadable under " << directory << std::endl;
			return false;
		}

		g2::Board board(makeConfig());
		Ram       ram(g_sdramSize);

		if(!ram.place(g_entryPc - g2::g_sdramBase, code))
		{
			std::cout << "FAIL the image does not fit the configured SDRAM window" << std::endl;
			return false;
		}

		{
			std::vector<uint8_t> table(g_vectorTableEntries * 4u);

			for(uint32_t entry = 0; entry < g_vectorTableEntries; ++entry)
			{
				for(uint32_t byte = 0; byte < 4u; ++byte)
					table[entry * 4u + byte] =
						uint8_t((g_vectorHandler >> ((3u - byte) * 8u)) & 0xffu);
			}

			if(!ram.place(g_vectorTableBase - g2::g_sdramBase, table))
			{
				std::cout << "FAIL the vector table does not fit the configured SDRAM window" << std::endl;
				return false;
			}
		}

		board.memory().attach(g2::Region::Sdram, &ram);

		board.resetMcu(g_entrySp, g_entryPc);

		if(!board.setMcuReg(g_regVbr, g_vectorTableBase))
		{
			std::cout << "FAIL the core refused VBR at register index " << g_regVbr << std::endl;
			return false;
		}

		g2::SerialExecutor executor;
		g2::Status         schedulerStatus{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(g2::Scheduler::Config(), executor, board, schedulerStatus);

		if(!scheduler)
		{
			std::cout << "FAIL Scheduler::create returned no object; g2::Status = "
			          << uint32_t(schedulerStatus) << std::endl;
			return false;
		}

		const unsigned count = board.dspSet().dspCount();

		// Both derivations need the kernel's own writes, so the loop stops when
		// every slot has taken its program rather than after a fixed count that
		// would either cost the bound every run or sample a machine mid-download.
		bool     converged  = false;
		uint32_t iterations = 0;

		DcoLatch dco;
		dco.reset(count);

		for(uint32_t i = 0; i < g_iterationBound; ++i)
		{
			iterations = i + 1;

			scheduler->runFrames(g_framesPerIteration);

			// Sampled before the exit test, so the iteration that satisfies the
			// predicate still contributes its observation.
			latchDco(board, dco);

			if(everyProgramLanded(board, count) && dco.complete())
			{
				converged = true;
				break;
			}

			if(board.mcuHalted())
				break;
		}

		check(converged,
		      "every slot took a program and armed both codec-edge DMA channels within the "
		      "iteration bound of " + std::to_string(g_iterationBound) + "; the run used " +
		      std::to_string(iterations));

		if(!converged)
			return false;

		/* The wiring is late: the constructor cannot know the order, so it
		 * attaches nothing and runFrames attaches once the firmware's table can
		 * be read. A Scheduler that never reached that point would leave every
		 * ESAI on the library's default callbacks -- silently, with no audio --
		 * and the probe below, which drives its own adapter over a fresh set,
		 * would still pass. */
		check(scheduler->chainAttached(),
		      "the Scheduler wired its chain during the boot, so the order derived below is the "
		      "order the shipped machine is running on");

		// ------------------------------------------------------- derivation A
		std::vector<unsigned> portOfPosition;
		const unsigned named = g2::readChainOrder(board, count, portOfPosition);

		check(named == count,
		      "the firmware's own base table names a port for every one of the " +
		      std::to_string(count) + " chain positions; it names " + std::to_string(named));

		if(named != count)
			return false;

		{
			std::string order;
			for(unsigned position = 0; position < count; ++position)
				order += (position ? ", " : "") + std::to_string(portOfPosition[position]);

			std::cout << "chain-order: position -> port = " << order << std::endl;
		}

		// It must be a permutation. A table that named the same port twice would
		// leave a DSP unwired and another driving two positions.
		{
			std::vector<unsigned> seen(count, 0u);

			for(unsigned position = 0; position < count; ++position)
				++seen[portOfPosition[position]];

			bool permutation = true;
			for(unsigned port = 0; port < count; ++port)
				permutation = permutation && seen[port] == 1u;

			check(permutation,
			      "the firmware's ordering is a permutation of the hardware ports: every port "
			      "carries exactly one chain position");

			if(!permutation)
				return false;
		}

		// ------------------------------------------------------- derivation B
		unsigned headPortByDma = count;
		unsigned tailPortByDma = count;

		const unsigned twoSlotReceivers    = endpointPorts(dco.rx, headPortByDma);
		const unsigned twoSlotTransmitters = endpointPorts(dco.tx, tailPortByDma);

		check(twoSlotReceivers == 1u,
		      "exactly one port's receive channel is programmed for the two-slot codec pair "
		      "(DCO2 = " + hex6(g_dcoEndpoint) + "), design section 2.4; " +
		      std::to_string(twoSlotReceivers) + " are");

		check(twoSlotTransmitters == 1u,
		      "exactly one port's transmit channel is programmed for the two-slot codec pair "
		      "(DCO4 = " + hex6(g_dcoEndpoint) + "), design section 2.4; " +
		      std::to_string(twoSlotTransmitters) + " are");

		if(twoSlotReceivers != 1u || twoSlotTransmitters != 1u)
			return false;

		std::cout << "chain-order: DMA head port = " << headPortByDma
		          << ", DMA tail port = " << tailPortByDma << std::endl;

		// ------------------------------- the two derivations must agree
		const unsigned headPort = portOfPosition[0];
		const unsigned tailPort = portOfPosition[count - 1u];

		check(headPort == headPortByDma,
		      "the firmware's table and the kernel's own DMA constants name the SAME head: the "
		      "table puts chain position 0 on port " + std::to_string(headPort) +
		      " and the two-slot receive channel is on port " + std::to_string(headPortByDma));

		check(tailPort == tailPortByDma,
		      "the firmware's table and the kernel's own DMA constants name the SAME tail: the "
		      "table puts chain position " + std::to_string(count - 1u) + " on port " +
		      std::to_string(tailPort) + " and the two-slot transmit channel is on port " +
		      std::to_string(tailPortByDma));

		if(headPort != headPortByDma || tailPort != tailPortByDma)
			return false;

		// ------------------------------------------------------- the wiring probe
		//
		// The adapter is declared first so it outlives every callback installed
		// on the set.
		g2::ChainAdapter adapter(count, 1u, g2::ChainTopology::Ring, 4u);
		g2::DspSet       probe;

		check(g2::attachChainCallbacks(adapter, probe, portOfPosition) == g2::Status::Ok,
		      "the installer accepted the firmware's own position-to-port order");

		// After the install, never before. Enabling a transmitter drives one
		// execTX out of writeTransmitControlRegister, and the callback that
		// fires is whichever one is installed at that instant.
		for(unsigned port = 0; port < count; ++port)
		{
			enableTransmitter(probe.peripherals(port).getEsai());
			enableReceiver(probe.peripherals(port).getEsai());
		}

		// Ingress. injectCodecSource writes the codec pair into the read frame of
		// audio mailbox 0, which is the mailbox chain position 0's receive
		// callback reads. The port that sees it is therefore the port carrying
		// chain position 0.
		{
			g2::Frame source{};
			source.slot[0] = g_ingressSentinel;
			source.slot[1] = g_ingressSentinel + 1;

			adapter.injectCodecSource(source);

			for(unsigned port = 0; port < count; ++port)
			{
				dsp56k::Esai& esai = probe.peripherals(port).getEsai();

				check(g2::receiveDspFrame(esai) == esai.getRxWordCount() + 1u,
				      "port " + std::to_string(port) + "'s audio receive frame ran, so its "
				      "reading below is the callback's answer and not an unenabled port's zero");

				const uint32_t observed = esai.readRX(0u);
				const bool     isHead   = port == headPort;

				check((observed == uint32_t(g_ingressSentinel)) == isHead,
				      "the codec ingress reaches the DSP the firmware treats as the HEAD and no "
				      "other: port " + std::to_string(port) + " is " +
				      (isHead ? "the head (port " : "not the head (the head is port ") +
				      std::to_string(headPort) + "), so it must " + (isHead ? "" : "not ") +
				      "carry " + hex6(uint32_t(g_ingressSentinel)) + "; it carries " +
				      hex6(observed));
			}
		}

		// Egress. Every port transmits a value of its own. Only the port
		// carrying chain position N - 1 writes the last audio mailbox, and
		// extractCodecSink reads exactly that one, so the sink's contents name
		// the port the wiring made the tail.
		{
			for(unsigned port = 0; port < count; ++port)
			{
				dsp56k::Esai& esai = probe.peripherals(port).getEsai();

				esai.writeTX(0u, dsp56k::TWord(g_egressSentinelBase + int32_t(port)));

				check(g2::transmitDspFrame(esai) == esai.getTxWordCount() + 1u,
				      "port " + std::to_string(port) + "'s audio transmit frame ran");
			}

			g2::Frame sink{};
			adapter.extractCodecSink(sink);

			check(sink.slot[0] == g_egressSentinelBase + int32_t(tailPort),
			      "the codec egress is fed by the DSP the firmware treats as the TAIL, port " +
			      std::to_string(tailPort) + ", whose transmit carried " +
			      hex6(uint32_t(g_egressSentinelBase + int32_t(tailPort))) + "; the sink carries " +
			      hex6(uint32_t(sink.slot[0])));
		}

		return g_failures == 0;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}

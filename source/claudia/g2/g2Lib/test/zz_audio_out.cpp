// zz_audio_out.cpp -- audio render test for the Nord Modular G2 emulator.
#include "gatedFixture.h"
#include "../board.h"
#include "../internalClient.h"
#include "../memoryMap.h"
#include "../model.h"
#include "../panelSram.h"
#include "../scheduler.h"
#include "../uart0.h"
#include "../../g2JucePlugin/g2PatchLoad.h"
#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/peripherals56311.h"
#include "dsp56kBase/logging.h"
#include "baseLib/logging.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
	constexpr uint32_t g_entryPc = 0x30000400u, g_entrySp = 0x30400000u;
	constexpr int g_regPc = 17, g_regVbr = 18;
	constexpr uint32_t g_vectorTableBase = 0x30000000u, g_vectorTableEntries = 256u, g_vectorHandler = 0x300585CEu;
	constexpr uint32_t g_mbarBase = 0x10000000u, g_cs0Base = 0x00000000u, g_cs0Size = 0x00020000u, g_cs1Size = 0x00010000u;
	constexpr uint32_t g_cs2Base = 0x12000000u, g_cs2Size = 0x00800000u, g_cs3Size = 0x00010000u;
	constexpr uint32_t g_cs4Base = 0x14000000u, g_cs4Size = 0x00010000u, g_cs5Size = 0x00000010u, g_sdramSize = 0x00800000u;
	constexpr uint32_t g_displayBase = 0x302A0DB8u, g_lineWidth = 16u, g_bootQuantumBound = 500000u, g_bannerSettleQuanta = 20000u;
	constexpr uint32_t g_sinkControlWord = 0x2B6D51u, g_esaiTransmitters = 6u;
	constexpr int32_t g_sinkControlExpected = int32_t(g_sinkControlWord);
	constexpr unsigned g_sinkControlQuanta = 256u;
	constexpr dsp56k::TWord g_dmaTxChannel = 4u;
	constexpr double g_pi = 3.14159265358979323846;

	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const size_t _size) : m_bytes(_size, 0u) {}
		uint32_t read(const uint32_t _offset, const int _size, mcf5407_bus_status& _status) override
		{
			_status = (_size == 8 || _size == 16 || _size == 32) ? MCF5407_BUS_OK : MCF5407_BUS_SIZE_ILLEGAL;
			if(_status != MCF5407_BUS_OK) return 0u;
			uint32_t val = 0u, count = uint32_t(_size) / 8u;
			for(uint32_t i = 0; i < count; ++i)
				if(size_t(_offset) + i < m_bytes.size()) val = (val << 8) | m_bytes[size_t(_offset) + i];
			return val;
		}
		void write(const uint32_t _offset, const int _size, const uint32_t _val, mcf5407_bus_status& _status) override
		{
			_status = (_size == 8 || _size == 16 || _size == 32) ? MCF5407_BUS_OK : MCF5407_BUS_SIZE_ILLEGAL;
			if(_status != MCF5407_BUS_OK) return;
			const uint32_t count = uint32_t(_size) / 8u;
			for(uint32_t i = 0; i < count; ++i) {
				const size_t idx = size_t(_offset) + i;
				if(idx >= m_bytes.size()) continue;
				const uint8_t byte = uint8_t((_val >> (8u * (count - 1u - i))) & 0xffu);
				if(m_watchLength && idx >= m_watchBase && idx < m_watchBase + m_watchLength && byte != 0x20u && byte != 0x00u)
					++m_contentWrites;
				m_bytes[idx] = byte;
			}
		}
		bool place(const uint32_t _offset, const std::vector<uint8_t>& _img) {
			if(size_t(_offset) + _img.size() > m_bytes.size()) return false;
			std::memcpy(m_bytes.data() + _offset, _img.data(), _img.size());
			return true;
		}
		void watchCells(const uint32_t _offset, const uint32_t _length) { m_watchBase = _offset; m_watchLength = _length; }
		uint64_t contentWrites() const { return m_contentWrites; }
	private:
		std::vector<uint8_t> m_bytes;
		uint32_t m_watchBase = 0, m_watchLength = 0;
		uint64_t m_contentWrites = 0;
	};

	std::vector<uint8_t> readFile(const std::string& _path) {
		std::ifstream in(_path, std::ios::binary);
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	g2::BoardConfig makeConfig(const bool _stretchCs4, const g2::Model _model) {
		g2::BoardConfig c; c.model = _model;
		c.memory.cs0 = {g_cs0Base, g_cs0Size}; c.memory.cs1 = {g2::g_cs1Base, g_cs1Size};
		c.memory.cs2 = {g_cs2Base, g_cs2Size}; c.memory.cs3 = {g2::g_cs3Base, g_cs3Size};
		c.memory.cs4 = _stretchCs4 ? g2::g_panelSramCs4Window : g2::Window{g_cs4Base, g_cs4Size};
		c.memory.cs5 = {g2::g_cs5Base, g_cs5Size}; c.memory.mbar = {g_mbarBase, g2::g_simSpaceSize};
		c.memory.sdram = {g2::g_sdramBase, g_sdramSize}; c.adc = g2::panelAdcConfig();
		return c;
	}

	unsigned portOfChainPosition(g2::Board& _board, const unsigned _wanted, const unsigned _count) {
		for(unsigned pos = 0; pos < _count; ++pos) {
			mcf5407_bus_status status = MCF5407_BUS_OK;
			const uint32_t entry = g2::Board::onRead(&_board, 0x30116970u + pos * 4u, 4, &status);
			const uint8_t low = uint8_t(~uint8_t((entry >> 3) & 0xffu));
			if(low == 0u || (low & uint8_t(low - 1u)) != 0u) continue;
			unsigned port = 0; for(uint8_t bit = low; bit > 1u; bit >>= 1) ++port;
			if(pos == _wanted && port < _count) return port;
		}
		return _count;
	}

	struct AudioReading {
		uint64_t framesRequested = 0, framesReturned = 0, nonZeroFrames = 0;
		int firstNonZero = -1; int32_t minSample = 0, maxSample = 0;
		std::vector<int32_t> left;
	};

	void observe(AudioReading& _r, const g2::Frame& _f, const unsigned _q) {
		bool any = false; _r.left.push_back(_f.slot[0]);
		for(unsigned s = 0; s < g2::Frame::kSlots; ++s) {
			const int32_t v = _f.slot[s]; if(v != 0) any = true;
			if(_r.framesReturned == 1 && s == 0) { _r.minSample = _r.maxSample = v; }
			else { _r.minSample = std::min(_r.minSample, v); _r.maxSample = std::max(_r.maxSample, v); }
		}
		if(any) { ++_r.nonZeroFrames; if(_r.firstNonZero < 0) _r.firstNonZero = int(_q); }
	}

	void fft(std::vector<double>& _re, std::vector<double>& _im) {
		const size_t n = _re.size();
		for(size_t i = 1, j = 0; i < n; ++i) {
			size_t bit = n >> 1;
			for(; (j & bit) != 0; bit >>= 1) j ^= bit;
			j ^= bit;
			if(i < j) { std::swap(_re[i], _re[j]); std::swap(_im[i], _im[j]); }
		}
		for(size_t len = 2; len <= n; len <<= 1) {
			const size_t half = len / 2;
			for(size_t i = 0; i < n; i += len) {
				for(size_t k = 0; k < half; ++k) {
					const double angle = -2.0 * g_pi * double(k) / double(len);
					const double wr = std::cos(angle), wi = std::sin(angle);
					const double vr = _re[i + k + half] * wr - _im[i + k + half] * wi;
					const double vi = _re[i + k + half] * wi + _im[i + k + half] * wr;
					_re[i + k + half] = _re[i + k] - vr; _im[i + k + half] = _im[i + k] - vi;
					_re[i + k] += vr; _im[i + k] += vi;
				}
			}
		}
	}

	struct Partial { double hz = 0.0, magnitude = 0.0; };

	std::vector<Partial> resolvePartials(const std::vector<double>& _mag, const double _binHz) {
		std::vector<Partial> peaks;
		for(size_t k = 1; k + 1 < _mag.size(); ++k) {
			if(_mag[k] <= _mag[k - 1] || _mag[k] < _mag[k + 1]) continue;
			const double a = std::log(_mag[k - 1] + 1.0e-300), b = std::log(_mag[k] + 1.0e-300), c = std::log(_mag[k + 1] + 1.0e-300);
			const double curvature = a - 2.0 * b + c, delta = curvature < 0.0 ? 0.5 * (a - c) / curvature : 0.0;
			peaks.push_back({(double(k) + delta) * _binHz, _mag[k]});
		}
		std::sort(peaks.begin(), peaks.end(), [](const Partial& a, const Partial& b) { return a.magnitude > b.magnitude; });
		if(!peaks.empty()) {
			const double floor = peaks.front().magnitude * 1.0e-4;
			peaks.erase(std::remove_if(peaks.begin(), peaks.end(), [floor](const Partial& p) { return p.magnitude < floor; }), peaks.end());
		}
		if(peaks.size() > 32) peaks.resize(32);
		return peaks;
	}

	struct PitchEstimate { double hz = 0.0, clarity = 0.0, lag = 0.0; };

	PitchEstimate nsdfFundamental(const std::vector<double>& _c) {
		PitchEstimate est; const size_t n = _c.size(), longestLag = n / 2;
		if(n < 8) return est;
		std::vector<double> nsdf(longestLag + 2, 0.0);
		for(size_t lag = 1; lag <= longestLag; ++lag) {
			double prod = 0.0, energy = 0.0;
			for(size_t i = 0; i < n - lag; ++i) { prod += _c[i] * _c[i + lag]; energy += _c[i] * _c[i] + _c[i + lag] * _c[i + lag]; }
			nsdf[lag] = energy > 0.0 ? 2.0 * prod / energy : 0.0;
		}
		double largest = 0.0;
		for(size_t lag = 2; lag + 1 <= longestLag; ++lag)
			if(nsdf[lag] > nsdf[lag - 1] && nsdf[lag] >= nsdf[lag + 1]) largest = std::max(largest, nsdf[lag]);
		if(largest <= 0.0) return est;
		for(size_t lag = 2; lag + 1 <= longestLag; ++lag) {
			if(nsdf[lag] <= nsdf[lag - 1] || nsdf[lag] < nsdf[lag + 1] || nsdf[lag] < 0.9 * largest) continue;
			const double cur = nsdf[lag - 1] - 2.0 * nsdf[lag] + nsdf[lag + 1];
			est.lag = double(lag) + (cur < 0.0 ? 0.5 * (nsdf[lag - 1] - nsdf[lag + 1]) / cur : 0.0);
			est.clarity = nsdf[lag]; est.hz = double(G2_FRAME_RATE_HZ) / est.lag;
			return est;
		}
		return est;
	}

	double refineHarmonicHz(const std::vector<Partial>& _p, const double _hz, const double _binHz, const double _nyq) {
		if(_p.empty() || _hz <= 0.0) return _hz;
		const unsigned slots = std::min(64u, unsigned(_nyq / _hz));
		std::vector<Partial> slot(slots); std::vector<char> used(_p.size(), 0);
		for(unsigned k = 1; k <= slots; ++k) {
			const double target = _hz * double(k), tol = 3.0 * _binHz + 0.005 * target;
			size_t pick = _p.size();
			for(size_t i = 0; i < _p.size(); ++i) {
				if(used[i] || std::fabs(_p[i].hz - target) > tol) continue;
				if(pick == _p.size() || _p[i].magnitude > _p[pick].magnitude) pick = i;
			}
			if(pick != _p.size()) { used[pick] = 1; slot[k - 1] = _p[pick]; }
		}
		double num = 0.0, den = 0.0;
		for(size_t i = 0; i < slot.size(); ++i) {
			const double k = double(i + 1), w = slot[i].magnitude;
			num += w * k * slot[i].hz; den += w * k * k;
		}
		return den > 0.0 ? num / den : _hz;
	}

	void reportAudio(const char* _label, const AudioReading& _r) {
		std::cout << _label << ": framesRequested=" << _r.framesRequested << " framesReturned=" << _r.framesReturned
		          << " nonZeroFrames=" << _r.nonZeroFrames << " firstNonZeroQuantum=" << _r.firstNonZero
		          << " range=[" << _r.minSample << ", " << _r.maxSample << "]" << std::endl;
		const size_t n = _r.left.size();
		if(n == 0) return;
		double mean = 0.0; for(const int32_t v : _r.left) mean += double(v);
		mean /= double(n);
		std::vector<double> centred(n); double peak = 0.0, sumSquares = 0.0;
		for(size_t i = 0; i < n; ++i) {
			centred[i] = double(_r.left[i]) - mean;
			peak = std::max(peak, std::fabs(centred[i]));
			sumSquares += centred[i] * centred[i];
		}
		std::cout << _label << ": level slot0 mean=" << mean << " peakLsb=" << peak
		          << " rmsLsb=" << std::sqrt(sumSquares / double(n)) << " samples=" << n << std::endl;
		size_t fftSize = 1; while(fftSize < 4 * n) fftSize <<= 1;
		std::vector<double> re(fftSize, 0.0), im(fftSize, 0.0);
		for(size_t i = 0; i < n; ++i) {
			const double phase = n > 1 ? 2.0 * g_pi * double(i) / double(n - 1) : 0.0;
			const double window = n > 1 ? 0.35875 - 0.48829 * std::cos(phase) + 0.14128 * std::cos(2.0 * phase)
			                              - 0.01168 * std::cos(3.0 * phase) : 1.0;
			re[i] = centred[i] * window;
		}
		fft(re, im);
		std::vector<double> mag(fftSize / 2);
		for(size_t k = 0; k < mag.size(); ++k) mag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]);
		const double binHz = double(G2_FRAME_RATE_HZ) / double(fftSize);
		const auto partials = resolvePartials(mag, binHz);
		const auto pitch = nsdfFundamental(centred);
		const double refinedHz = refineHarmonicHz(partials, pitch.hz, binHz, 0.5 * double(G2_FRAME_RATE_HZ));
		std::cout << _label << ": tone slot0 fundamentalHz=" << refinedHz << " lagHz=" << pitch.hz
		          << " periodSamples=" << pitch.lag << " clarity=" << pitch.clarity
		          << " partials=" << partials.size() << std::endl;
	}
}

int main()
{
	g2::EnvArtifactResolver resolver;
	g2::test::GatedCounters counters;

	g2::test::runGated(resolver, std::cout, counters, [&]() -> bool
	{
		std::string why;
		const std::string dir = resolver.resolve(why);
		if(dir.empty()) { std::cout << "FAIL " << why << std::endl; return false; }

		const char* const modeEnv = std::getenv("G2_AUDIO_MODE");
		const std::string mode = modeEnv ? modeEnv : "framed";
		g2::Model model = g2::Model::G2X;
		if(const char* const m = std::getenv("G2_AUDIO_MODEL")) g2::modelFromName(m, model);
		const uint32_t windowQuanta = std::getenv("G2_AUDIO_QUANTA") ? uint32_t(std::strtoul(std::getenv("G2_AUDIO_QUANTA"), nullptr, 10)) : 500001u;
		const uint32_t walkQuanta = std::getenv("G2_AUDIO_WALK") ? uint32_t(std::strtoul(std::getenv("G2_AUDIO_WALK"), nullptr, 10)) : 8192u;
		const bool sendNote = std::getenv("G2_AUDIO_NOTE") != nullptr;
		const uint32_t noteQuanta = std::getenv("G2_AUDIO_NOTEQUANTA") ? uint32_t(std::strtoul(std::getenv("G2_AUDIO_NOTEQUANTA"), nullptr, 10)) : 40000u;
		const double deadline = std::getenv("G2_AUDIO_DEADLINE") ? std::strtod(std::getenv("G2_AUDIO_DEADLINE"), nullptr) : 3600.0;
		std::string patchPath = std::getenv("G2_AUDIO_PATCH") ? std::getenv("G2_AUDIO_PATCH") : "ChOrgan demo", patchName = patchPath;
		if(const auto slash = patchName.find_last_of('/'); slash != std::string::npos) patchName = patchName.substr(slash + 1);

		const auto code = readFile(dir + "/CODE_30000400.bin"), patch = readFile(dir + "/corpus/pch2/" + patchPath + ".pch2");
		if(code.empty() || (mode != "none" && patch.empty())) {
			std::cout << "FAIL artifacts missing under " << dir << std::endl; return false;
		}

		g2::Board board(makeConfig(true, model));
		Ram ram(g_sdramSize);
		g2::PanelSram cs4(board.memory());
		const float vol = std::getenv("G2_AUDIO_VOLUME") ? float(std::atof(std::getenv("G2_AUDIO_VOLUME"))) : 1.0f;
		board.adc().setChannelVolts(uint8_t(g2::PanelControl::MasterVolume), vol * makeConfig(true, model).adc.externalReferenceVolts);

		if(!cs4.place(g2::g_panelSramImageBase, readFile(dir + "/" + g2::g_panelSramImageName))) return false;
		board.memory().attach(g2::Region::Cs4, &cs4);
		if(!ram.place(g_entryPc - g2::g_sdramBase, code)) return false;

		std::vector<uint8_t> table(g_vectorTableEntries * 4u);
		for(uint32_t e = 0; e < g_vectorTableEntries; ++e)
			for(uint32_t b = 0; b < 4u; ++b) table[e * 4u + b] = uint8_t((g_vectorHandler >> ((3u - b) * 8u)) & 0xffu);
		if(!ram.place(g_vectorTableBase - g2::g_sdramBase, table)) return false;

		board.memory().attach(g2::Region::Sdram, &ram);
		ram.watchCells(g_displayBase - g2::g_sdramBase, g_lineWidth);
		board.resetMcu(g_entrySp, g_entryPc);
		board.setMcuReg(g_regVbr, g_vectorTableBase);

		g2::SerialExecutor executor; g2::Status st{}; const g2::Scheduler::Config cfg;
		const auto scheduler = g2::Scheduler::create(cfg, executor, board, st);
		if(!scheduler) return false;

		const unsigned dspCount = board.dspSet().dspCount();
		const auto start = std::chrono::steady_clock::now();
		const auto expired = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > deadline; };

		uint32_t settle = 0;
		for(uint32_t i = 0; i < g_bootQuantumBound; ++i) {
			scheduler->runFrames(1);
			if(board.mcuHalted() || expired()) break;
			if(ram.contentWrites() == 0) continue;
			if(++settle < g_bannerSettleQuanta) continue;
			unsigned landed = 0;
			for(unsigned d = 0; d < dspCount; ++d)
				if(board.dspSet().programLanded(d) && *board.dspSet().programLanded(d)) ++landed;
			if(landed == dspCount) break;
		}

		std::vector<uint8_t> scratch(g2::g_maxPatchLoadMessageBytes + 4);
		std::optional<g2::InternalClient> clientStorage;
		clientStorage.emplace(board.transport(), scratch.size(), 4);
		if(mode == "framed") {
			g2::pch2LoadFramed(patch.data(), patch.size(), patchName.c_str(), 0, *clientStorage, scratch.data(), scratch.size());
			board.pumpTransport();
			scheduler->runFrames(1);
		}

		for(uint32_t i = 0; i < windowQuanta; ++i) {
			scheduler->runFrames(1);
			if(board.mcuHalted() || expired()) break;
		}

		if(sendNote) {
			for(uint8_t b : {0x90u, 0x3Cu, 0x64u}) {
				board.uart0().receive(b);
				if((board.uart0().usr() & 1u) == 0) continue;
				for(uint32_t i = 0; i < 20000u; ++i)
					if((board.uart0().usr() & 1u) == 0 || board.mcuHalted()) break;
					else scheduler->runFrames(1);
			}
			for(uint32_t i = 0; i < noteQuanta; ++i) {
				scheduler->runFrames(1);
				if(board.mcuHalted() || expired()) break;
			}
		}

		scheduler->beginPlayPhase();
		std::vector<g2::Frame> primed(cfg.lookaheadFrames);
		scheduler->pull(primed.data(), primed.size());

		AudioReading walkRead;
		const g2::Frame silence{};
		for(uint32_t q = 0; q < walkQuanta; ++q) {
			scheduler->push(&silence, 1);
			scheduler->runFrames(1);
			g2::Frame out{};
			++walkRead.framesRequested;
			if(scheduler->pull(&out, 1) != 0) {
				++walkRead.framesReturned;
				observe(walkRead, out, q);
			}
		}
		reportAudio("WALK", walkRead);

		const unsigned tailPort = portOfChainPosition(board, dspCount - 1u, dspCount);
		if(tailPort < dspCount) {
			auto& p = board.dspSet().peripherals(tailPort);
			auto& esai = p.getEsai();
			int arrival = -1; int32_t gotL = 0, gotR = 0; bool exact = false;
			for(unsigned q = 0; q < g_sinkControlQuanta && arrival < 0; ++q) {
				const auto en = esai.hasEnabledTransmitters();
				for(uint32_t reg = 0; reg < g_esaiTransmitters; ++reg)
					if(en & (1u << reg)) esai.writeTX(reg, g_sinkControlWord);
				const auto src = p.getDMA().getDSR(g_dmaTxChannel);
				const auto fw = esai.getTxWordCount() + 1u;
				const auto base = src - (src % fw);
				auto& mem = board.dspSet().dsp(tailPort).memory();
				for(dsp56k::TWord i = 0; i < fw * 2u; ++i) mem.set(dsp56k::MemArea_X, base + i, g_sinkControlWord);
				scheduler->push(&silence, 1);
				scheduler->runFrames(1);
				g2::Frame out{};
				if(scheduler->pull(&out, 1) == 0 || (out.slot[0] == 0 && out.slot[1] == 0)) continue;
				arrival = int(q); gotL = out.slot[0]; gotR = out.slot[1];
				exact = (gotL == g_sinkControlExpected && gotR == g_sinkControlExpected);
			}
			std::cout << "known positive: tailPortFound=1 arrival=" << arrival << " exact=" << (exact ? 1 : 0)
			          << " slot0=" << gotL << " slot1=" << gotR << " expected=" << g_sinkControlExpected << std::endl;
		}

		std::cout << g2::test::g_verdictNotVerified << " diagnostic instrument; no assertion is made here" << std::endl;
		return true;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;
	return g2::test::gatedExitCode(counters);
}

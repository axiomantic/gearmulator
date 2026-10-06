/* t1_audio_output_lean.cpp -- Integration test verifying that a booted G2 engine
 * with a loaded patch produces non-zero audio output when stimulated with a MIDI note.
 *
 * Tier T1, gated: boots Clavia firmware and requires the artifact corpus, skipping
 * cleanly when NMG2_ARTIFACTS is unset or artifacts are absent.
 */

#include "../../rg2JucePlugin/rg2PatchLoad.h"
#include "../board.h"
#include "../internalClient.h"
#include "../memoryMap.h"
#include "../model.h"
#include "../panelSram.h"
#include "../scheduler.h"
#include "../uart0.h"
#include "baseLib/logging.h"
#include "dsp56kBase/logging.h"
#include "gatedFixture.h"

#include <algorithm>
#include <atomic>
#include <cmath>
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
    int g_failures = 0, g_cases = 0;

    void check(const bool _cond, const std::string& _what)
    {
        ++g_cases;
        std::cout << (_cond ? "ok   " : "FAIL ") << _what << std::endl;
        if (!_cond)
            ++g_failures;
    }

    const char* const g_underrunMessage = "ESAI transmit underrun";
    constexpr uint64_t g_underrunLinesKept = 4;
    std::atomic<uint64_t> g_underrunLines{0};

    void filterLog(const std::string& _s)
    {
        if (_s.find(g_underrunMessage) != std::string::npos && g_underrunLines.fetch_add(1) >= g_underrunLinesKept)
            return;
        std::cout << _s << '\n';
    }

    void installLogFilter()
    {
        if (!std::getenv("G2_LOG_ESAI_UNDERRUN"))
            Logging::setLogFunc(&filterLog);
    }

    constexpr uint32_t g_entryPc = 0x30000400u, g_entrySp = 0x30400000u, g_regVbr = 18;
    constexpr uint32_t g_vectorTableBase = 0x30000000u, g_vectorTableEntries = 256u, g_vectorHandler = 0x300585CEu;
    constexpr uint32_t g_mbarBase = 0x10000000u, g_cs0Base = 0x00000000u, g_cs0Size = 0x00020000u,
                       g_cs1Size = 0x00010000u;
    constexpr uint32_t g_cs2Base = 0x12000000u, g_cs2Size = 0x00800000u, g_cs3Size = 0x00010000u;
    constexpr uint32_t g_cs4Base = 0x14000000u, g_cs4Size = 0x00010000u, g_cs5Size = 0x00000010u,
                       g_sdramSize = 0x00800000u;
    constexpr uint32_t g_bootQuantaDefault = 500000u;

    class Ram final : public rg2::BusTarget
    {
    public:
        explicit Ram(const size_t _size) : m_bytes(_size, 0u) {}
        uint32_t read(const uint32_t _o, const int _s, cf_bus_status& _st) override
        {
            _st = (_s == 8 || _s == 16 || _s == 32) ? CF_BUS_OK : CF_BUS_SIZE_ILLEGAL;
            if (_st != CF_BUS_OK)
                return 0u;
            uint32_t val = 0u, count = uint32_t(_s) / 8u;
            for (uint32_t i = 0; i < count; ++i)
                if (size_t(_o) + i < m_bytes.size())
                    val = (val << 8) | m_bytes[size_t(_o) + i];
            return val;
        }
        void write(const uint32_t _o, const int _s, const uint32_t _val, cf_bus_status& _st) override
        {
            _st = (_s == 8 || _s == 16 || _s == 32) ? CF_BUS_OK : CF_BUS_SIZE_ILLEGAL;
            if (_st != CF_BUS_OK)
                return;
            ++m_writes;
            const uint32_t count = uint32_t(_s) / 8u;
            for (uint32_t i = 0; i < count; ++i)
            {
                const size_t idx = size_t(_o) + i;
                if (idx < m_bytes.size())
                    m_bytes[idx] = uint8_t((_val >> (8u * (count - 1u - i))) & 0xffu);
            }
        }
        bool place(const uint32_t _o, const std::vector<uint8_t>& _img)
        {
            if (size_t(_o) + _img.size() > m_bytes.size())
                return false;
            std::memcpy(m_bytes.data() + _o, _img.data(), _img.size());
            return true;
        }
        uint64_t writes() const { return m_writes; }

    private:
        std::vector<uint8_t> m_bytes;
        uint64_t m_writes = 0;
    };

    std::vector<uint8_t> readFile(const std::string& _path)
    {
        std::ifstream in(_path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    rg2::BoardConfig makeConfig(const rg2::Model _model)
    {
        rg2::BoardConfig c;
        c.model = _model;
        c.memory.cs0 = {g_cs0Base, g_cs0Size};
        c.memory.cs1 = {rg2::g_cs1Base, g_cs1Size};
        c.memory.cs2 = {g_cs2Base, g_cs2Size};
        c.memory.cs3 = {rg2::g_cs3Base, g_cs3Size};
        c.memory.cs4 = rg2::g_panelSramCs4Window;
        c.memory.cs5 = {rg2::g_cs5Base, g_cs5Size};
        c.memory.mbar = {g_mbarBase, rg2::g_simSpaceSize};
        c.memory.sdram = {rg2::g_sdramBase, g_sdramSize};
        c.adc = rg2::panelAdcConfig();
        return c;
    }

    struct AudioReading
    {
        uint64_t requested = 0, returned = 0, nonZero = 0;
        int32_t minVal = 0, maxVal = 0, peakAbs = 0;
        void observe(const rg2::Frame& _f)
        {
            bool any = false;
            for (unsigned s = 0; s < rg2::Frame::kSlots; ++s)
            {
                const int32_t v = _f.slot[s];
                if (v != 0)
                    any = true;
                if (returned == 1 && s == 0)
                {
                    minVal = maxVal = v;
                    peakAbs = std::abs(v);
                }
                else
                {
                    minVal = std::min(minVal, v);
                    maxVal = std::max(maxVal, v);
                    peakAbs = std::max(peakAbs, std::abs(v));
                }
            }
            if (any)
                ++nonZero;
        }
    };

    void runControls()
    {
        std::cout << "-- controls (ungated: verify measurement evaluation)" << std::endl;
        AudioReading silence;
        rg2::Frame silenceFrame{};
        for (unsigned i = 0; i < 64; ++i)
        {
            ++silence.requested;
            ++silence.returned;
            silence.observe(silenceFrame);
        }
        check(silence.nonZero == 0, "CONTROL silence produces 0 non-zero frames");
        check(silence.minVal == 0 && silence.maxVal == 0, "CONTROL silence reports flat [0, 0] range");

        AudioReading tone;
        for (unsigned i = 0; i < 64; ++i)
        {
            rg2::Frame toneFrame{};
            toneFrame.slot[0] = (i % 2 == 0) ? 1000 : -1000;
            ++tone.requested;
            ++tone.returned;
            tone.observe(toneFrame);
        }
        check(tone.nonZero == 64, "CONTROL synthesized tone registers all frames as non-zero");
        check(tone.peakAbs == 1000, "CONTROL synthesized tone detects expected peak magnitude");
    }
} // namespace

int main()
{
    installLogFilter();
    runControls();

    rg2::EnvArtifactResolver resolver;
    rg2::test::GatedCounters counters;

    rg2::test::runGated(
        resolver, std::cout, counters,
        [&]() -> bool
        {
            const int failuresBefore = g_failures;

            std::string why;
            const std::string dir = resolver.resolve(why);
            if (dir.empty())
            {
                std::cout << "FAIL " << why << std::endl;
                return false;
            }

            const char* const p = std::getenv("G2_AUDIO_PATCH");
            const std::string patchName = p ? p : "ChOrgan demo";
            const std::string patchFile = dir + "/corpus/pch2/" + patchName + ".pch2";

            const auto code = readFile(dir + "/CODE_30000400.bin"), patch = readFile(patchFile),
                       sram = readFile(dir + "/" + rg2::g_panelSramImageName);
            if (code.empty() || patch.empty() || sram.empty())
            {
                std::cout << "FAIL required artifacts missing under " << dir << std::endl;
                return false;
            }

            rg2::Model model = rg2::Model::Engine;
            if (const char* const m = std::getenv("G2_AUDIO_MODEL"))
                rg2::modelFromName(m, model);

            const uint32_t bootQuanta = std::getenv("G2_AUDIO_BOOTQUANTA")
                ? uint32_t(std::strtoul(std::getenv("G2_AUDIO_BOOTQUANTA"), nullptr, 10))
                : g_bootQuantaDefault;
            const uint32_t windowQuanta = std::getenv("G2_AUDIO_QUANTA")
                ? uint32_t(std::strtoul(std::getenv("G2_AUDIO_QUANTA"), nullptr, 10))
                : 50000u;
            const uint32_t noteQuanta = std::getenv("G2_AUDIO_NOTEQUANTA")
                ? uint32_t(std::strtoul(std::getenv("G2_AUDIO_NOTEQUANTA"), nullptr, 10))
                : 20000u;
            const uint32_t walkQuanta = std::getenv("G2_AUDIO_WALK")
                ? uint32_t(std::strtoul(std::getenv("G2_AUDIO_WALK"), nullptr, 10))
                : 1024u;

            rg2::Board board(makeConfig(model));
            Ram ram(g_sdramSize);
            rg2::PanelSram cs4(board.memory());

            board.adc().setChannelVolts(uint8_t(rg2::PanelControl::MasterVolume),
                                        1.0f * makeConfig(model).adc.externalReferenceVolts);
            check(cs4.place(rg2::g_panelSramImageBase, sram), "panel SRAM image placed in CS4");
            board.memory().attach(rg2::Region::Cs4, &cs4);

            check(ram.place(g_entryPc - rg2::g_sdramBase, code), "firmware image placed in SDRAM");

            std::vector<uint8_t> table(g_vectorTableEntries * 4u);
            for (uint32_t e = 0; e < g_vectorTableEntries; ++e)
                for (uint32_t b = 0; b < 4u; ++b)
                    table[e * 4u + b] = uint8_t((g_vectorHandler >> ((3u - b) * 8u)) & 0xffu);

            check(ram.place(g_vectorTableBase - rg2::g_sdramBase, table), "vector table placed in SDRAM");

            board.memory().attach(rg2::Region::Sdram, &ram);

            board.resetMcu(g_entrySp, g_entryPc);
            board.setMcuReg(g_regVbr, g_vectorTableBase);

            rg2::SerialExecutor executor;
            rg2::Status st{};
            const rg2::Scheduler::Config cfg;
            const auto scheduler = rg2::Scheduler::create(cfg, executor, board, st);

            check(scheduler != nullptr, "Scheduler instance created successfully");
            if (!scheduler)
                return false;

            for (uint32_t i = 0; i < bootQuanta && !board.mcuHalted(); ++i)
                scheduler->runFrames(1);

            check(!board.mcuHalted() && ram.writes() > 0,
                  "MCU boot completed and SDRAM active (" + std::to_string(ram.writes()) + " writes across " +
                      std::to_string(bootQuanta) + " quanta)");

            std::vector<uint8_t> scratch(rg2::g_maxPatchLoadMessageBytes + 4);
            rg2::InternalClient client(board.transport(), scratch.size(), 4);
            const auto loadResult = rg2::pch2LoadFramed(patch.data(), patch.size(), patchName.c_str(), 0, client,
                                                       scratch.data(), scratch.size());

            check(loadResult == rg2::Pch2LoadResult::Loaded,
                  std::string("patch-load framed message accepted: ") + rg2::pch2LoadResultName(loadResult));

            board.pumpTransport();
            scheduler->runFrames(1);

            for (uint32_t i = 0; i < windowQuanta && !board.mcuHalted(); ++i)
                scheduler->runFrames(1);
            check(!board.mcuHalted(), "MCU remained running through patch compilation window");

            for (uint8_t b : {0x90u, 0x3Cu, 0x64u})
            {
                board.uart0().receive(b);
                if ((board.uart0().usr() & 1u) == 0)
                    continue;
                for (uint32_t i = 0; i < 20000u && (board.uart0().usr() & 1u) != 0 && !board.mcuHalted(); ++i)
                    scheduler->runFrames(1);
            }

            for (uint32_t i = 0; i < noteQuanta && !board.mcuHalted(); ++i)
                scheduler->runFrames(1);
            check(!board.mcuHalted(), "MCU remained running through note-on window");

            scheduler->beginPlayPhase();
            std::vector<rg2::Frame> primed(cfg.lookaheadFrames);
            scheduler->pull(primed.data(), primed.size());

            AudioReading reading;
            const rg2::Frame silence{};
            for (uint32_t q = 0; q < walkQuanta; ++q)
            {
                scheduler->push(&silence, 1);
                scheduler->runFrames(1);
                rg2::Frame out{};
                ++reading.requested;
                if (scheduler->pull(&out, 1) != 0)
                {
                    ++reading.returned;
                    reading.observe(out);
                }
            }

            std::cout << "audio measurement: requested=" << reading.requested << " returned=" << reading.returned
                      << " nonZero=" << reading.nonZero << " peakAbs=" << reading.peakAbs << " range=["
                      << reading.minVal << ", " << reading.maxVal << "]" << std::endl;

            check(reading.returned == reading.requested,
                  "all requested audio frames were returned by scheduler (" + std::to_string(reading.returned) +
                      " of " + std::to_string(reading.requested) + ")");

            check(reading.nonZero > 0,
                  "synthesizer produced non-zero audio frames: " + std::to_string(reading.nonZero) + " of " +
                      std::to_string(reading.returned));

            check(reading.peakAbs > 0,
                  "audio output peak magnitude is non-zero: peakAbs=" + std::to_string(reading.peakAbs));

            std::cout << "t1_audio_output_lean: " << g_failures << " failure(s) in " << g_cases << " case(s)"
                      << std::endl;

            return g_failures == failuresBefore;
        });

    std::cout << rg2::test::summaryLine(counters) << std::endl;
    return (counters.run == 0 && g_failures > 0) ? 1 : rg2::test::gatedExitCode(counters);
}

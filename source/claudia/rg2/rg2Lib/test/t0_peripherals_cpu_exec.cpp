// End-to-end ColdFire CPU execution tests for MCF5407 on-chip peripherals.
//
// What this test proves:
//
//   1. ColdFire CPU instruction execution recognizes the internal peripheral
//      memory space at MBAR (0x10000000..0x10000300). Instructions write and
//      read back timer and interrupt controller registers without bus faults.
//
//   2. Timer 1 compare match asserts the level 6 autovectored interrupt on
//      the interrupt controller and core when programmed with ORI and RST.
//
//   3. Interrupt priority masking in the status register inhibits interrupt
//      take when SR IPL >= 6. The core continues executing the main sequence,
//      the stack pointer remains untouched, and the ISR does not run.
//
//   4. Lowering SR IPL allows the core to sample and take the level 6 interrupt:
//      the CPU stacks the 8-byte ColdFire exception frame, fetches the handler
//      address from vector 30 (offset 0x78 from VBR), executes the ISR, clears
//      the timer event condition, and returns via RTE to the interrupted loop.

#include "../board.h"
#include "../interruptController.h"
#include "../memoryMap.h"
#include "../sim.h"
#include "../timer.h"

#include <mcf5407.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;
    int g_cases = 0;

    template <typename T> void checkEqual(const T& _actual, const T& _expected, const std::string& _what)
    {
        ++g_cases;
        if (_actual == _expected)
        {
            std::cout << "ok   " << _what << std::endl;
            return;
        }
        std::cout << "FAIL " << _what << ": expected <" << _expected << ">, got <" << _actual << ">" << std::endl;
        ++g_failures;
    }

    void check(const bool _condition, const std::string& _what)
    {
        ++g_cases;
        if (_condition)
        {
            std::cout << "ok   " << _what << std::endl;
            return;
        }
        std::cout << "FAIL " << _what << std::endl;
        ++g_failures;
    }

    // ---------------------------------------------------------------- RAM model
    //
    // A flat memory store attached as SDRAM. Provides big-endian 8, 16, and
    // 32-bit transfers matching the ColdFire bus interface.
    class TestRam final : public rg2::BusTarget
    {
    public:
        explicit TestRam(const size_t _size) : m_bytes(_size, 0) {}

        uint32_t read(const uint32_t _offset, const int _size, mcf5407_bus_status& _status) override
        {
            _status = MCF5407_BUS_OK;
            if (_size != 8 && _size != 16 && _size != 32)
            {
                _status = MCF5407_BUS_SIZE_ILLEGAL;
                return 0u;
            }

            const uint32_t count = uint32_t(_size) / 8u;
            uint32_t value = 0u;
            for (uint32_t i = 0; i < count; ++i)
            {
                value <<= 8;
                const size_t index = size_t(_offset) + i;
                if (index < m_bytes.size())
                    value |= m_bytes[index];
            }
            return value;
        }

        void write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5407_bus_status& _status) override
        {
            _status = MCF5407_BUS_OK;
            if (_size != 8 && _size != 16 && _size != 32)
            {
                _status = MCF5407_BUS_SIZE_ILLEGAL;
                return;
            }

            const uint32_t count = uint32_t(_size) / 8u;
            for (uint32_t i = 0; i < count; ++i)
            {
                const size_t index = size_t(_offset) + i;
                if (index >= m_bytes.size())
                    continue;
                const int shift = int(8u * (count - 1u - i));
                m_bytes[index] = uint8_t((_value >> shift) & 0xffu);
            }
        }

        uint8_t read8(const uint32_t _offset) const
        {
            if (_offset >= m_bytes.size())
                return 0u;
            return m_bytes[_offset];
        }

        uint16_t read16(const uint32_t _offset) const
        {
            if (_offset + 2 > m_bytes.size())
                return 0u;
            return uint16_t((uint16_t(m_bytes[_offset]) << 8) | uint16_t(m_bytes[_offset + 1]));
        }

        uint32_t read32(const uint32_t _offset) const
        {
            if (_offset + 4 > m_bytes.size())
                return 0u;
            return (uint32_t(m_bytes[_offset]) << 24) | (uint32_t(m_bytes[_offset + 1]) << 16) |
                (uint32_t(m_bytes[_offset + 2]) << 8) | uint32_t(m_bytes[_offset + 3]);
        }

        void write32(const uint32_t _offset, const uint32_t _val)
        {
            if (_offset + 4 <= m_bytes.size())
            {
                m_bytes[_offset] = uint8_t((_val >> 24) & 0xffu);
                m_bytes[_offset + 1] = uint8_t((_val >> 16) & 0xffu);
                m_bytes[_offset + 2] = uint8_t((_val >> 8) & 0xffu);
                m_bytes[_offset + 3] = uint8_t(_val & 0xffu);
            }
        }

        void load(const uint32_t _offset, const std::vector<uint8_t>& _data)
        {
            if (_offset + _data.size() <= m_bytes.size())
                std::memcpy(m_bytes.data() + _offset, _data.data(), _data.size());
        }

    private:
        std::vector<uint8_t> m_bytes;
    };

    constexpr uint32_t kMbarBase = 0x10000000u;
    constexpr uint32_t kMbarSize = 0x400u;

    constexpr uint32_t kRamBase = 0x00000000u;
    constexpr uint32_t kRamSize = 0x00010000u; // 64 KiB

    constexpr uint32_t kInitialSp = 0x00008000u;
    constexpr uint32_t kCodeEntry = 0x00000400u;
    constexpr uint32_t kIsrEntry = 0x00000600u;

    constexpr uint32_t kFlagAddr = 0x00001000u;
    constexpr uint32_t kTrrReadbackAddr = 0x00001004u;
    constexpr uint32_t kIcrReadbackAddr = 0x00001008u;

    constexpr uint8_t kTerRef = 0x02u;

    rg2::BoardConfig makeBoardConfig()
    {
        rg2::BoardConfig config;
        config.memory.mbar = {kMbarBase, kMbarSize};
        config.memory.sdram = {kRamBase, kRamSize};
        return config;
    }

    // --------------------------------------------------------- Assembly routines
    //
    // Binary opcode sequences assembled for ColdFire MCF5407 (ISA_A):
    //
    // Routine 1:
    //   moveq   #0, %d0
    //   movec   %d0, %vbr
    //   moveal  #0x10000000, %a0
    //   moveb   #0x9b, %d0            ; ICR1: AVEC=1, IL=6, IP=3
    //   moveb   %d0, (0x04d, %a0)
    //   movew   #5, %d0               ; TRR1: reference = 5
    //   movew   %d0, (0x144, %a0)
    //   movew   (0x144, %a0), %d1     ; read back TRR1 into D1
    //   moveal  #0x1004, %a1
    //   movew   %d1, (%a1)            ; store TRR1 readback to RAM
    //   moveb   (0x04d, %a0), %d2     ; read back ICR1 into D2
    //   moveal  #0x1008, %a1
    //   moveb   %d2, (%a1)            ; store ICR1 readback to RAM
    //   movew   #0x001b, %d0          ; TMR1: PS=0, CLK=1, FRR=1, ORI=1, RST=1
    //   movew   %d0, (0x140, %a0)
    // masked_loop:
    //   nop
    //   bras    masked_loop
    const std::vector<uint8_t> kCodeProg1 = {
        0x70, 0x00, 0x4e, 0x7b, 0x08, 0x01, 0x20, 0x7c, 0x10, 0x00, 0x00, 0x00, 0x10, 0x3c, 0x00, 0x9b,
        0x11, 0x40, 0x00, 0x4d, 0x30, 0x3c, 0x00, 0x05, 0x31, 0x40, 0x01, 0x44, 0x32, 0x28, 0x01, 0x44,
        0x22, 0x7c, 0x00, 0x00, 0x10, 0x04, 0x32, 0x81, 0x14, 0x28, 0x00, 0x4d, 0x22, 0x7c, 0x00, 0x00,
        0x10, 0x08, 0x12, 0x82, 0x30, 0x3c, 0x00, 0x1b, 0x31, 0x40, 0x01, 0x40, 0x4e, 0x71, 0x60, 0xfc};

    // Routine 2 (CPU self-unmasking sequence):
    //   moveq   #0, %d0
    //   movec   %d0, %vbr
    //   moveal  #0x10000000, %a0
    //   moveb   #0x9b, %d0            ; ICR1: Level 6, priority 3, autovector
    //   moveb   %d0, (0x04d, %a0)
    //   movew   #5, %d0               ; TRR1 = 5
    //   movew   %d0, (0x144, %a0)
    //   movew   #0x001b, %d0          ; TMR1 enable with ORI
    //   movew   %d0, (0x140, %a0)
    //   moveq   #30, %d2
    // mask_loop:
    //   subql   #1, %d2
    //   bnes    mask_loop
    //   movew   #0x2000, %sr          ; unmask IPL: Supervisor=1, IPL=0
    // idle_loop:
    //   nop
    //   bras    idle_loop
    const std::vector<uint8_t> kCodeProg2 = {
        0x70, 0x00, 0x4e, 0x7b, 0x08, 0x01, 0x20, 0x7c, 0x10, 0x00, 0x00, 0x00, 0x10, 0x3c, 0x00, 0x9b, 0x11,
        0x40, 0x00, 0x4d, 0x30, 0x3c, 0x00, 0x05, 0x31, 0x40, 0x01, 0x44, 0x30, 0x3c, 0x00, 0x1b, 0x31, 0x40,
        0x01, 0x40, 0x74, 0x1e, 0x53, 0x82, 0x66, 0xfc, 0x46, 0xfc, 0x20, 0x00, 0x4e, 0x71, 0x60, 0xfc};

    // ISR Routine:
    //   moveal  #0x1000, %a1
    //   addql   #1, (%a1)             ; increment isr hit counter
    //   moveal  #0x10000000, %a0
    //   moveb   #2, %d0
    //   moveb   %d0, (0x151, %a0)     ; clear TER1[REF]
    //   movew   #0, %d0
    //   movew   %d0, (0x140, %a0)     ; disable Timer 1 (RST=0)
    //   rte
    const std::vector<uint8_t> kCodeIsr = {0x22, 0x7c, 0x00, 0x00, 0x10, 0x00, 0x52, 0x91, 0x20, 0x7c, 0x10,
                                           0x00, 0x00, 0x00, 0x10, 0x3c, 0x00, 0x02, 0x11, 0x40, 0x01, 0x51,
                                           0x30, 0x3c, 0x00, 0x00, 0x31, 0x40, 0x01, 0x40, 0x4e, 0x73};
} // namespace

int main()
{
    // -----------------------------------------------------------------------
    // Case group 1. ColdFire CPU execution recognizes the internal peripheral
    // space (0x10000000..0x10000300).
    //
    // The CPU executes instructions that write and read back registers in
    // both the interrupt controller block (ICR1 at 0x1000004D) and the timer
    // block (TRR1 at 0x10000144). The CPU stores the readback values into
    // RAM and runs into a masked spin loop without faulting.
    {
        rg2::Board board(makeBoardConfig());
        TestRam ram(kRamSize);
        board.memory().attach(rg2::Region::Sdram, &ram);

        // Install vector table entries:
        // Vector 0 (SP): 0x8000, Vector 1 (PC): 0x400
        // Vector 30 (Level 6 autovector at offset 0x78): points to ISR at 0x600
        ram.write32(0x00, kInitialSp);
        ram.write32(0x04, kCodeEntry);
        ram.write32(0x78, kIsrEntry);

        ram.load(kCodeEntry, kCodeProg1);
        ram.load(kIsrEntry, kCodeIsr);

        board.resetMcu(kInitialSp, kCodeEntry);

        // Step enough cycles to run the configuration instructions and enter
        // the loop.
        const uint32_t cycles = board.runMcu(100);

        check(!board.faulted(), "CPU executes peripheral write and read instructions without bus fault");
        check(cycles > 0, "CPU reported non-zero executed cycles");
        checkEqual(uint32_t(ram.read16(kTrrReadbackAddr)), uint32_t(5),
                   "CPU reads back TRR1 value (5) from internal peripheral space 0x10000144");
        checkEqual(uint32_t(ram.read8(kIcrReadbackAddr)), uint32_t(0x9b),
                   "CPU reads back ICR1 value (0x9b) from internal peripheral space 0x1000004d");
        checkEqual(board.mcuReg(18), uint32_t(0), "VBR was set to 0 by movec %d0, %vbr");

        // -------------------------------------------------------------------
        // Case group 2. Timer compare match triggers interrupt autovector (level 6).
        //
        // Advancing the MCU advanced the timers by the cycles spent. Since
        // TRR1 = 5 with prescaler = 0, the counter reached 5, setting TER1[REF]
        // and asserting level 6 autovectored interrupt on the arbiter.
        mcf5407_bus_status busStatus = MCF5407_BUS_OK;
        const uint32_t ter1 = board.sim().read(0x151, 8, busStatus);
        checkEqual(ter1 & kTerRef, uint32_t(kTerRef), "Timer 1 compare match set TER1[REF] event flag");
        checkEqual(board.interrupts().presentedLevel(), 6, "InterruptController presents level 6 for Timer 1 match");
        checkEqual(board.interrupts().presentedAutovector(), 1,
                   "InterruptController presents autovector flag for ICR1 with AVEC set");

        // -------------------------------------------------------------------
        // Case group 3. INTC priority masking works (SR IPL >= 6 blocks interrupt).
        //
        // Reset left SR IPL at 7. Because 6 is not strictly greater than 7,
        // the interrupt must be inhibited.
        const uint32_t initialSr = board.mcuReg(16);
        checkEqual((initialSr >> 8) & 0x7u, uint32_t(7), "CPU status register has IPL = 7");

        // Step more cycles with IPL = 7: CPU must remain in the masked loop.
        board.runMcu(50);
        checkEqual(ram.read32(kFlagAddr), uint32_t(0), "while SR IPL = 7, interrupt is masked: ISR has not executed");
        checkEqual(board.mcuReg(15), kInitialSp,
                   "while SR IPL = 7, stack pointer is untouched (no exception frame pushed)");

        // Test priority boundary: SR IPL = 6 must also block level 6 interrupt.
        board.setMcuReg(16, 0x2600u); // Supervisor mode = 1, IPL = 6
        board.runMcu(50);
        checkEqual(ram.read32(kFlagAddr), uint32_t(0),
                   "while SR IPL = 6, interrupt is masked: level 6 does not exceed IPL 6");
        checkEqual(board.mcuReg(15), kInitialSp, "while SR IPL = 6, stack pointer remains untouched");

        // -------------------------------------------------------------------
        // Case group 4. Unmasking SR IPL allows interrupt take, stack frame
        // push, ISR entry, and RTE return.
        //
        // Lower SR IPL to 0. The next instruction boundary must take the
        // interrupt, push the ColdFire exception frame, vector to 0x600,
        // execute the ISR, clear TER1, and return to the interrupted loop.
        board.setMcuReg(16, 0x2000u); // Supervisor mode = 1, IPL = 0

        board.runMcu(60);

        check(!board.faulted(), "CPU took interrupt and executed ISR without faulting");
        checkEqual(ram.read32(kFlagAddr), uint32_t(1), "CPU entered ISR and incremented the test flag in memory");

        // Verify that ISR cleared the TER1 event register:
        const uint32_t ter1After = board.sim().read(0x151, 8, busStatus);
        checkEqual(ter1After & kTerRef, uint32_t(0), "ISR write of 2 to MBAR+0x151 cleared TER1[REF]");
        checkEqual(board.interrupts().presentedLevel(), 0,
                   "clearing TER1[REF] deasserted the interrupt: presented level dropped to 0");

        // Verify RTE restored registers:
        checkEqual(board.mcuReg(15), kInitialSp,
                   "RTE popped 8-byte exception frame: stack pointer restored to initial value");
        checkEqual(board.mcuReg(16), uint32_t(0x2000), "RTE restored status register (IPL returned to 0)");

        // Inspect the stacked exception frame left in memory at SP - 8:
        const uint32_t frameHeader = ram.read32(kInitialSp - 8);
        const uint32_t returnPc = ram.read32(kInitialSp - 4);
        const uint32_t format = (frameHeader >> 28) & 0x0fu;
        const uint32_t frameVector = (frameHeader >> 18) & 0xffu;
        const uint32_t savedSr = frameHeader & 0xffffu;

        checkEqual(format, uint32_t(4), "exception frame has ColdFire format 4 (0x4xxx)");
        checkEqual(frameVector, uint32_t(30), "exception frame carries vector 30 (level 6 autovector)");
        checkEqual(savedSr, uint32_t(0x2000), "exception frame preserves pre-interrupt status register");
        check(returnPc >= kCodeEntry && returnPc < kCodeEntry + kCodeProg1.size(),
              "exception frame return PC points inside the interrupted loop");
    }

    // -----------------------------------------------------------------------
    // Case group 5. End-to-end CPU-only execution with in-stream unmasking.
    //
    // The entire sequence runs from a single program across two execution quanta:
    //   Quantum 1:
    //     1. Sets VBR to vector table.
    //     2. Programs ICR1 and Timer 1.
    //     3. Loops with IPL = 7. Advancing timers at quantum end asserts level 6.
    //   Quantum 2:
    //     4. CPU unmasks SR IPL from assembly via `move.w #0x2000, %sr`.
    //     5. Core samples and takes level 6 autovectored interrupt, enters ISR,
    //        clears TER1, and returns via RTE to idle loop.
    {
        rg2::Board board(makeBoardConfig());
        TestRam ram(kRamSize);
        board.memory().attach(rg2::Region::Sdram, &ram);

        ram.write32(0x00, kInitialSp);
        ram.write32(0x04, kCodeEntry);
        ram.write32(0x78, kIsrEntry);

        ram.load(kCodeEntry, kCodeProg2);
        ram.load(kIsrEntry, kCodeIsr);

        board.resetMcu(kInitialSp, kCodeEntry);

        mcf5407_bus_status busStatus = MCF5407_BUS_OK;
        (void)busStatus;

        // Quantum 1: runs configuration and enters mask loop.
        // Advancing timers at completion of this quantum asserts the interrupt.
        const uint32_t q1 = board.runMcu(80);
        check(!board.faulted(), "quantum 1: CPU configured peripheral and entered mask loop without faulting");
        check(q1 > 0, "quantum 1: reported non-zero executed cycles");
        checkEqual(board.interrupts().presentedLevel(), 6,
                   "quantum 1: timer reached match and asserted level 6 interrupt");
        checkEqual(ram.read32(kFlagAddr), uint32_t(0), "quantum 1: while masked at IPL = 7, ISR has not executed");

        // Quantum 2: CPU unmasks IPL to 0 via `move.w #0x2000, %sr`, takes the
        // interrupt, runs ISR, and returns to idle loop via RTE.
        const uint32_t q2 = board.runMcu(300);
        check(!board.faulted(), "quantum 2: completed ISR and RTE without faulting");
        check(q2 > 0, "quantum 2: reported non-zero executed cycles");
        checkEqual(ram.read32(kFlagAddr), uint32_t(1),
                   "quantum 2: CPU unmasked IPL in-stream, entered ISR and set test flag");
        checkEqual(board.mcuReg(15), kInitialSp, "quantum 2: stack pointer fully restored by RTE");
        checkEqual(board.interrupts().presentedLevel(), 0,
                   "quantum 2: presented interrupt level deasserted following ISR completion");
    }

    if (g_failures)
    {
        std::cout << "t0_peripherals_cpu_exec: " << g_failures << " of " << g_cases << " cases failed" << std::endl;
        return 1;
    }

    std::cout << "t0_peripherals_cpu_exec: " << g_cases << " of " << g_cases << " cases passed" << std::endl;
    return 0;
}

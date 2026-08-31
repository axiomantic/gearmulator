// The MCF5307 M-Bus module at MBAR+$280, modelled as far as the firmware drives
// it. It is a BusTarget, so it takes the MBAR-relative offset the decode
// produced and carries no knowledge of where the boot loader put MBAR.
//
// The status register is not storage. MBB tracks the MSTA transitions in MBCR
// and MIF tracks byte completions, because the firmware requires MBB to read
// CLEAR after a STOP and SET after the next START, and MIF to be re-settable
// after each software clear. No constant satisfies either requirement.
//
// A not-acknowledge is not a fault. The firmware never inspects RXAK, so a NACK
// it cannot see must not become a failure this model invents. RXAK is reported
// and nothing else happens.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "memoryMap.h"

namespace g2
{
	/* A byte-level two-wire slave. The seam is a BYTE and not a bit because the
	 * M-Bus module is a byte-level master: the firmware hands it whole bytes
	 * through MBDR and reads whole bytes back, and no part of this model sees a
	 * clock edge. */
	class BusSlave
	{
	public:
		virtual ~BusSlave() = default;

		// The address phase. Returns TRUE when the slave acknowledges.
		virtual bool start(uint8_t _address7, bool _read) = 0;

		// One transmitted byte. Returns TRUE when the slave acknowledges.
		virtual bool write(uint8_t _byte) = 0;

		// One received byte.
		virtual uint8_t read() = 0;

		virtual void stop() = 0;
	};

	class MBus final : public BusTarget
	{
	public:
		// MCF5307UM Table 15-1. Every register is one byte wide and they sit
		// on four-byte centres.
		static constexpr uint32_t g_madr = 0x280u;
		static constexpr uint32_t g_mfdr = 0x284u;
		static constexpr uint32_t g_mbcr = 0x288u;
		static constexpr uint32_t g_mbsr = 0x28Cu;
		static constexpr uint32_t g_mbdr = 0x290u;

		static constexpr uint32_t g_base = g_madr;
		static constexpr uint32_t g_size = (g_mbdr + 4u) - g_madr;

		// MBCR, bit 7 to 0: MEN MIEN MSTA MTX TXAK RSTA - -
		static constexpr uint8_t g_men  = 0x80u;
		static constexpr uint8_t g_mien = 0x40u;
		static constexpr uint8_t g_msta = 0x20u;
		static constexpr uint8_t g_mtx  = 0x10u;
		static constexpr uint8_t g_txak = 0x08u;
		static constexpr uint8_t g_rsta = 0x04u;

		// MBSR, bit 7 to 0: MCF MAAS MBB MAL - SRW MIF RXAK
		static constexpr uint8_t g_mcf  = 0x80u;
		static constexpr uint8_t g_maas = 0x40u;
		static constexpr uint8_t g_mbb  = 0x20u;
		static constexpr uint8_t g_mal  = 0x10u;
		static constexpr uint8_t g_srw  = 0x04u;
		static constexpr uint8_t g_mif  = 0x02u;
		static constexpr uint8_t g_rxak = 0x01u;

		explicit MBus(BusSlave* _slave = nullptr);

		uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

		// One line for every access this model rejected, and one for every
		// offset inside the module window that carries no register.
		const std::vector<std::string>& log() const { return m_log; }
		void clearLog() { m_log.clear(); }

	private:
		void logLine(const char* _reason, bool _isWrite, int _size, uint32_t _offset);

		uint8_t readRegister(uint32_t _offset);
		void writeRegister(uint32_t _offset, uint8_t _value);

		void writeControl(uint8_t _value);
		void transmit(uint8_t _value);
		uint8_t receive();

		BusSlave* m_slave;

		uint8_t m_madr = 0u;
		uint8_t m_mfdr = 0u;
		uint8_t m_mbcr = 0u;

		// The byte the last receive clocked in. An MBDR read returns this and
		// starts the next receive, which is what the controller does.
		uint8_t m_received = 0u;

		bool m_busBusy       = false;   // MBB
		bool m_interrupt     = false;   // MIF
		bool m_notAcknowledged = false; // RXAK

		// TRUE between a START and the address byte that follows it.
		bool m_addressPhase = false;

		std::vector<std::string> m_log;
	};
}

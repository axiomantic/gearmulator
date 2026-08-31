// The anomaly log.
//
// The board model logs any peripheral register offset that does not match the
// MCF5307 manual, and it never adjusts the model in silence. Writing such an
// offset is accepted, the bus does not fault, but it changes no model state.
// Accessing it writes exactly one anomaly log line.
//
// The canonical anomaly is RAMBAR1 at offset $C05. The firmware writes it in
// genuine code in two places, and no MCF5307 manual assigns that offset to any
// register of this part: $C05 is a ColdFire V4 register and this part is V3.
//
// This file is a pure logging sink and carries no model state and no
// acceptance decision. That is what lets SIM, panel and a CPU-attached log all
// write to one log without any of them silently correcting a register to make
// a line go away.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace g2
{
	// The struct holds the address, the width and the direction separately
	// from the formatted line, so that a consumer can inspect them without
	// reparsing text.
	struct AnomalyEntry
	{
		std::string reason;
		bool isWrite;
		int sizeBits;
		uint32_t offset;
		std::string line;
	};

	// The full board anomaly log.
	class AnomalyLog
	{
	public:
		AnomalyLog() = default;

		// Each call appends exactly one line.
		void record(const char* _reason, bool _isWrite, int _sizeBits, uint32_t _offset);

		void clear() { m_entries.clear(); }

		std::size_t size() const { return m_entries.size(); }
		bool empty() const { return m_entries.empty(); }

		const AnomalyEntry& at(std::size_t _index) const { return m_entries[_index]; }
		const std::vector<AnomalyEntry>& entries() const { return m_entries; }

	private:
		std::vector<AnomalyEntry> m_entries;
	};

	namespace
	{
		// The fixed eight-hex-digit form the SIM's own trace uses, so that a
		// line in this log is lexically compatible with one the SIM writes.
		std::string anomalyHex32(const uint32_t _value)
		{
			static const char* digits = "0123456789abcdef";
			std::string result = "0x";
			for(int shift = 28; shift >= 0; shift -= 4)
				result += digits[(_value >> shift) & 0xfu];
			return result;
		}
	}

	void AnomalyLog::record(const char* _reason, const bool _isWrite, const int _sizeBits, const uint32_t _offset)
	{
		AnomalyEntry entry;
		entry.reason = _reason ? _reason : "";
		entry.isWrite = _isWrite;
		entry.sizeBits = _sizeBits;
		entry.offset = _offset;
		entry.line = "anomaly: " + entry.reason
			+ " " + (_isWrite ? "write of " : "read of ")
			+ std::to_string(_sizeBits) + " bits at offset " + anomalyHex32(_offset);
		m_entries.push_back(std::move(entry));
	}
}

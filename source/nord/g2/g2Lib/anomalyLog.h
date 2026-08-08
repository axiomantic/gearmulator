// Task BRD-5. The anomaly log.
//
// Plan section 13.1, BRD-5. Design section 6.3 and section 5.2.1 rule 2.
//
// WHAT THIS FILE IS. The board model logs any peripheral register offset that
// does not match the MCF5307 manual, and it NEVER adjusts the model in
// silence. Writing such an offset is ACCEPTED (the bus does not fault), but it
// changes no model state. Accessing it writes exactly one anomaly log line.
// A log line is the trigger for a LOGBOOK ENTRY, not for a quiet fix of the
// model.
//
// THE CANONICAL ANOMALY IS RAMBAR1 (offset $C05). AGENTS.md section 2.2
// records that the firmware writes RAMBAR1 in genuine code in two places, and
// that no MCF5307 manual assigns that offset to any register of this part --
// $C05 is a ColdFire V4 register and this part is V3. The board ACCEPTS the
// write (design section 6.3) and records one anomaly line. It does not
// silently turn the write into something the manual does describe, because a
// silent adjustment is exactly the failure the log exists to prevent.
//
// THIS FILE IS A PURE LOGGING SINK AND THAT IS DELIBERATE. It carries no model
// state and no acceptance decision. The acceptance rule (write to an offset
// the model does not carry: accepted, no state change, one log line) is the
// BOARD's contract, and each board model that meets it -- the SIM of task BRD-2
// is the first, and its own trace "sim: ..." is one writer of this full log --
// records its anomalies here. Keeping the sink free of model state is what
// lets SIM, panel and the future CPU-attached log all write to one log without
// any of them silently correcting a register to make a line go away.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). The default build is
// Release and it defines NDEBUG, so a bare assert() is removed and a check
// built on one can never fail.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace g2
{
	// One anomaly log line, carrying the address, the width and the access
	// direction together with the reason. The struct holds the three facts
	// separately from the formatted line so that a consumer can inspect them
	// without reparsing text, and so that the assertion "the line names the
	// offset, the width and the direction" does not depend on string matching.
	struct AnomalyEntry
	{
		std::string reason;
		bool isWrite;
		int sizeBits;
		uint32_t offset;
		std::string line;
	};

	// The full board anomaly log. Design section 6.3 requires it; section
	// 5.2.1 rule 2 ties a fault to its trace so that "a fault cannot be
	// reported without a trace of it".
	class AnomalyLog
	{
	public:
		AnomalyLog() = default;

		// Record one anomaly. Each call appends exactly one line. The offset,
		// the width and the direction are carried in the entry and are part of
		// the line text, so the log is self-describing and a reader does not
		// need another channel to learn what was accessed.
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
		// The offset is printed in the fixed eight-hex-digit form the SIM's
		// own trace uses, so that a line in this log is lexically compatible
		// with a line the SIM writes and a later unification does not have to
		// reconcile two spellings of one number.
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

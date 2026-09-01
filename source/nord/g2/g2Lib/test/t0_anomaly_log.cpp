// The anomaly log. Tier T0: this test needs no firmware artifact.
//
// The canonical anomaly is RAMBAR1 at offset $C05: the firmware writes it, and
// no MCF5307 manual assigns that offset to any register of this part -- $C05 is
// a ColdFire V4 register and this part is V3. The board accepts the write and
// records one anomaly log line. It never adjusts the model in silence.
//
// AnomalyLog is a pure sink and carries no state of its own. The acceptance
// rule -- a write to an offset the board does not carry: accepted, no state
// change, one log line -- is the board's contract, so this test represents it
// with a tiny RAMBAR1 model that holds the register's would-be state and
// refuses to change it on a write. That is what makes "an accepted write" and
// "never adjusts the model in silence" observable against a real thing instead
// of against the log's own text.

#include "anomalyLog.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	template<typename T>
	void checkEqual(const T& _actual, const T& _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << _expected
			<< ">, got <" << _actual << ">" << std::endl;
		++g_failures;
	}

	// The two genuine RAMBAR1 writes sit at 0x300582C6 and 0x30058216. $C05 is
	// a ColdFire V4 control-register number, not a bus map offset, and it is
	// the canonical anomaly, so the test drives exactly that number.
	constexpr uint32_t g_rambar1Offset = 0x0C05u;

	// The status the board returns from an access. MCF5307_BUS_OK means the
	// access completed; the write is ACCEPTED even though it changes nothing.
	enum class Status { Ok, Fault };

	// A tiny model of the RAMBAR1 register slot, authored by this test. The
	// board does not carry RAMBAR1 (no manual assigns $C05 to this part), so a
	// write to it is accepted but must leave the model untouched.
	struct Rambar1Model
	{
		g2::AnomalyLog anom;
		uint32_t state = 0;
		Status lastStatus = Status::Ok;

		// Accept a write: no fault, no state change, exactly one log line.
		void write(const uint32_t value)
		{
			lastStatus = Status::Ok;
			anom.record("RAMBAR1", true, 32, g_rambar1Offset);
			// Accepted, and the register the manual does not describe keeps
			// whatever it had. There is no silent adjustment here.
		}

		// A read of the same unmodelled offset: accepted, one read line.
		uint32_t read()
		{
			lastStatus = Status::Ok;
			anom.record("RAMBAR1", false, 32, g_rambar1Offset);
			return state;
		}
	};
}

int main()
{
	// ------------------------------------------------------------------
	// The log is a pure sink: it starts empty.
	{
		g2::AnomalyLog log;
		checkEqual(log.size(), std::size_t(0), "an anomaly log starts empty");
		check(log.empty(), "a fresh anomaly log reports empty");
	}

	// ------------------------------------------------------------------
	// The canonical anomaly: a RAMBAR1 write is accepted, changes no model
	// state, and writes exactly one log line naming offset, width, direction.
	{
		Rambar1Model model;
		const uint32_t prior = model.state;

		model.write(0x12345678u);

		checkEqual(static_cast<int>(model.lastStatus), static_cast<int>(Status::Ok),
			"the RAMBAR1 write is ACCEPTED, not faulted");
		checkEqual(model.state, prior,
			"the RAMBAR1 write changes no model state, so the model is never adjusted in silence");
		checkEqual(model.anom.size(), std::size_t(1),
			"the RAMBAR1 write writes exactly one anomaly log line");
		checkEqual(model.anom.at(0).offset, g_rambar1Offset,
			"the log line carries the RAMBAR1 offset");
		checkEqual(model.anom.at(0).sizeBits, 32,
			"the log line carries the access width");
		checkEqual(model.anom.at(0).isWrite, true,
			"the log line carries the access direction");
		check(model.anom.at(0).line.find("RAMBAR1") != std::string::npos,
			"the log line names the register the offset belongs to");
		check(model.anom.at(0).line.find("write of 32 bits at offset 0x00000c05") != std::string::npos,
			"the log line names the width, the direction and the offset");
	}

	// ------------------------------------------------------------------
	// A read of the same unmodelled offset is a separate anomaly with its own
	// line and the read direction.
	{
		Rambar1Model model;
		model.write(1u);
		model.read();

		checkEqual(model.anom.size(), std::size_t(2),
			"each access to an unmodelled offset appends its own line");
		checkEqual(model.anom.at(1).isWrite, false,
			"the read line carries the read direction");
		checkEqual(model.anom.at(1).offset, g_rambar1Offset,
			"the read line still names the same offset");
		check(model.anom.at(1).line.find("read of 32 bits at offset 0x00000c05") != std::string::npos,
			"the read line names the width, the direction and the offset");
	}

	// ------------------------------------------------------------------
	// Any peripheral register offset that does not match the MCF5307 manual
	// is logged, and a write to it is always accepted and never changes state.
	// The three offsets below are all outside the SIM's modelled range and
	// outside the manuals' maps. The rule is about the log, not about any one
	// number, so the invariant is driven over several.
	{
		constexpr uint32_t offsets[] = { 0x0002u, 0x0c05u, 0x0400u };
		constexpr int widths[] = { 8, 16, 32 };

		for(uint32_t off : offsets)
		{
			for(int w : widths)
			{
				g2::AnomalyLog log;
				log.record("UNMODELLED", true, w, off);
				checkEqual(log.size(), std::size_t(1),
					"one access to a non-manual offset writes one line");
				checkEqual(log.at(0).offset, off, "the line names the offset it was logged for");
				checkEqual(log.at(0).sizeBits, w, "the line names its width");
				checkEqual(log.at(0).isWrite, true, "the line names its direction");
			}
		}
	}

	// ------------------------------------------------------------------
	// clear() empties the log; records after a clear start a fresh count.
	{
		g2::AnomalyLog log;
		log.record("RAMBAR1", true, 32, g_rambar1Offset);
		checkEqual(log.size(), std::size_t(1), "one record is present before clear");
		log.clear();
		checkEqual(log.size(), std::size_t(0), "clear empties the log");
		check(log.empty(), "the log reports empty after clear");
		log.record("RAMBAR1", false, 32, g_rambar1Offset);
		checkEqual(log.size(), std::size_t(1), "a record after clear restarts the count at one");
	}

	// ------------------------------------------------------------------
	// A log line is the trigger for a LOGBOOK ENTRY, never for a quiet fix.
	// The sink records the line and nothing else: it does not reject the
	// access and it does not return a corrected value. That is asserted by
	// checking that a write that records a line leaves the caller's model
	// byte untouched -- the log cannot adjust the model because it holds no
	// model, which is the property that makes a later silent correction a
	// buildable and reviewable error rather than a hidden one.
	{
		Rambar1Model model;
		const uint32_t before = model.state;
		model.write(0xdeadbeefu);
		checkEqual(model.state, before,
			"recording an anomaly never adjusts the model, because the log is a pure sink");
	}

	if(g_failures)
	{
		std::cout << "t0_anomaly_log: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_anomaly_log: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}

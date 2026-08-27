#pragma once

/* The outcome of a transport probe, as one word.
 *
 * `--impulse` injects a known, non-silent sample at the codec source and
 * observes whether and where it propagates. It is a transport probe and not an
 * audio claim: it answers "does the path carry data", never "does the machine
 * make music". The default state of a Nord Modular is to not play sound, and
 * sound comes from loading patches, so a chain that carries nothing on an
 * unpatched machine is the emulator agreeing with the hardware and is not a
 * defect in the transport.
 *
 * A word rather than a figure, because an unobserved impulse and a blind
 * observer both print `arrival=-1`.
 *
 * The classification is a free function over a plain record so that every arm
 * is reachable from a test without booting a machine. The console fills the
 * record from the run; the test fills it directly. */

namespace g2console
{
	enum class ImpulseOutcome
	{
		// The play phase was never reached: no artifact, an image that did not
		// place, a halted or faulted MCU, or kernels that never landed. Nothing
		// about the chain was measured, and the word says so rather than
		// reporting a chain that carried nothing.
		DidNotRun,

		// Every arm below this line rests on the observer having observed
		// something. This one says it did not, so no arm below is available.
		InstrumentBlind,

		// The observer received frames and none carried the pattern: the chain
		// did not carry it to the sink. Expected on a machine with no patch
		// loaded.
		Stopped,

		// The pattern reached the codec sink, unchanged, at the derived frame.
		Propagated,

		// The pattern reached the codec sink, but late, early, changed, or
		// beside a non-zero chain-health counter.
		PropagatedOffSpec
	};

	struct ImpulseObservation
	{
		/* Did the machine reach the play phase at all. */
		bool     reachedPlayPhase = false;

		/* The observer's own known-positive/known-negative control, run on the
		 * comparator this program uses and not on the chain. False means the
		 * detector cannot detect, so a report of "no arrival" would say nothing
		 * about the chain. */
		bool     observerSelfTest = false;

		/* Frames the sink actually delivered across the walk. Zero is the
		 * blindness case: a zero-filled buffer that was never written looks
		 * exactly like silence that was. */
		unsigned framesPulled     = 0;

		/* The walk quantum at which a non-silent frame first appeared at the
		 * sink, or -1 for none. */
		int      arrival          = -1;

		/* Whether that frame carried the injected pattern unchanged. */
		bool     arrivalExact     = false;

		/* The derived expectation, (dspCount - 1) * hopFrames. */
		unsigned expectedArrival  = 0;

		/* Every chain-health counter read zero across the walk. */
		bool     countersZero     = false;

		/* THE ARRIVAL INSTRUMENT'S OWN KNOWN POSITIVE, AND IT IS A DIFFERENT
		 * CONTROL FROM observerSelfTest. That one drives the detector's two
		 * predicates over two frames the PROGRAM built; it holds even when
		 * nothing between the tail DSP and `Scheduler::pull` works, because no
		 * part of that path is on its evidence. These two fields carry the
		 * result of a walk in which a known sentinel was placed at the tail
		 * position's TRANSMIT SOURCE and then read back OUT OF THE SINK, so
		 * they are a statement about the arrival path itself.
		 *
		 * sinkControlArrival is the control walk's quantum at which the sink
		 * first delivered a non-silent frame, or -1 for none; sinkControlExact
		 * says the frame carried the sentinel unchanged. A path that mangles a
		 * KNOWN value has not earned the right to be believed about an
		 * unknown one, so both are required. */
		int      sinkControlArrival = -1;
		bool     sinkControlExact   = false;
	};

	/* The order of these clauses runs from the weakest premise outwards. Each
	 * clause rests on the one above it having held: a chain verdict rests on
	 * the observer having seen something, and the observer's report rests on
	 * the machine having run. Reversing any two would let a later clause read
	 * a field the earlier one has just said is meaningless. */
	constexpr ImpulseOutcome classify(const ImpulseObservation& _o)
	{
		// Nothing ran, so every field below it describes a machine that was
		// never driven and none of them may be read as a chain verdict.
		if(!_o.reachedPlayPhase)
			return ImpulseOutcome::DidNotRun;

		/* THE ZERO IS NOT ALLOWED TO PASS UNPAIRED. An absence reported by an
		 * instrument that cannot observe is not an absence, so both halves of
		 * "the observer worked" are required before an arrival or its lack is
		 * given any meaning: the detector proved on a known positive and a
		 * known negative, and the sink having delivered at least one frame for
		 * it to look at. A buffer nothing wrote reads exactly like silence. */
		/* AND THE THIRD HALF, WHICH THE FIRST TWO DO NOT COVER. The comparator
		 * self-test proves the DETECTOR; the frame count proves the sink
		 * DELIVERED something. Neither proves the path BETWEEN the tail DSP and
		 * that delivery can carry a value: a sink that hands over a zero frame
		 * every quantum satisfies both and reports `arrival=-1` forever. The
		 * sink control is the known positive for exactly that path, and it must
		 * have arrived AND arrived unchanged. */
		if(!_o.observerSelfTest || _o.framesPulled == 0
			|| _o.sinkControlArrival < 0 || !_o.sinkControlExact)
			return ImpulseOutcome::InstrumentBlind;

		// The observer worked and saw no pattern: the chain did not carry it.
		// On an unpatched machine this is the correct answer.
		if(_o.arrival < 0)
			return ImpulseOutcome::Stopped;

		if(_o.arrival == int(_o.expectedArrival) && _o.arrivalExact && _o.countersZero)
			return ImpulseOutcome::Propagated;

		return ImpulseOutcome::PropagatedOffSpec;
	}

	constexpr const char* name(const ImpulseOutcome _outcome)
	{
		switch(_outcome)
		{
		case ImpulseOutcome::DidNotRun:         return "DID-NOT-RUN";
		case ImpulseOutcome::InstrumentBlind:   return "INSTRUMENT-BLIND";
		case ImpulseOutcome::Stopped:           return "STOPPED";
		case ImpulseOutcome::Propagated:        return "PROPAGATED";
		case ImpulseOutcome::PropagatedOffSpec: return "PROPAGATED-OFF-SPEC";
		}

		return "UNCLASSIFIED";
	}

	/* The exit status separates "the answer is no" from "there is no answer":
	 * 1 is a chain that did not carry the pattern, 2 is a machine that never
	 * ran, and 3 is an instrument that could not see. */
	constexpr int exitStatus(const ImpulseOutcome _outcome)
	{
		switch(_outcome)
		{
		case ImpulseOutcome::Propagated:        return 0;
		case ImpulseOutcome::Stopped:           return 1;
		case ImpulseOutcome::PropagatedOffSpec: return 1;
		case ImpulseOutcome::DidNotRun:         return 2;
		case ImpulseOutcome::InstrumentBlind:   return 3;
		}

		return 1;
	}
}

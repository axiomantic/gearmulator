#pragma once

// Task REPO-5. Design section 4.2 is the definition site of this interface and
// this file implements that section's declaration and no other.
//
// The Python half of this task is `nmg2_tools/artifacts.py` in
// `axiomantic/nmg2-tools`. Both halves are written by ONE task so that the two
// messages cannot drift; plan section 7.4.2 records that as the reason
// `nmg2_tools/artifacts.py` has REPO-5 as its owner.

#include <string>

namespace g2
{
	// The message a failed resolve writes, WORD FOR WORD. Design section 4.2
	// fixes the wording and section 18.5 builds the skip line on top of it.
	//
	// It reads "unset" for BOTH failure cases -- the variable unset, and the
	// variable naming a directory that is not there. That is the design's own
	// decision: section 4.2 gives the two cases one message, and REPO-5's check
	// requires "the result is the same". This constant is not the place to
	// improve on it.
	constexpr const char* g_artifactUnavailableMessage = "firmware artifact not available (NMG2_ARTIFACTS unset)";

	/* Resolves the directory that holds the Clavia-derived artifacts.
	 *
	 * Ownership   The test fixture, or the plugin's construction path, owns the
	 *             resolver. Nothing in the emulation holds one.
	 * Lifetime    Setup time only. Destroyed before the first quantum runs.
	 * Threading   Called from test setup and from plugin construction. NEVER
	 *             from the scheduler thread and never from the audio callback.
	 *             An implementation is therefore allowed to do slow work.
	 */
	class ArtifactResolver
	{
	public:
		virtual ~ArtifactResolver() = default;

		/* Returns the directory, or an empty string. On an empty return, `why`
		 * carries the reason in the exact words section 18.5 requires. On a
		 * non-empty return, `why` is cleared. Never throws: this obeys the
		 * no-exceptions rule of design sections 5.3 and 13.10. */
		virtual std::string resolve(std::string& _why) = 0;
	};

	/* The only implementation the design specifies. Reads NMG2_ARTIFACTS.
	 * When the variable is unset, empty, or names anything that is not an
	 * existing directory, it returns an empty string and writes
	 * g_artifactUnavailableMessage. */
	class EnvArtifactResolver final : public ArtifactResolver
	{
	public:
		std::string resolve(std::string& _why) override;
	};
}

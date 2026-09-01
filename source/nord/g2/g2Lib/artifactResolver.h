#pragma once

// The artifact resolver interface.
//
// The Python half is `nmg2_tools/artifacts.py` in `axiomantic/nmg2-tools`. The
// three messages are word-for-word identical between the two halves and are
// named in this header's comment on `EnvArtifactResolver::resolve`.

#include <string>

namespace g2
{
	// Message 1 of 3. It fires when the variable is unset or empty. The skip
	// line is built on top of it.
	//
	// Windows removes a variable
	// through _putenv_s by assigning it the empty string, so the empty value
	// must behave as the unset value or the two halves of this task would
	// disagree on the same input.
	constexpr const char* g_artifactUnavailableMessage = "firmware artifact not available (NMG2_ARTIFACTS unset)";

	// Message 2 of 3. The variable is set but names something that is not an
	// existing directory. Echoes the variable's value unchanged so an operator
	// with a wrong path sees the path they actually typed rather than the
	// message for the case they did not hit.
	//
	// Message 3 of 3. The variable is set, names an existing directory, and the
	// caller asked for a file that is not in it. Echoes the variable's value
	// unchanged for the same reason as message 2.
	//
	// Messages 2 and 3 are built by concatenation in
	// EnvArtifactResolver::resolve because the wording includes the variable's
	// value, and a constant cannot carry a runtime value.

	/* Resolves the directory that holds the Clavia-derived artifacts.
	 *
	 * Ownership   The test fixture, or the plugin's construction path, owns the
	 *             resolver. Nothing in the emulation holds one.
	 * Lifetime    Setup time only. Destroyed before the first quantum runs.
	 * Threading   Called from test setup and from plugin construction. Never
	 *             from the scheduler thread and never from the audio callback.
	 *             An implementation is therefore allowed to do slow work.
	 */
	class ArtifactResolver
	{
	public:
		virtual ~ArtifactResolver() = default;

		/* Returns the directory, or an empty string. On an empty return, `why`
		 * carries one of three messages:
		 *
		 *   1. G_artifactUnavailableMessage when the variable is unset or empty.
		 *   2. "firmware artifact not available (NMG2_ARTIFACTS names no
		 *      directory: <path>)" when the variable names something that is
		 *      not an existing directory.
		 *   3. "firmware artifact not available (<name> not found under
		 *      NMG2_ARTIFACTS: <path>)" when the variable names an existing
		 *      directory but the named artifact is not in it. Message 3 only
		 *      fires when `_name` is not null; passing null makes a present
		 *      directory a successful resolve and never fires message 3.
		 *
		 * On a non-empty return, `why` is cleared. Never throws.
		 *
		 * `_name` is the artifact the caller asked for, matching the Python
		 * `resolve_artifacts(name: Optional[str] = None)`. */
		virtual std::string resolve(std::string& _why, const char* _name = nullptr) = 0;
	};

	/* Reads NMG2_ARTIFACTS.
	 *
	 * Three failure messages, one per condition, all word-for-word identical
	 * to `nmg2_tools/artifacts.py` in `axiomantic/nmg2-tools`:
	 *
	 *   unset or empty    -> g_artifactUnavailableMessage
	 *   not a directory   -> "firmware artifact not available (NMG2_ARTIFACTS
	 *                        names no directory: <value>)"
	 *   file not present  -> "firmware artifact not available (<name> not found
	 *                        under NMG2_ARTIFACTS: <value>)"
	 *
	 * On success, returns the directory unchanged and clears `why`. */
	class EnvArtifactResolver final : public ArtifactResolver
	{
	public:
		std::string resolve(std::string& _why, const char* _name = nullptr) override;
	};
}

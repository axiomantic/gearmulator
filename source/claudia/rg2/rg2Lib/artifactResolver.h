#pragma once

// Resolves directory holding firmware artifacts.

#include <string>

namespace rg2
{
	// Returned when NMG2_ARTIFACTS is unset or empty.
	constexpr const char* g_artifactUnavailableMessage = "firmware artifact not available (NMG2_ARTIFACTS unset)";

	class ArtifactResolver
	{
	public:
		virtual ~ArtifactResolver() = default;

		// Resolves directory, optionally checking for _name. On failure returns empty and sets _why.
		virtual std::string resolve(std::string& _why, const char* _name = nullptr) = 0;
	};

	// Environment variable (NMG2_ARTIFACTS) resolver.
	class EnvArtifactResolver final : public ArtifactResolver
	{
	public:
		std::string resolve(std::string& _why, const char* _name = nullptr) override;
	};
} // namespace rg2

#pragma once

#include <string>

namespace RuntimeDependencies
{
	// Build one stable detail string for the user-facing warning.  Kept pure
	// so the diagnostic remains unit-testable without Steam or the logger.
	inline std::string missingTools(bool hasUnzip, bool hasGzip,
	                                bool hasTimeout)
	{
		std::string missing;
		if (!hasUnzip) missing = "unzip";
		if (!hasGzip)
		{
			if (!missing.empty()) missing += ", ";
			missing += "gzip";
		}
		if (!hasTimeout)
		{
			if (!missing.empty()) missing += ", ";
			missing += "timeout";
		}
		return missing;
	}

	// Format missing helper names from an explicit PATH value.  This
	// deterministic PATH probe is used by check() and is exposed so tests can
	// supply a fixture without depending on the host's PATH or logger.
	std::string missingToolsOnPath(const char* path);

	// Probe the external helpers used by manifest extraction, CM gzip
	// expansion, and Steamless timeout enforcement once per process. Returns
	// true when all are available.
	bool check();

}

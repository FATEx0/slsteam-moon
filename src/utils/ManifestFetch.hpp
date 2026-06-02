
#pragma once

#include <cstdint>
#include <optional>


namespace ManifestFetch
{
	int getTimeoutSec();
	const char* defaultTimeoutKey();

	void submit(uint64_t jobId, uint64_t manifestGid,
	            uint32_t appId, uint32_t depotId);

	void submitManifestBlob(uint64_t manifestGid,
	                        uint32_t appId, uint32_t depotId);

	std::optional<uint64_t> resolve(uint64_t jobId);

	void discard(uint64_t jobId);
}

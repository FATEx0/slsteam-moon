
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

	// Submit (or join an in-flight one) and block up to timeoutSec for the
	// manifest blob to land on disk.  Returns true if the .manifest file is
	// now present (or was already there).  Used by BYldRequestDepotManifest
	// to avoid the "no internet / hit retry" UX: Steam's first install
	// attempt finds the file already cached.
	bool awaitManifestBlob(uint64_t manifestGid, uint32_t depotId,
	                       int timeoutSec);

	bool fetchManifestBlobSync(uint64_t manifestGid, uint32_t depotId);

	std::optional<uint64_t> resolve(uint64_t jobId);

	void discard(uint64_t jobId);
}

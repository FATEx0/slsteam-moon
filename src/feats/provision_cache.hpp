// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure freshness logic for the AdditionalApps provisioning cache.
//
// AppInfoProvision::provisionApp does one synchronous HTTP GET per added
// app, and Steam re-execs setup() several times during a single cold boot,
// so the naive cost is O(n_apps * n_passes) network round-trips that grow
// with every game the user adds.  We short-circuit the fetch when a
// freshly-written picsbuffer_<appid>.bin is already on disk.
//
// The window (TTL) is intentionally short: it must cover the re-exec storm
// of ONE boot (so those passes reuse the buffer and Steam launches fast)
// without surviving into a later genuine relaunch — across sessions we
// re-fetch the live public gid, because the install-first-attempt path
// stages whatever gid the buffer carries and Steam refreshes appinfo to
// the live gid at install time, so a stale cross-session buffer would
// reintroduce the gid mismatch this project already fixed.
//
// This header is PURE (no I/O) so the decision is unit-testable; the
// stat()/fetch/persist wiring lives in appinfo_provision.cpp.

#pragma once

#include <cstdint>

namespace AppInfoProvision
{
namespace cache
{

// Identity of the on-disk buffer used as the memoization key for its
// expensive YAML/SHA-1/VDF validation.  Seconds plus size are not enough:
// an atomic replacement can preserve both while changing the content within
// the same second.  Include nanoseconds and inode so that replacement files
// cannot inherit a previous validation result.
struct CacheValidationKey
{
	uint32_t appId = 0;
	long long mtimeSecs = 0;
	long long mtimeNsecs = 0;
	long long size = 0;
	std::uint64_t inode = 0;

	bool operator==(const CacheValidationKey& other) const noexcept
	{
		return appId == other.appId
		    && mtimeSecs == other.mtimeSecs
		    && mtimeNsecs == other.mtimeNsecs
		    && size == other.size
		    && inode == other.inode;
	}

	bool operator<(const CacheValidationKey& other) const noexcept
	{
		if (appId != other.appId) return appId < other.appId;
		if (mtimeSecs != other.mtimeSecs) return mtimeSecs < other.mtimeSecs;
		if (mtimeNsecs != other.mtimeNsecs)
			return mtimeNsecs < other.mtimeNsecs;
		if (size != other.size) return size < other.size;
		return inode < other.inode;
	}
};

enum class CacheUse
{
	None,
	Fresh,
	Fallback,
};

// A metadata record written before the provenance marker was introduced is
// ambiguous: it may be a provider-normalized pair or a raw PICS pair. Keep
// it authoritative against raw replacement until a provider refresh writes
// an explicit marker. Explicit raw records remain replaceable.
inline bool shouldPreserveCacheFromRawPics(bool hasNormalizedMarker,
                                           bool normalized)
{
	return !hasNormalizedMarker || normalized;
}

// Permit a cache publication only when the caller still represents the
// managed app generation that started the work. A matching generation is
// required even when the app is managed again after a removal.
inline bool cachePublicationAllowed(bool managed,
                                    std::uint64_t expectedGeneration,
                                    std::uint64_t currentGeneration)
{
	return managed && expectedGeneration == currentGeneration;
}

inline bool protonPublicationAllowed(bool managed,
                                      std::uint64_t expectedGeneration,
                                      std::uint64_t currentGeneration)
{
	return cachePublicationAllowed(managed, expectedGeneration,
	                               currentGeneration);
}

// A cache pair and its provenance marker are one logical publication. A
// reader must reject either half of an interrupted transition: a synthetic
// pair without its marker, or a normal pair retaining an old marker.
inline bool syntheticMarkerConsistent(bool synthetic, bool markerPresent)
{
	return synthetic == markerPresent;
}

// Publication-side form of the same invariant: the marker write must have
// succeeded AND the resulting on-disk marker must match the pair's provenance.
// Either failure leaves an interrupted transition, so the caller restores the
// previous pair and marker instead of publishing half of one.
inline bool syntheticMarkerPublicationConsistent(bool synthetic,
                                                 bool operationSucceeded,
                                                 bool markerPresent)
{
	return operationSucceeded && syntheticMarkerConsistent(synthetic, markerPresent);
}

// Metadata written before the explicit provenance field existed is still
// readable: back then `synthetic_<appid>` itself was the persisted provenance
// bit, so a legacy pair is valid with or without that marker. Explicit records
// must agree with their marker.
inline bool syntheticMarkerStateConsistent(bool hasSyntheticMetadata,
                                            bool synthetic,
                                            bool markerPresent)
{
	// Legacy normal (no marker) and legacy synthetic (marker present) pairs
	// are both valid, so provenance cannot be inferred and is not required.
	if (!hasSyntheticMetadata) return true;
	return syntheticMarkerConsistent(synthetic, markerPresent);
}

// A managed-source removal may retain only the synthetic marker when the app
// remains active through compatibility ownership. That marker is a durable
// PICS-protection state, not a readable cache pair; it must remain effective
// after a process restart while the metadata file is absent.
inline bool retainedSyntheticMarkerProtectionAllowed(
    bool markerPresent, bool metadataPresent, bool activeCompatibility)
{
	return markerPresent && !metadataPresent && activeCompatibility;
}

// Preserve the PICS protection marker only while compatibility ownership is
// retained and the pair is known to be synthetic. An explicit normal pair
// with a stale marker must not be promoted into synthetic state.
inline bool shouldPreserveSyntheticMarker(bool retainCompatibility,
                                           bool isSynthetic)
{
	return retainCompatibility && isSynthetic;
}

struct CacheRecordFacts
{
	unsigned int requestedAppId = 0;
	unsigned int metadataAppId = 0;
	unsigned long long declaredSize = 0;
	unsigned long long actualSize = 0;
	unsigned long long shaSize = 0;
	bool shaMatches = false;
	bool parsed = false;
	bool hasUsableContent = false;
};

inline bool isCacheRecordValid(const CacheRecordFacts& facts)
{
	return facts.requestedAppId != 0
	    && facts.metadataAppId == facts.requestedAppId
	    && facts.declaredSize > 0
	    && facts.declaredSize == facts.actualSize
	    && facts.shaSize == 20
	    && facts.shaMatches
	    && facts.parsed
	    && facts.hasUsableContent;
}

// A stale buffer may preserve the last known-good appinfo only after the live
// refresh path is unavailable. It must never win while online, where doing so
// would conceal new change numbers and manifest gids.
inline CacheUse chooseCacheUse(bool cacheValid, bool cacheFresh,
                               bool refreshUnavailable)
{
	if (!cacheValid) return CacheUse::None;
	if (cacheFresh) return CacheUse::Fresh;
	return refreshUnavailable ? CacheUse::Fallback : CacheUse::None;
}

// Full validation is needed only on paths that can serve the buffer: a fresh
// same-boot hit or an offline fallback.  A stale online buffer is going to be
// refreshed and must not pay the YAML/SHA-1/VDF validation cost.
inline bool shouldValidateCache(bool cacheFresh, bool refreshUnavailable)
{
	return cacheFresh || refreshUnavailable;
}

inline bool wireSizeMatches(unsigned long long actualSize,
                            unsigned long long declaredSize)
{
	return declaredSize > 0 && actualSize == declaredSize;
}

// Decide whether an existing provisioned buffer can be reused (i.e. the
// network fetch can be skipped) given:
//   bufExists  — whether picsbuffer_<appid>.bin is present and non-empty
//   mtimeSecs  — that file's last-modified time, in epoch seconds
//   nowSecs    — current time, in epoch seconds
//   ttlSecs    — freshness window; <= 0 disables the cache entirely
//
// Reuse only when the buffer exists and its age is strictly within the
// TTL.  A future mtime (clock skew / tampering) is not trusted.
inline bool isBufferReusable(bool bufExists, long long mtimeSecs,
                             long long nowSecs, long long ttlSecs)
{
	if (!bufExists)    return false;
	if (ttlSecs <= 0)  return false;

	const long long age = nowSecs - mtimeSecs;
	if (age < 0)       return false;   // mtime in the future — don't trust it
	return age < ttlSecs;
}

} // namespace cache
} // namespace AppInfoProvision

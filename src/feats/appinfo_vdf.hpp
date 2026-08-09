// SPDX-License-Identifier: AGPL-3.0-only
//
// appinfo.vdf v41 reader/writer.
//
// Steam keeps its product info cache at `<Steam>/appcache/appinfo.vdf`.
// On boot it reads the entire file into RAM (`ThreadedReadFromDisk`) and
// every depot/manifest lookup the downloader does goes through that
// in-memory cache.  For an app the user doesn't own, no entry is ever put
// in this cache, so the downloader fails fast with `Manifest: 0` and
// `BYldRequestDepotManifest 'Invalid Parameter'`.
//
// Path A.5: between Steam runs, splice synthetic AppInfo entries into
// `appinfo.vdf` for our AdditionalApps.  The data comes from the PICS
// response we already capture (see feats/pics.cpp).  When Steam next
// starts and reads the file, it sees the entry, the downloader finds the
// manifest GID, and the install proceeds normally.
//
// File format (v41), captured here for reference:
//
//   header:
//     uint32  magic           (0x07564429 for v41)
//     uint32  universe        (always 1)
//     int64   string_table_offset
//
//   apps[]:                    (repeated until appid == 0)
//     uint32  appid
//     uint32  size              (bytes from this point until end of binary VDF)
//     uint32  info_state        (1 = prerelease/no info, 2 = normal)
//     uint32  last_updated      (unix time)
//     uint64  pics_token
//     bytes   sha[20]           (SHA-1 of the original PICS protobuf buffer)
//     uint32  change_number
//     bytes   binary_hash[20]   (v40+, SHA-1 of the binary VDF that follows)
//     bytes   binary_vdf[]      (KeyValues1Binary; in v41 keys are uint32
//                                indices into the string table)
//
//   footer:
//     uint32  0
//
//   string_table (at string_table_offset):
//     uint32          string_count
//     bytes           strings[string_count]   (each null-terminated UTF-8)
//
// PICS-wire buffers use v39-style binary KV (cstring keys inline).  This
// module's writer turns those into v41-indexed binary KV during the
// splice, looking up cstring keys in the existing string table and
// extending it with new strings as needed.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "synthmark.hpp"

namespace AppInfoVdf
{
	// Append (or replace, by appid) one entry into the on-disk
	// appinfo.vdf at `path`.  `wireBuffer` is the v39-inline binary VDF
	// from the PICS response.  `sha` is the 20-byte sha from the same
	// response (stored verbatim in the entry).  Returns true on success.
	//
	// Idempotent: if an entry for `appid` with the same `changeNumber`
	// already exists in the file, the function returns true without
	// rewriting.
	bool injectApp(const std::string& path,
	               uint32_t appid,
	               uint32_t changeNumber,
	               const std::string& sha,
	               const std::string& wireBuffer);

	// Convenience: load every cached PICS buffer from
	// `<config>/cache/picsbuffer_*.{bin,yaml}` and inject each one into
	// the appinfo.vdf at `path`.  Returns the number of entries
	// successfully injected (or unchanged-already-present).
	int injectAllCached(const std::string& path);
}

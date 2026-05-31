// SPDX-License-Identifier: AGPL-3.0-only
//
// Steamtools-Linux: PICS appinfo injection.
//
// Background: when the user clicks "Install" on an app they don't own, Steam
// pulls product info from PICS (CMsgClientPICSProductInfo{Request,Response}).
// For unowned apps the response leaves the app in `unknown_app_ids` and the
// `apps` repeated field is empty.  Steam then tries to read the manifest GID
// from its appinfo cache, fails, and aborts with:
//
//   CDepotDownloadMgr::BYldRequestDepotManifest(App: X, Depot: X, Manifest: 0,
//     branch: ''): Failed to get manifest request code, 'Invalid Parameter'
//
// Our solution: hook the CMsgClientPICSProductInfoResponse (EMSG 8904) and
// inject a synthetic `AppInfo` entry for any app in our local catalog,
// containing a minimal Binary KeyValues (BKV) blob with the depot/manifest
// data the downloader needs.
//
// Phase 1 (current): diagnostic-only.  Log every PICS response we observe so
// we can confirm the field layout against real traffic before we start
// writing BKV.
//
// Phase 2: BKV writer + injection.  Layout will be ported from SteamKit2's
// KeyValue.SaveAsBinary() and SteamDatabase/SteamAppInfo's appinfo-buffer
// schema, both MIT-licensed C# references.

#pragma once

class CProtoBufMsgBase;
class CMsgClientPICSProductInfoResponse;

namespace PICS
{
	void recvMsg(CProtoBufMsgBase* msg);
	void recvProductInfoResponse(CMsgClientPICSProductInfoResponse* resp);
}

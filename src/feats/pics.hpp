// SPDX-License-Identifier: AGPL-3.0-only
//
// PICS appinfo handler.
//
// Background: Steam pulls product info from PICS
// (CMsgClientPICSProductInfo{Request,Response}). The default
// response shape leaves apps the user doesn't have a license for
// out of the populated `apps` list (the appid lands in
// `unknown_app_ids`). Steam then tries to read the manifest GID
// from its appinfo cache, fails, and aborts with:
//
//   CDepotDownloadMgr::BYldRequestDepotManifest(App: X, Depot: X,
//     Manifest: 0, branch: ''): Failed to get manifest request code,
//     'Invalid Parameter'
//
// This module hooks CMsgClientPICSProductInfoResponse (EMSG 8904)
// and persists per-app Binary KeyValues (BKV) buffers to the local
// cache so the offline appinfo splice can put them in front of
// Steam on the next start. The injection is paired with the
// outbound-side `meta_data_only=false` flip in feats/apps.cpp so
// Valve actually returns the buffer.

#pragma once

class CProtoBufMsgBase;
class CMsgClientPICSProductInfoResponse;

namespace PICS
{
	void recvMsg(CProtoBufMsgBase* msg);
	void recvProductInfoResponse(CMsgClientPICSProductInfoResponse* resp);
}

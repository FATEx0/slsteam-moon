// Standalone test for the pure Steam CM websocket wire helpers
// (src/feats/cmwire.hpp).
//
// Why this exists
// ---------------
// We are replacing the external api.steamcmd.net product-info dependency
// with a native anonymous Steam CM websocket client.  The wire framing,
// CMsgMulti expansion, and PICS request/response (de)serialisation are
// PURE byte transforms with no I/O, so they can — and must — be unit
// tested in isolation.  The impure transport (TLS, websocket handshake,
// CM server list) lives in feats/cmclient.cpp and is verified live.
//
// Steam CM packet framing per websocket binary message:
//   <uint32 emsg (high bit = proto mask 0x80000000)>
//   <uint32 header_len>
//   <CMsgProtoBufHeader bytes>
//   <body protobuf bytes>
//
// Build (Tasks 1-2, pure, from repo root):
//   g++ -std=c++20 -I include tools/test_cmwire.cpp -o /tmp/test_cmwire && /tmp/test_cmwire
// Build (Task 3, links project protobufs):
//   make test-cmwire && /tmp/test_cmwire

#include "../src/feats/cmwire.hpp"

#include <cstdio>
#include <cstring>
#include <unordered_map>

static int g_failures = 0;

#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

int main()
{
	// --- Task 1: packet pack/parse round-trip -----------------------------
	{
		// packet = <u32 emsg|0x80000000><u32 hlen><hdr bytes><body bytes>
		const std::string hdr = "HDR", body = "BODY";
		const auto pkt = CmWire::packPacket(820, hdr, body); // 820 = ClientLogon

		uint32_t emsg = 0;
		std::string outHdr, outBody;
		const bool ok = CmWire::parsePacket(pkt, emsg, outHdr, outBody);
		CHECK(ok, "parse ok");
		CHECK(emsg == 820, "emsg masked off");
		CHECK(outHdr == "HDR", "header slice");
		CHECK(outBody == "BODY", "body slice");

		// The on-wire emsg must carry the proto mask high bit set.
		uint32_t rawEmsg = 0;
		std::memcpy(&rawEmsg, pkt.data(), 4);
		CHECK((rawEmsg & 0x80000000u) != 0, "proto mask set on wire");

		// Empty header + body must still round-trip.
		const auto pkt2 = CmWire::packPacket(1, "", "");
		uint32_t e2 = 0; std::string h2 = "x", b2 = "x";
		CHECK(CmWire::parsePacket(pkt2, e2, h2, b2), "empty hdr/body parses");
		CHECK(e2 == 1 && h2.empty() && b2.empty(), "empty hdr/body slices");

		// Truncated input must fail safely, not crash or overrun.
		CHECK(!CmWire::parsePacket(std::string("\x01\x02", 2), emsg, outHdr, outBody),
		      "short pkt rejected");
		// Header length that runs past the buffer must be rejected.
		std::string lying;
		lying.resize(8);
		uint32_t big = 0xFFFFFFu;
		std::memcpy(&lying[4], &big, 4); // hlen huge, no payload
		CHECK(!CmWire::parsePacket(lying, emsg, outHdr, outBody),
		      "oversized header length rejected");
	}

	// --- Task 2: CMsgMulti inner-packet expansion -------------------------
	{
		// A (decompressed) multi body is a sequence of <u32 len><bytes>.
		const std::string inner1 = "AAA", inner2 = "BBBB";
		std::string blob;
		const uint32_t l1 = 3, l2 = 4;
		blob.append(reinterpret_cast<const char*>(&l1), 4); blob += inner1;
		blob.append(reinterpret_cast<const char*>(&l2), 4); blob += inner2;

		const auto parts = CmWire::expandMultiBody(blob);
		CHECK(parts.size() == 2, "two inner packets");
		CHECK(parts.size() == 2 && parts[0] == "AAA" && parts[1] == "BBBB",
		      "inner slices");

		// An empty body yields no packets (a valid, fully-consumed stream).
		CHECK(CmWire::expandMultiBody("").empty(), "empty multi body");

		// A length prefix that runs past the buffer must reject the whole
		// blob rather than over-read.
		const auto bad = CmWire::expandMultiBody(std::string("\x05\x00\x00\x00""AB", 6));
		CHECK(bad.empty(), "short inner rejected");
	}

	// --- Task 3: build logon/PICS bodies, parse PICS response -------------
	// Only compiled when the project protobufs are linked in (the
	// `make test-cmwire` target defines CMWIRE_PROTOBUF).
#ifdef CMWIRE_PROTOBUF
	{
		// PICS request: two apps, full data (not metadata-only).
		const auto reqBytes = CmWire::buildPicsRequest({3035500u, 250900u});
		CMsgClientPICSProductInfoRequest rt;
		CHECK(rt.ParseFromString(reqBytes), "req parses");
		CHECK(rt.apps_size() == 2, "two apps in request");
		CHECK(rt.apps_size() == 2 && rt.apps(0).appid() == 3035500u &&
		      rt.apps(1).appid() == 250900u, "request appids");
		CHECK(rt.meta_data_only() == false, "meta_data_only false");

		// Anonymous logon body: the documented client identity fields.
		const auto logonBytes = CmWire::buildAnonLogon();
		CMsgClientLogon lt;
		CHECK(lt.ParseFromString(logonBytes), "logon parses");
		CHECK(lt.protocol_version() == 65580u, "logon protocol_version");

		// PICS response parse: one app with a buffer, response_pending set.
		CMsgClientPICSProductInfoResponse resp;
		auto* a = resp.add_apps();
		a->set_appid(3035500u);
		a->set_buffer("X");
		auto* empty = resp.add_apps();
		empty->set_appid(999u); // no buffer -> must be skipped
		resp.set_response_pending(true);
		const std::string respBytes = resp.SerializeAsString();

		std::unordered_map<uint32_t, std::string> out;
		const bool pending = CmWire::parsePicsResponse(respBytes, out);
		CHECK(pending, "response_pending propagated");
		CHECK(out.size() == 1, "only non-empty buffers kept");
		CHECK(out.count(3035500u) && out[3035500u] == "X", "buffer mapped by appid");
		CHECK(out.count(999u) == 0, "empty-buffer app skipped");

		// The optional change-number sink captures each non-empty app's
		// change_number (used as part of the appinfo.vdf idempotency key).
		CMsgClientPICSProductInfoResponse resp2;
		auto* b = resp2.add_apps();
		b->set_appid(3035500u);
		b->set_buffer("X");
		b->set_change_number(4242u);
		std::unordered_map<uint32_t, std::string> out3;
		std::unordered_map<uint32_t, uint32_t> changes;
		CmWire::parsePicsResponse(resp2.SerializeAsString(), out3, &changes);
		CHECK(changes.count(3035500u) && changes[3035500u] == 4242u,
		      "change_number captured");

		// A response with response_pending unset reports no more pending.
		CMsgClientPICSProductInfoResponse done;
		done.set_response_pending(false);
		std::unordered_map<uint32_t, std::string> out2;
		CHECK(!CmWire::parsePicsResponse(done.SerializeAsString(), out2),
		      "no pending when flag unset");
	}
#endif

	if (g_failures == 0) std::printf("\nall cmwire checks passed\n");
	else                 std::printf("\n%d cmwire check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}

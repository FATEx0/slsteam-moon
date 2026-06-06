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

	if (g_failures == 0) std::printf("\nall cmwire checks passed\n");
	else                 std::printf("\n%d cmwire check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}

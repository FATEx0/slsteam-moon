// SPDX-License-Identifier: AGPL-3.0-only
//
// See cmclient.hpp for design notes.
//
// Transport overview
// ------------------
//   1. Fetch the public CM websocket server list (no auth) from
//      ISteamDirectory/GetCMListForConnect.
//   2. TLS-connect to wss://<host>:<port>/cmsocket/ using libcurl's
//      CONNECT_ONLY mode (curl 7.81 routes curl_easy_send/recv through
//      the TLS layer transparently — verified empirically; ALPN is
//      disabled so the server speaks plain HTTP/1.1 for the upgrade).
//   3. Perform the RFC 6455 websocket client handshake by hand.
//   4. Frame/deframe binary messages (client frames masked, FIN+0x2).
//   5. Anonymous ClientLogon (EMsg 5514), capture the assigned steamid +
//      client_sessionid from the ClientLogOnResponse (EMsg 751) header.
//   6. Batched ClientPICSProductInfoRequest (EMsg 8903), loop reading
//      responses (EMsg 8904) until response_pending == false, expanding
//      any EMsg.Multi (1) envelopes (gzip-inflated via `gzip -dc`).

#define CMWIRE_PROTOBUF 1
#include "cmwire.hpp"

#include "cmclient.hpp"

#include "../log.hpp"

#include "../sdk/protobufs/steammessages_base.pb.h"

#include "base64/base64.hpp"

#include <curl/curl.h>
#include <dlfcn.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

namespace CmClient
{

namespace
{

// EMsg values (canonical, cross-checked against the reference enums in
// .kiro/research/.../Enums.h — NOT the mis-transcribed values that
// appeared in an early handoff draft).
constexpr uint32_t EMSG_MULTI                       = 1;
constexpr uint32_t EMSG_CLIENT_LOGON                = 5514;
constexpr uint32_t EMSG_CLIENT_LOGON_RESPONSE       = 751;
constexpr uint32_t EMSG_PICS_PRODUCT_INFO_REQUEST   = 8903;
constexpr uint32_t EMSG_PICS_PRODUCT_INFO_RESPONSE  = 8904;

// Anonymous-user steamid: universe = Public(1), account type =
// AnonUser(10).  Layout: type << 52, universe << 56.
constexpr uint64_t kAnonSteamId =
	(static_cast<uint64_t>(1) << 56) | (static_cast<uint64_t>(10) << 52);

// Overall wall-clock budget for the whole exchange.  Past this we bail
// and the caller falls back to steamcmd.net.
constexpr int kTotalTimeoutSecs = 15;

// ---------------------------------------------------------------------------
// libcurl via dlsym (same portable pattern as ManifestFetch / provision).
// ---------------------------------------------------------------------------

typedef CURL*    (*curl_easy_init_t)();
typedef CURLcode (*curl_easy_setopt_t)(CURL*, CURLoption, ...);
typedef CURLcode (*curl_easy_perform_t)(CURL*);
typedef void     (*curl_easy_cleanup_t)(CURL*);
typedef CURLcode (*curl_easy_getinfo_t)(CURL*, CURLINFO, ...);
typedef CURLcode (*curl_easy_send_t)(CURL*, const void*, size_t, size_t*);
typedef CURLcode (*curl_easy_recv_t)(CURL*, void*, size_t, size_t*);
typedef const char* (*curl_easy_strerror_t)(CURLcode);

curl_easy_init_t     p_init     = nullptr;
curl_easy_setopt_t   p_setopt   = nullptr;
curl_easy_perform_t  p_perform  = nullptr;
curl_easy_cleanup_t  p_cleanup  = nullptr;
curl_easy_getinfo_t  p_getinfo  = nullptr;
curl_easy_send_t     p_send     = nullptr;
curl_easy_recv_t     p_recv     = nullptr;
curl_easy_strerror_t p_strerror = nullptr;

bool loadCurl()
{
	if (p_init) return true;
	void* h = dlopen("libcurl.so.4", RTLD_NOLOAD | RTLD_LAZY);
	if (!h) h = dlopen("libcurl.so.4", RTLD_LAZY);
	if (!h) h = RTLD_DEFAULT;
	p_init     = (curl_easy_init_t)     dlsym(h, "curl_easy_init");
	p_setopt   = (curl_easy_setopt_t)   dlsym(h, "curl_easy_setopt");
	p_perform  = (curl_easy_perform_t)  dlsym(h, "curl_easy_perform");
	p_cleanup  = (curl_easy_cleanup_t)  dlsym(h, "curl_easy_cleanup");
	p_getinfo  = (curl_easy_getinfo_t)  dlsym(h, "curl_easy_getinfo");
	p_send     = (curl_easy_send_t)     dlsym(h, "curl_easy_send");
	p_recv     = (curl_easy_recv_t)     dlsym(h, "curl_easy_recv");
	p_strerror = (curl_easy_strerror_t) dlsym(h, "curl_easy_strerror");
	return p_init && p_setopt && p_perform && p_cleanup &&
	       p_getinfo && p_send && p_recv;
}

std::size_t writeCb(const char* p, std::size_t sz, std::size_t n, std::string* dst)
{
	dst->append(p, sz * n);
	return sz * n;
}

// Plain HTTPS GET (one-shot, not CONNECT_ONLY) for the CM list.
bool httpsGet(const std::string& url, std::string& body)
{
	if (!loadCurl()) return false;
	CURL* c = p_init();
	if (!c) return false;
	body.clear();
	p_setopt(c, CURLOPT_URL, url.c_str());
	p_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	p_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
	p_setopt(c, CURLOPT_WRITEDATA, &body);
	p_setopt(c, CURLOPT_TIMEOUT, 8L);
	p_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
	p_setopt(c, CURLOPT_NOSIGNAL, 1L);
	p_setopt(c, CURLOPT_USERAGENT, "SLSsteam-CmClient/0.1");
	const CURLcode rc = p_perform(c);
	long status = 0;
	p_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	p_cleanup(c);
	return rc == CURLE_OK && status == 200 && !body.empty();
}

// ---------------------------------------------------------------------------
// CM server list (public, no auth).  Parsed via yaml-cpp (JSON ⊂ YAML),
// mirroring appinfo_provision's extractAppNode trick.
// ---------------------------------------------------------------------------

struct Endpoint { std::string host; int port; };

std::vector<Endpoint> fetchCmList()
{
	std::vector<Endpoint> out;
	const std::string url =
		"https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/"
		"?cellid=0&cmtype=websockets&format=json";
	std::string body;
	if (!httpsGet(url, body)) return out;

	// Extract every "endpoint":"host:port" with a lightweight string scan.
	// (Avoids a full JSON/YAML parse of a large, float-heavy document for
	// what is just a flat list of host:port strings.)
	const std::string needle = "\"endpoint\":\"";
	size_t pos = 0;
	while ((pos = body.find(needle, pos)) != std::string::npos)
	{
		pos += needle.size();
		const size_t end = body.find('"', pos);
		if (end == std::string::npos) break;
		const std::string ep = body.substr(pos, end - pos);
		pos = end + 1;
		const auto colon = ep.rfind(':');
		if (colon == std::string::npos) continue;
		Endpoint e;
		e.host = ep.substr(0, colon);
		try { e.port = std::stoi(ep.substr(colon + 1)); }
		catch (...) { continue; }
		if (!e.host.empty() && e.port > 0) out.push_back(std::move(e));
	}
	return out;
}

// ---------------------------------------------------------------------------
// gzip inflate for CMsgMulti via `gzip -dc` (zlib has no -dev in the
// builder; this mirrors ManifestFetch's `unzip` shell-out).  `expected`
// is size_unzipped for a sanity bound.
// ---------------------------------------------------------------------------

bool gzipInflate(const std::string& in, uint32_t expected, std::string& out)
{
	char tmpIn[] = "/tmp/slsteam_cm_gz_XXXXXX";
	int fd = mkstemp(tmpIn);
	if (fd < 0) return false;
	const ssize_t w = write(fd, in.data(), in.size());
	close(fd);
	if (w != static_cast<ssize_t>(in.size())) { unlink(tmpIn); return false; }

	const std::string cmd = "gzip -dc " + std::string(tmpIn) + " 2>/dev/null";
	FILE* pp = popen(cmd.c_str(), "r");
	if (!pp) { unlink(tmpIn); return false; }
	out.clear();
	char buf[8192];
	size_t got;
	// Cap the inflate so a malicious/oversized stream can't exhaust memory.
	const size_t cap = expected ? (static_cast<size_t>(expected) + 64)
	                            : (64u * 1024u * 1024u);
	bool overflow = false;
	while ((got = fread(buf, 1, sizeof(buf), pp)) > 0)
	{
		if (out.size() + got > cap) { overflow = true; break; }
		out.append(buf, got);
	}
	const int rc = pclose(pp);
	unlink(tmpIn);
	if (overflow || rc != 0) { out.clear(); return false; }
	return !out.empty();
}

// ---------------------------------------------------------------------------
// RFC 6455 websocket framing (client side).
// ---------------------------------------------------------------------------

// A small synchronous TLS socket wrapper around curl CONNECT_ONLY.
class TlsSocket
{
public:
	~TlsSocket() { if (m_c) p_cleanup(m_c); }

	bool connect(const Endpoint& ep)
	{
		m_c = p_init();
		if (!m_c) return false;
		// Use an https URL so curl performs the TLS handshake; the path
		// is irrelevant in CONNECT_ONLY mode.
		const std::string url = "https://" + ep.host + ":" +
		                        std::to_string(ep.port) + "/";
		p_setopt(m_c, CURLOPT_URL, url.c_str());
		p_setopt(m_c, CURLOPT_CONNECT_ONLY, 1L);
		// Speak plain HTTP/1.1 for the websocket upgrade — no h2.
		p_setopt(m_c, CURLOPT_SSL_ENABLE_ALPN, 0L);
		p_setopt(m_c, CURLOPT_CONNECTTIMEOUT, 6L);
		p_setopt(m_c, CURLOPT_TIMEOUT, static_cast<long>(kTotalTimeoutSecs));
		p_setopt(m_c, CURLOPT_NOSIGNAL, 1L);
		const CURLcode rc = p_perform(m_c);
		return rc == CURLE_OK;
	}

	// Send all bytes, retrying on CURLE_AGAIN within the deadline.
	bool sendAll(const std::string& data)
	{
		size_t total = 0;
		while (total < data.size())
		{
			if (expired()) return false;
			size_t n = 0;
			const CURLcode rc = p_send(m_c, data.data() + total,
			                           data.size() - total, &n);
			if (rc == CURLE_OK) { total += n; continue; }
			if (rc == CURLE_AGAIN) { usleep(20 * 1000); continue; }
			return false;
		}
		return true;
	}

	// Receive at least one chunk into the internal buffer.  Returns
	// false on a hard error or deadline; true if some bytes arrived (or
	// CURLE_AGAIN was retried — caller re-checks its parse condition).
	bool recvSome()
	{
		char buf[16384];
		size_t n = 0;
		const CURLcode rc = p_recv(m_c, buf, sizeof(buf), &n);
		if (rc == CURLE_OK) { m_rx.append(buf, n); return n > 0; }
		if (rc == CURLE_AGAIN) { usleep(20 * 1000); return true; }
		return false;
	}

	std::string& rx() { return m_rx; }

	void startDeadline()
	{
		m_deadline = std::chrono::steady_clock::now() +
		             std::chrono::seconds(kTotalTimeoutSecs);
	}
	bool expired() const
	{
		return std::chrono::steady_clock::now() >= m_deadline;
	}

private:
	CURL* m_c = nullptr;
	std::string m_rx;
	std::chrono::steady_clock::time_point m_deadline;
};

std::string randomKeyB64()
{
	std::random_device rd;
	std::string raw(16, '\0');
	for (auto& ch : raw) ch = static_cast<char>(rd() & 0xFF);
	return base64::to_base64(raw);
}

// Perform the HTTP/1.1 websocket upgrade.  Reads until the end of the
// response headers; leaves any trailing bytes in the socket rx buffer.
bool wsHandshake(TlsSocket& sock, const Endpoint& ep)
{
	const std::string key = randomKeyB64();
	std::string req;
	req += "GET /cmsocket/ HTTP/1.1\r\n";
	req += "Host: " + ep.host + ":" + std::to_string(ep.port) + "\r\n";
	req += "Upgrade: websocket\r\n";
	req += "Connection: Upgrade\r\n";
	req += "Sec-WebSocket-Key: " + key + "\r\n";
	req += "Sec-WebSocket-Version: 13\r\n";
	req += "\r\n";
	if (!sock.sendAll(req)) return false;

	while (sock.rx().find("\r\n\r\n") == std::string::npos)
	{
		if (sock.expired()) return false;
		if (!sock.recvSome()) return false;
	}
	const auto hdrEnd = sock.rx().find("\r\n\r\n");
	const std::string headers = sock.rx().substr(0, hdrEnd);
	// Status line must be 101.
	if (headers.compare(0, 12, "HTTP/1.1 101") != 0 &&
	    headers.find(" 101 ") == std::string::npos)
	{
		g_pLog->info("CmClient: ws upgrade rejected: %.40s\n", headers.c_str());
		return false;
	}
	// Drop the consumed headers; keep any framed bytes that followed.
	sock.rx().erase(0, hdrEnd + 4);
	return true;
}

// Build a masked client websocket binary frame around `payload`.
std::string wsFrame(const std::string& payload)
{
	std::string f;
	f.push_back(static_cast<char>(0x82)); // FIN + binary opcode
	const size_t len = payload.size();
	uint8_t maskFlag = 0x80;
	if (len <= 125)
	{
		f.push_back(static_cast<char>(maskFlag | len));
	}
	else if (len <= 0xFFFF)
	{
		f.push_back(static_cast<char>(maskFlag | 126));
		f.push_back(static_cast<char>((len >> 8) & 0xFF));
		f.push_back(static_cast<char>(len & 0xFF));
	}
	else
	{
		f.push_back(static_cast<char>(maskFlag | 127));
		for (int i = 7; i >= 0; --i)
			f.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
	}
	uint8_t mask[4];
	std::random_device rd;
	for (auto& m : mask) m = static_cast<uint8_t>(rd() & 0xFF);
	f.append(reinterpret_cast<char*>(mask), 4);
	std::string masked = payload;
	for (size_t i = 0; i < masked.size(); ++i)
		masked[i] = static_cast<char>(masked[i] ^ mask[i % 4]);
	f += masked;
	return f;
}

// Try to extract ONE complete websocket frame's payload from the front
// of `buf`.  On success removes the frame from `buf`, sets `payload` and
// `opcode`, returns true.  Returns false if a full frame isn't buffered
// yet (or on a protocol error, signalled via `error`).
bool wsTryReadFrame(std::string& buf, std::string& payload,
                    uint8_t& opcode, bool& error)
{
	error = false;
	if (buf.size() < 2) return false;
	const uint8_t b0 = static_cast<uint8_t>(buf[0]);
	const uint8_t b1 = static_cast<uint8_t>(buf[1]);
	opcode = b0 & 0x0F;
	const bool masked = (b1 & 0x80) != 0; // server frames are NOT masked
	uint64_t len = b1 & 0x7F;
	size_t pos = 2;
	if (len == 126)
	{
		if (buf.size() < pos + 2) return false;
		len = (static_cast<uint8_t>(buf[pos]) << 8) |
		      static_cast<uint8_t>(buf[pos + 1]);
		pos += 2;
	}
	else if (len == 127)
	{
		if (buf.size() < pos + 8) return false;
		len = 0;
		for (int i = 0; i < 8; ++i)
			len = (len << 8) | static_cast<uint8_t>(buf[pos + i]);
		pos += 8;
	}
	if (masked) pos += 4; // shouldn't happen from server; skip if present
	if (len > (128u * 1024u * 1024u)) { error = true; return false; }
	if (buf.size() < pos + len) return false;
	payload = buf.substr(pos, len);
	buf.erase(0, pos + len);
	return true;
}

// ---------------------------------------------------------------------------
// CM session: logon + batched PICS.
// ---------------------------------------------------------------------------

struct Session
{
	uint64_t steamid   = kAnonSteamId;
	int32_t  sessionid = 0;
	bool     loggedOn  = false;
};

// Build a framed CM packet for `emsg` with the given protobuf body, using
// the current session identity in the header.
std::string buildCmPacket(uint32_t emsg, const std::string& body,
                          const Session& s)
{
	CMsgProtoBufHeader hdr;
	hdr.set_steamid(s.steamid);
	hdr.set_client_sessionid(s.sessionid);
	const std::string hdrBytes = hdr.SerializeAsString();
	return CmWire::packPacket(emsg, hdrBytes, body);
}

// Dispatch one decoded CM packet.  Recurses into Multi.  Updates `out`
// for PICS apps, `session` on logon response, and sets `picsDone` when a
// PICS response with response_pending==false arrives.
void handlePacket(const std::string& pkt, Session& session,
                  std::unordered_map<uint32_t, std::string>& out,
                  bool& gotLogon, bool& picsDone)
{
	uint32_t emsg = 0;
	std::string hdrBytes, body;
	if (!CmWire::parsePacket(pkt, emsg, hdrBytes, body)) return;

	if (emsg == EMSG_MULTI)
	{
		CMsgMulti multi;
		if (!multi.ParseFromString(body)) return;
		std::string payload = multi.message_body();
		if (multi.size_unzipped() > 0)
		{
			std::string inflated;
			if (!gzipInflate(payload, multi.size_unzipped(), inflated)) return;
			payload.swap(inflated);
		}
		for (const auto& inner : CmWire::expandMultiBody(payload))
			handlePacket(inner, session, out, gotLogon, picsDone);
		return;
	}

	// Capture the assigned session identity from the response header.
	if (emsg == EMSG_CLIENT_LOGON_RESPONSE)
	{
		CMsgProtoBufHeader hdr;
		if (hdr.ParseFromString(hdrBytes))
		{
			session.steamid   = hdr.steamid();
			session.sessionid = hdr.client_sessionid();
		}
		CMsgClientLogonResponse resp;
		if (resp.ParseFromString(body))
		{
			gotLogon = (resp.eresult() == 1); // k_EResultOK
			session.loggedOn = gotLogon;
		}
		return;
	}

	if (emsg == EMSG_PICS_PRODUCT_INFO_RESPONSE)
	{
		const bool pending = CmWire::parsePicsResponse(body, out);
		if (!pending) picsDone = true;
		return;
	}
}

// Read+dispatch frames until `predicate` returns true or the deadline
// hits.  Returns false on transport error / timeout.
template <typename Pred>
bool pumpUntil(TlsSocket& sock, Session& session,
               std::unordered_map<uint32_t, std::string>& out,
               bool& gotLogon, bool& picsDone, Pred predicate)
{
	for (;;)
	{
		// Drain any whole frames already buffered.
		for (;;)
		{
			std::string payload; uint8_t opcode = 0; bool err = false;
			if (!wsTryReadFrame(sock.rx(), payload, opcode, err))
			{
				if (err) return false;
				break;
			}
			if (opcode == 0x8) return false;          // close
			if (opcode == 0x9 || opcode == 0xA) continue; // ping/pong: ignore
			if (opcode == 0x2 || opcode == 0x0)
				handlePacket(payload, session, out, gotLogon, picsDone);
		}
		if (predicate()) return true;
		if (sock.expired()) return false;
		if (!sock.recvSome()) return false;
	}
}

bool runSession(const Endpoint& ep, const std::vector<uint32_t>& appids,
                std::unordered_map<uint32_t, std::string>& out)
{
	TlsSocket sock;
	sock.startDeadline();
	if (!sock.connect(ep)) return false;
	if (!wsHandshake(sock, ep)) return false;

	Session session;
	bool gotLogon = false, picsDone = false;

	// Anonymous logon.
	const std::string logonBody = CmWire::buildAnonLogon();
	if (!sock.sendAll(wsFrame(buildCmPacket(EMSG_CLIENT_LOGON, logonBody, session))))
		return false;
	if (!pumpUntil(sock, session, out, gotLogon, picsDone,
	               [&] { return gotLogon; }))
		return false;
	if (!gotLogon) return false;

	// Batched PICS product-info request (echo assigned session identity).
	const std::string picsBody = CmWire::buildPicsRequest(appids);
	if (!sock.sendAll(wsFrame(buildCmPacket(EMSG_PICS_PRODUCT_INFO_REQUEST,
	                                        picsBody, session))))
		return false;
	if (!pumpUntil(sock, session, out, gotLogon, picsDone,
	               [&] { return picsDone; }))
		return false;

	return !out.empty();
}

} // namespace


bool fetchProductInfo(const std::vector<uint32_t>& appids,
                      std::unordered_map<uint32_t, std::string>& out)
{
	if (appids.empty()) return false;
	if (!loadCurl())
	{
		g_pLog->info("CmClient: libcurl unavailable, falling back\n");
		return false;
	}

	const auto cms = fetchCmList();
	if (cms.empty())
	{
		g_pLog->info("CmClient: empty CM list, falling back\n");
		return false;
	}

	// Try a few CMs before giving up; a single edge may refuse/drop.
	const size_t maxTries = std::min<size_t>(cms.size(), 4);
	for (size_t i = 0; i < maxTries; ++i)
	{
		out.clear();
		const auto& ep = cms[i];
		g_pLog->info("CmClient: trying CM %s:%d (%zu apps)\n",
		             ep.host.c_str(), ep.port, appids.size());
		if (runSession(ep, appids, out))
		{
			g_pLog->info("CmClient: fetched %zu/%zu apps via %s\n",
			             out.size(), appids.size(), ep.host.c_str());
			return true;
		}
	}
	out.clear();
	g_pLog->info("CmClient: all CM attempts failed, falling back\n");
	return false;
}

} // namespace CmClient

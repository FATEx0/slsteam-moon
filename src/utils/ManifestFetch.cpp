
#include "ManifestFetch.hpp"

#include "../config.hpp"
#include "../log.hpp"

#include <curl/curl.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>


namespace ManifestFetch
{

namespace
{

const std::vector<std::string>& providerChain()
{
	static const std::vector<std::string> chain = {
		"http://gmrc.wudrm.com/manifest/{gid}",
		"https://manifest.steam.run/api/manifest/{gid}",
	};
	return chain;
}


std::mutex g_lock;
std::map<uint64_t, std::shared_future<std::optional<uint64_t>>> g_pending;


bool parseDigitsOnly(std::string_view body, uint64_t* out)
{
	if (body.empty()) return false;
	std::size_t b = 0, e = body.size();
	auto isWs = [](char c)
	{
		return c == ' ' || c == '\r' || c == '\n' || c == '\t';
	};
	while (b < e && isWs(body[b])) ++b;
	while (e > b && isWs(body[e - 1])) --e;
	if (b == e) return false;
	for (std::size_t i = b; i < e; ++i)
	{
		if (body[i] < '0' || body[i] > '9') return false;
	}
	uint64_t v = 0;
	auto [_, ec] = std::from_chars(body.data() + b, body.data() + e, v);
	if (ec != std::errc{}) return false;
	*out = v;
	return true;
}

bool parseJsonDigitField(std::string_view body, uint64_t* out)
{
	static constexpr std::string_view kKeys[] =
	{
		"\"manifest_request_code\"",
		"\"content\"",
		"\"code\"",
	};
	for (auto key : kKeys)
	{
		auto k = body.find(key);
		if (k == std::string_view::npos) continue;
		auto q1 = body.find('"', k + key.size());
		if (q1 == std::string_view::npos) continue;
		auto q2 = body.find('"', q1 + 1);
		if (q2 == std::string_view::npos) continue;
		if (parseDigitsOnly(body.substr(q1 + 1, q2 - q1 - 1), out))
		{
			return true;
		}
	}
	return false;
}

std::string expandTemplate(std::string_view tmpl,
                           uint64_t gid, uint32_t appId, uint32_t depotId)
{
	std::string out;
	out.reserve(tmpl.size() + 32);
	for (std::size_t i = 0; i < tmpl.size();)
	{
		if (tmpl[i] != '{') { out.push_back(tmpl[i++]); continue; }
		auto end = tmpl.find('}', i + 1);
		if (end == std::string_view::npos) { out.push_back(tmpl[i++]); continue; }
		auto tag = tmpl.substr(i + 1, end - i - 1);
		if (tag == "gid")          out += std::to_string(gid);
		else if (tag == "appid")   out += std::to_string(appId);
		else if (tag == "depotid") out += std::to_string(depotId);
		else                       out.append(tmpl.substr(i, end - i + 1));
		i = end + 1;
	}
	return out;
}

struct HttpResponse
{
	long status         = 0;
	std::string body;
	bool networkError   = false;
	std::string diagnostic;
};

std::size_t curlWriteCb(const char* p, std::size_t sz, std::size_t n, std::string* dst)
{
	dst->append(p, sz * n);
	return sz * n;
}

#include <dlfcn.h>

typedef CURL* (*curl_easy_init_t)();
typedef CURLcode (*curl_easy_setopt_t)(CURL *curl, CURLoption option, ...);
typedef CURLcode (*curl_easy_perform_t)(CURL *curl);
typedef void (*curl_easy_cleanup_t)(CURL *curl);
typedef CURLcode (*curl_easy_getinfo_t)(CURL *curl, CURLINFO info, ...);
typedef const char* (*curl_easy_strerror_t)(CURLcode);

static curl_easy_init_t p_curl_easy_init = nullptr;
static curl_easy_setopt_t p_curl_easy_setopt = nullptr;
static curl_easy_perform_t p_curl_easy_perform = nullptr;
static curl_easy_cleanup_t p_curl_easy_cleanup = nullptr;
static curl_easy_getinfo_t p_curl_easy_getinfo = nullptr;
static curl_easy_strerror_t p_curl_easy_strerror = nullptr;

static bool load_curl() {
	if (p_curl_easy_init) return true;

	void* handle = dlopen("libcurl.so.4", RTLD_NOLOAD | RTLD_LAZY);
	if (!handle) handle = dlopen("libcurl.so.4", RTLD_LAZY);
	if (!handle) handle = RTLD_DEFAULT;

	p_curl_easy_init = (curl_easy_init_t)dlsym(handle, "curl_easy_init");
	p_curl_easy_setopt = (curl_easy_setopt_t)dlsym(handle, "curl_easy_setopt");
	p_curl_easy_perform = (curl_easy_perform_t)dlsym(handle, "curl_easy_perform");
	p_curl_easy_cleanup = (curl_easy_cleanup_t)dlsym(handle, "curl_easy_cleanup");
	p_curl_easy_getinfo = (curl_easy_getinfo_t)dlsym(handle, "curl_easy_getinfo");
	p_curl_easy_strerror = (curl_easy_strerror_t)dlsym(handle, "curl_easy_strerror");

	return p_curl_easy_init && p_curl_easy_setopt && p_curl_easy_perform && p_curl_easy_cleanup;
}

HttpResponse httpGet(const std::string& url)
{
    HttpResponse r;
    
    if (!load_curl())
    {
            r.networkError = true;
            r.diagnostic = "failed to load libcurl dynamically";
            return r;
    }

    CURL* c = p_curl_easy_init();
    if (!c)
    {
            r.networkError = true;
            r.diagnostic = "curl_easy_init failed";
            return r;
    }
    p_curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    p_curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    p_curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteCb);
    p_curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    p_curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    p_curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    // MANDATORY for multi-threaded use: httpGet runs on a ManifestFetch
    // worker thread.  Without CURLOPT_NOSIGNAL, libcurl built with a
    // synchronous resolver implements timeouts via SIGALRM + siglongjmp.
    // That handler is process-wide; if SIGALRM fires while another thread
    // (e.g. Steam's main thread in poll()) is running, the longjmp targets
    // the wrong stack and glibc's __longjmp_chk aborts the whole client.
    // NOSIGNAL switches libcurl to signal-free timeouts.
    p_curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    p_curl_easy_setopt(c, CURLOPT_USERAGENT, "SLSsteam-ManifestFetch/0.1");
    const CURLcode rc = p_curl_easy_perform(c);
    if (rc != CURLE_OK)
    {
            r.networkError = true;
            r.diagnostic = p_curl_easy_strerror ? p_curl_easy_strerror(rc) : "curl error";
    }
    else
    {
            r.networkError = false;
            r.diagnostic = "OK";
            if (p_curl_easy_getinfo) p_curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    }
    p_curl_easy_cleanup(c);
    return r;
}

std::optional<uint64_t> runOnce(uint64_t gid, uint32_t appId, uint32_t depotId)
{
	const auto& chain = providerChain();
	if (chain.empty())
	{
		g_pLog->debug("ManifestFetch: gid=%llu skipped, no providers configured\n",
		              static_cast<unsigned long long>(gid));
		return std::nullopt;
	}

	for (std::size_t i = 0; i < chain.size(); ++i)
	{
		const auto& tmpl = chain[i];
		if (tmpl.empty()) continue;
		const auto url = expandTemplate(tmpl, gid, appId, depotId);
		g_pLog->info("ManifestFetch: gid=%llu provider %zu/%zu GET %s\n",
		             static_cast<unsigned long long>(gid),
		             i + 1, chain.size(), url.c_str());

		const auto resp = httpGet(url);
		if (resp.networkError)
		{
			g_pLog->warn("ManifestFetch: gid=%llu provider %zu net err '%s', trying next\n",
			             static_cast<unsigned long long>(gid),
			             i + 1, resp.diagnostic.c_str());
			continue;
		}
		if (resp.status != 200)
		{
			g_pLog->warn("ManifestFetch: gid=%llu provider %zu HTTP=%ld body_bytes=%zu, trying next\n",
			             static_cast<unsigned long long>(gid),
			             i + 1, resp.status, resp.body.size());
			continue;
		}
		uint64_t code = 0;
		if (parseDigitsOnly(resp.body, &code) || parseJsonDigitField(resp.body, &code))
		{
			g_pLog->info("ManifestFetch: gid=%llu resolved code=%llu via provider %zu\n",
			             static_cast<unsigned long long>(gid),
			             static_cast<unsigned long long>(code),
			             i + 1);
			return code;
		}
		g_pLog->warn("ManifestFetch: gid=%llu provider %zu body unparseable (first 64: '%.*s'), trying next\n",
		             static_cast<unsigned long long>(gid),
		             i + 1,
		             static_cast<int>(std::min<std::size_t>(resp.body.size(), 64)),
		             resp.body.c_str());
	}

	g_pLog->warn("ManifestFetch: gid=%llu all %zu providers exhausted\n",
	             static_cast<unsigned long long>(gid), chain.size());
	return std::nullopt;
}

} // namespace


namespace
{

std::string findSteamRootForBlob()
{
	const char* home = std::getenv("HOME");
	if (!home) return {};
	const std::vector<std::string> candidates = {
		std::string(home) + "/.steam/steam",
		std::string(home) + "/.steam/debian-installation",
		std::string(home) + "/.local/share/Steam",
	};
	for (const auto& candidate : candidates)
	{
		struct stat st{};
		if (stat((candidate + "/steam.sh").c_str(), &st) == 0)
		{
			return candidate;
		}
	}
	return {};
}

bool fetchManifestBlob(uint64_t gid, uint32_t depotId, const std::string& depotcacheDir)
{
	std::string targetPath = depotcacheDir + "/" + std::to_string(depotId)
	                          + "_" + std::to_string(gid) + ".manifest";
	{
		struct stat st{};
		if (stat(targetPath.c_str(), &st) == 0 && st.st_size > 0)
		{
			g_pLog->debug("ManifestFetch: blob depot=%u gid=%llu already at %s\n",
			              depotId,
			              static_cast<unsigned long long>(gid),
			              targetPath.c_str());
			return true;
		}
	}

	auto codeOpt = runOnce(gid, /*appId=*/0, depotId);
	if (!codeOpt)
	{
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu request-code lookup failed\n",
		             depotId, static_cast<unsigned long long>(gid));
		return false;
	}
	const uint64_t code = *codeOpt;

	const std::string cdnUrl = "http://cache1-gru1.steamcontent.com/depot/"
	                           + std::to_string(depotId) + "/manifest/"
	                           + std::to_string(gid) + "/5/"
	                           + std::to_string(code);

	const auto zipResp = httpGet(cdnUrl);
	if (zipResp.networkError || zipResp.status != 200 || zipResp.body.empty())
	{
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu CDN HTTP=%ld err='%s'\n",
		             depotId, static_cast<unsigned long long>(gid),
		             zipResp.status, zipResp.diagnostic.c_str());
		return false;
	}

	char tmpZip[]  = "/tmp/slsteam_mfetch_zip_XXXXXX";
	int tmpZipFd = mkstemp(tmpZip);
	if (tmpZipFd < 0)
	{
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu mkstemp failed\n",
		             depotId, static_cast<unsigned long long>(gid));
		return false;
	}
	const ssize_t written =
	    write(tmpZipFd, zipResp.body.data(), zipResp.body.size());
	close(tmpZipFd);
	if (written != static_cast<ssize_t>(zipResp.body.size()))
	{
		unlink(tmpZip);
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu zip write short\n",
		             depotId, static_cast<unsigned long long>(gid));
		return false;
	}

	const std::string tmpOutPath = targetPath + ".slsteam_tmp";
	const std::string cmd =
	    "unzip -p " + std::string(tmpZip) + " > " + tmpOutPath + " 2>/dev/null";
	const int rc = std::system(cmd.c_str());
	unlink(tmpZip);
	if (rc != 0)
	{
		unlink(tmpOutPath.c_str());
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu unzip rc=%d\n",
		             depotId, static_cast<unsigned long long>(gid), rc);
		return false;
	}

	std::ifstream verify(tmpOutPath, std::ios::binary);
	uint32_t magic = 0;
	verify.read(reinterpret_cast<char*>(&magic), sizeof(magic));
	verify.close();
	if (magic != 0x71F617D0u)
	{
		unlink(tmpOutPath.c_str());
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu bad magic 0x%x\n",
		             depotId, static_cast<unsigned long long>(gid), magic);
		return false;
	}

	if (rename(tmpOutPath.c_str(), targetPath.c_str()) != 0)
	{
		unlink(tmpOutPath.c_str());
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu rename failed errno=%d\n",
		             depotId, static_cast<unsigned long long>(gid), errno);
		return false;
	}

	g_pLog->info("ManifestFetch: blob depot=%u gid=%llu wrote %s\n",
	             depotId, static_cast<unsigned long long>(gid),
	             targetPath.c_str());
	return true;
}

} // namespace

void submitManifestBlob(uint64_t manifestGid, uint32_t /*appId*/, uint32_t depotId)
{
	const auto steamRoot = findSteamRootForBlob();
	if (steamRoot.empty())
	{
		g_pLog->debug("ManifestFetch: blob depot=%u gid=%llu skip, no Steam root\n",
		              depotId,
		              static_cast<unsigned long long>(manifestGid));
		return;
	}
	const std::string depotcacheDir = steamRoot + "/depotcache";

	std::thread([gid = manifestGid, depotId, depotcacheDir]() {
		fetchManifestBlob(gid, depotId, depotcacheDir);
	}).detach();
}

bool fetchManifestBlobSync(uint64_t manifestGid, uint32_t depotId)
{
	const auto steamRoot = findSteamRootForBlob();
	if (steamRoot.empty()) return false;
	const std::string depotcacheDir = steamRoot + "/depotcache";
	return fetchManifestBlob(manifestGid, depotId, depotcacheDir);
}



int getTimeoutSec()
{
	return 12;
}

const char* defaultTimeoutKey()
{
	return "ManifestFetch.timeout_sec";
}

void submit(uint64_t jobId, uint64_t manifestGid, uint32_t appId, uint32_t depotId)
{
	std::lock_guard<std::mutex> lk(g_lock);
	if (g_pending.count(jobId))
	{
		g_pLog->debug("ManifestFetch: duplicate submit for jobId=%llu\n",
		              static_cast<unsigned long long>(jobId));
		return;
	}
	auto fut = std::async(std::launch::async,
	                      [manifestGid, appId, depotId]() -> std::optional<uint64_t>
	                      {
	                          return runOnce(manifestGid, appId, depotId);
	                      });
	g_pending.emplace(jobId, fut.share());
}

std::optional<uint64_t> resolve(uint64_t jobId)
{
	std::shared_future<std::optional<uint64_t>> fut;
	{
		std::lock_guard<std::mutex> lk(g_lock);
		auto it = g_pending.find(jobId);
		if (it == g_pending.end()) return std::nullopt;
		fut = it->second;
		g_pending.erase(it);
	}
	const int budget = getTimeoutSec() > 0 ? getTimeoutSec() : 12;
	if (fut.wait_for(std::chrono::seconds(budget)) != std::future_status::ready)
	{
		g_pLog->warn("ManifestFetch: jobId=%llu timed out after %ds\n",
		             static_cast<unsigned long long>(jobId), budget);
		return std::nullopt;
	}
	return fut.get();
}

void discard(uint64_t jobId)
{
	std::lock_guard<std::mutex> lk(g_lock);
	g_pending.erase(jobId);
}

} // namespace ManifestFetch

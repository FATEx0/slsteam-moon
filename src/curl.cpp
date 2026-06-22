#include "curl.hpp"
#include "cainfo.hpp"
#include "log.hpp"

#include <curl/curl.h>
#include <curl/easy.h>
#include <dlfcn.h>

typedef CURL* (*curl_easy_init_t)();
typedef CURLcode (*curl_easy_setopt_t)(CURL *curl, CURLoption option, ...);
typedef CURLcode (*curl_easy_perform_t)(CURL *curl);
typedef void (*curl_easy_cleanup_t)(CURL *curl);

static curl_easy_init_t p_curl_easy_init = nullptr;
static curl_easy_setopt_t p_curl_easy_setopt = nullptr;
static curl_easy_perform_t p_curl_easy_perform = nullptr;
static curl_easy_cleanup_t p_curl_easy_cleanup = nullptr;

static bool load_curl() {
	if (p_curl_easy_init) return true;

	void* handle = dlopen("libcurl.so.4", RTLD_NOLOAD | RTLD_LAZY);
	if (!handle) handle = dlopen("libcurl.so.4", RTLD_LAZY);
	if (!handle) handle = RTLD_DEFAULT;

	p_curl_easy_init = (curl_easy_init_t)dlsym(handle, "curl_easy_init");
	p_curl_easy_setopt = (curl_easy_setopt_t)dlsym(handle, "curl_easy_setopt");
	p_curl_easy_perform = (curl_easy_perform_t)dlsym(handle, "curl_easy_perform");
	p_curl_easy_cleanup = (curl_easy_cleanup_t)dlsym(handle, "curl_easy_cleanup");

	return p_curl_easy_init && p_curl_easy_setopt && p_curl_easy_perform && p_curl_easy_cleanup;
}

static size_t writeCallback(const char* content, size_t size, size_t memberSize, std::string* data)
{
	data->append(content, size * memberSize);
	return size * memberSize;
}

int Curl::getString(const char* url, std::string& out)
{
	if (!load_curl()) return -1;

	CURL* curl = p_curl_easy_init();
	if (!curl) return -1;

	p_curl_easy_setopt(curl, CURLOPT_URL, url);
	p_curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
	p_curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
	// Pin the system trust store so TLS verification works even when the
	// libcurl Steam loads can't find a CA bundle on its compiled-in default
	// (SteamOS/Arch). No-op when nothing is found -> curl keeps its default.
	if (const char* f = ca::bundleFile()) p_curl_easy_setopt(curl, CURLOPT_CAINFO, f);
	if (const char* d = ca::bundleDir())  p_curl_easy_setopt(curl, CURLOPT_CAPATH, d);
	// Signal-free timeouts: this can be called off the main thread, and
	// libcurl's default SIGALRM/siglongjmp timeout path is not thread-safe
	// (it aborts via __longjmp_chk when the signal lands on another
	// thread's stack).  See ManifestFetch::httpGet for the full rationale.
	p_curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	auto res = p_curl_easy_perform(curl);

	p_curl_easy_cleanup(curl);

	return res;
}

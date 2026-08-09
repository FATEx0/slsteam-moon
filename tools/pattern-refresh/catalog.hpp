#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>


namespace PatternRefresh
{
	inline constexpr std::size_t kMaximumCatalogSize = 256 * 1024;
	inline constexpr std::size_t kSignatureSize = 64;
	using PublicKey = std::array<unsigned char, 32>;

	struct ExecutableRange
	{
		std::uint64_t begin = 0;
		std::uint64_t end = 0;
	};

	struct ExactModule
	{
		std::string component;
		std::string moduleName;
		std::string sha256;
		std::uint64_t size = 0;
		std::string gnuBuildId;
		std::vector<ExecutableRange> executableRanges;
	};

	struct ValidatedCatalog
	{
		std::uint64_t revision = 0;
	};

	struct InspectedModule
	{
		ExactModule identity;
		bool fromStatCache = false;
	};

	struct ActivationResult
	{
		bool active = false;
		bool changed = false;
		std::uint64_t revision = 0;
	};

	struct MirrorBases
	{
		std::string github =
			"https://raw.githubusercontent.com/swwayps/steam-monitor/main/";
		std::string jsDelivr =
			"https://cdn.jsdelivr.net/gh/swwayps/steam-monitor@main/";
		bool allowHttpForTests = false;
	};

	enum class Mirror
	{
		GitHub,
		JsDelivr,
	};

	struct UrlPair
	{
		std::string catalog;
		std::string signature;
	};

	struct Response
	{
		bool transportOk = false;
		long status = 0;
		std::string body;
		std::string etag;
	};

	struct ResponsePair
	{
		Response catalog;
		Response signature;
	};

	struct CachedPair
	{
		std::string catalog;
		std::string signature;
		bool exactSha = false;
	};

	enum class Source
	{
		None,
		GitHub,
		JsDelivr,
		RevalidatedCache,
		OfflineCache,
	};

	enum class RefreshMode
	{
		Remote,
		CacheOnly,
	};

	struct RefreshResult
	{
		bool active = false;
		bool changed = false;
		Source source = Source::None;
		std::uint64_t revision = 0;
	};

	struct Invocation
	{
		std::filesystem::path steamRoot;
		std::filesystem::path configRoot;
		RefreshMode mode = RefreshMode::Remote;
	};

	struct Selection
	{
		Source source = Source::None;
		std::string catalog;
		std::string signature;

		explicit operator bool() const noexcept { return source != Source::None; }
	};

	using Validator = std::function<bool(std::string_view, std::string_view)>;

	UrlPair makeUrls(Mirror mirror, std::string_view component, std::string_view sha256);
	UrlPair makeUrls(
		std::string_view base,
		std::string_view component,
		std::string_view sha256
	);

	Selection chooseCandidate(
		const ResponsePair& github,
		const ResponsePair& jsDelivr,
		const std::optional<CachedPair>& cache,
		const Validator& validate
	);

	std::optional<ValidatedCatalog> validateSignedCatalog(
		std::string_view body,
		std::span<const unsigned char> signature,
		const ExactModule& module,
		const PublicKey& publicKey,
		std::string* error = nullptr
	);

	bool acceptsRevision(
		const ValidatedCatalog& candidate,
		std::string_view candidateBody,
		const std::optional<ValidatedCatalog>& active,
		std::string_view activeBody
	) noexcept;

	std::optional<InspectedModule> inspectModule(
		std::string component,
		std::string moduleName,
		const std::filesystem::path& modulePath,
		const std::filesystem::path& statePath,
		const std::function<void()>& beforeRestat = {},
		std::string* error = nullptr
	);

	ActivationResult activateSignedCatalog(
		const std::filesystem::path& patternRoot,
		const ExactModule& module,
		std::string_view body,
		std::span<const unsigned char> signature,
		const PublicKey& publicKey,
		std::string* error = nullptr
	);

	RefreshResult refreshComponent(
		const std::filesystem::path& patternRoot,
		const ExactModule& module,
		const PublicKey& publicKey,
		RefreshMode mode = RefreshMode::Remote,
		const MirrorBases& mirrors = {},
		std::string* error = nullptr
	);

	std::optional<PublicKey> parsePublicKeyHex(std::string_view value) noexcept;
	std::optional<Invocation> parseInvocation(
		const std::vector<std::string_view>& arguments
	);

	int refreshInstallation(
		const std::filesystem::path& steamRoot,
		const std::filesystem::path& configRoot,
		const PublicKey& publicKey,
		RefreshMode mode = RefreshMode::Remote,
		const MirrorBases& mirrors = {},
		std::string* error = nullptr
	);
}

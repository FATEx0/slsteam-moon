
#include "pics.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"

#include "manifestid.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/emitter.h"
#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>

namespace PICS
{

namespace
{

std::string getCacheDir()
{
	std::stringstream ss;
	ss << g_config.getDir() << "/cache";
	const auto dir = ss.str();
	if (!std::filesystem::exists(dir))
	{
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
	}
	return dir;
}

std::string getBufferPath(uint32_t appId)
{
	std::stringstream ss;
	ss << getCacheDir() << "/picsbuffer_" << appId << ".bin";
	return ss.str();
}

std::string getMetaPath(uint32_t appId)
{
	std::stringstream ss;
	ss << getCacheDir() << "/picsbuffer_" << appId << ".yaml";
	return ss.str();
}

bool persistAppBuffer(uint32_t appId, uint32_t changeNumber,
                      const std::string& sha, const std::string& buffer)
{
	if (buffer.empty()) return false;
	if (sha.size() != 20)
	{
		g_pLog->debug("PICS: refusing to persist app=%u (bad sha size %zu)\n",
		              appId, sha.size());
		return false;
	}

	const auto bufPath = getBufferPath(appId);
	const auto metaPath = getMetaPath(appId);

	if (std::filesystem::exists(metaPath) && std::filesystem::exists(bufPath))
	{
		try
		{
			auto node = YAML::LoadFile(metaPath);
			const auto cachedChange = node["change_number"].as<uint32_t>();
			const auto cachedSize = node["wire_size"].as<size_t>();
			if (cachedChange == changeNumber && cachedSize == buffer.size())
			{
				return true;
			}
		}
		catch (...) { /* fall through to rewrite */ }
	}

	{
		std::ofstream ofs(bufPath, std::ios::binary | std::ios::trunc);
		if (!ofs.is_open())
		{
			g_pLog->debug("PICS: cannot write %s\n", bufPath.c_str());
			return false;
		}
		ofs.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
	}

	{
		YAML::Emitter em;
		em << YAML::BeginMap;
		em << YAML::Key << "appid"          << YAML::Value << appId;
		em << YAML::Key << "change_number"  << YAML::Value << changeNumber;
		em << YAML::Key << "wire_size"      << YAML::Value << buffer.size();
		em << YAML::Key << "sha_b64"        << YAML::Value << base64::to_base64(sha);
		em << YAML::EndMap;

		std::ofstream ofs(metaPath, std::ios::trunc);
		if (!ofs.is_open())
		{
			g_pLog->debug("PICS: cannot write %s\n", metaPath.c_str());
			return false;
		}
		ofs.write(em.c_str(), em.size());
	}

	g_pLog->debug("PICS: cached app=%u change=%u buffer=%zu bytes -> %s\n",
	              appId, changeNumber, buffer.size(), bufPath.c_str());
	return true;
}

} // namespace

void recvProductInfoResponse(CMsgClientPICSProductInfoResponse* resp)
{
	if (!resp) return;

	g_pLog->debug
	(
		"PICS: response apps=%d packages=%d unknown_apps=%d unknown_packages=%d meta_only=%i\n",
		resp->apps_size(),
		resp->packages_size(),
		resp->unknown_appids_size(),
		resp->unknown_packageids_size(),
		resp->meta_data_only() ? 1 : 0
	);

	if (!resp->meta_data_only())
	{
		resp->set_meta_data_only(true);
		g_pLog->debug("PICS: forced response.meta_data_only=true to satisfy client-side assert\n");
	}

	const auto added = g_config.addedAppIds.get();
	for (int i = 0; i < resp->apps_size(); ++i)
	{
		auto* app = resp->mutable_apps(i);
		g_pLog->debug
		(
			"PICS: app=%u change=%u missing_token=%i only_public=%i sha_size=%zu buffer_size=%zu\n",
			app->appid(),
			app->change_number(),
			app->missing_token() ? 1 : 0,
			app->only_public() ? 1 : 0,
			app->sha().size(),
			app->buffer().size()
		);

		if (added.count(app->appid()) && app->buffer().size() > 0)
		{
			std::string pinned = ManifestId::applyToWireBuffer(app->buffer());
			if (pinned.size() != app->buffer().size() || pinned != app->buffer())
			{
				app->set_buffer(pinned);
			}
			persistAppBuffer(app->appid(), app->change_number(),
			                 app->sha(), app->buffer());
		}
	}

	if (resp->unknown_appids_size() > 0)
	{
		std::stringstream ss;
		for (int i = 0; i < resp->unknown_appids_size(); ++i)
		{
			if (i) ss << ',';
			ss << resp->unknown_appids(i);
		}
		g_pLog->debug("PICS: unknown_appids=[%s]\n", ss.str().c_str());
	}
}

void recvMsg(CProtoBufMsgBase* msg)
{
	if (!msg) return;
	switch (msg->type)
	{
		case EMSG_PICS_PRODUCTINFO_RESPONSE:
			recvProductInfoResponse(msg->getBody<CMsgClientPICSProductInfoResponse>());
			break;
		default:
			break;
	}
}

} // namespace PICS

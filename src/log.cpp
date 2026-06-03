#include "log.hpp"

#include "config.hpp"

#include <cstdlib>
#include <memory>

CLog::CLog(const char* path) : path(path)
{
	// Append (not truncate) so a complete record survives even if the
	// logger is constructed more than once in a session.  This is also
	// a diagnostic safety net: truncation was hiding the init phase.
	ofstream = std::ofstream(path, std::ios::out | std::ios::app);
	if (!ofstream.is_open())
	{
		throw std::runtime_error("Unable to open logfile!");
	}
}

CLog::~CLog()
{
	if (ofstream.is_open())
	{
		ofstream.close();
	}
}

//Dirty workaround for not being able to access g_config from __log
LogLevel CLog::getMinLevel()
{
	return static_cast<LogLevel>(g_config.logLevel.get());
}

bool CLog::shouldNotify()
{
	return g_config.notifications.get();
}

CLog* CLog::createDefaultLog()
{
	const char* home = getenv("HOME");
	if (home)
	{
		std::stringstream ss;
		ss << home << "/.SLSsteam.log";

		return new CLog(ss.str().c_str());
	}

	return nullptr;
}

std::unique_ptr<CLog> g_pLog;

#pragma once

// Pure, dependency-free desktop-notification command builder.
//
// Split out of log.hpp so the notify-send mapping is unit-testable without
// dragging in openssl / config / the rest of the logger.  Owns the single
// definition of LogLevel (log.hpp includes this header for it).
//
// HANDOFF 2026-06-05 fix: the old Warn branch emitted
// `notify-send -u "critical"` with NO `-t`.  freedesktop urgency=critical
// notifications are "resident" — GNOME/Zorin's daemon ignores the expire
// timeout for them, so every warn-level popup stayed on screen forever.
// Here every notifying level gets an explicit timeout AND a non-critical
// urgency, so they all auto-dismiss.

#include <string>

enum class LogLevel : unsigned int
{
	//TODO: Add Trace without breaking configs and without using -1 for Once
	Once,
	Debug,
	Info,
	NotifyShort,
	NotifyLong,
	Warn,
	None
};

namespace Notify
{
	struct Spec
	{
		bool        enabled;   // does this level raise a desktop popup?
		int         timeoutMs; // notify-send -t (auto-dismiss after this)
		const char* urgency;   // notify-send -u; never "critical" (resident)
	};

	// Maps a log level to how its desktop notification should behave.
	// Non-notifying levels return {enabled=false}.
	inline Spec specForLevel(LogLevel lvl)
	{
		switch (lvl)
		{
			case LogLevel::NotifyShort: return { true, 10000, "normal" };
			case LogLevel::NotifyLong:  return { true, 30000, "normal" };
			// Warn used to be urgency=critical with no timeout, so it never
			// auto-dismissed.  Give it the long timeout + normal urgency so
			// it behaves like the other popups.
			case LogLevel::Warn:        return { true, 30000, "normal" };
			default:                    return { false, 0, "normal" };
		}
	}

	// Escape a string for safe inclusion inside a double-quoted shell word.
	// Without this a stray `"`, `$`, or backtick in a log message could break
	// the system() command or inject into the shell (fixes the old TODO in
	// log.hpp about breakage on a single `"`).
	inline std::string shellEscapeDoubleQuoted(const std::string& s)
	{
		std::string out;
		out.reserve(s.size() + 8);
		for (char c : s)
		{
			// Drop embedded NULs.  __log builds its message with
			// snprintf(size)+1 then resize(size), so the std::string it
			// hands us carries a trailing '\0'.  If we copied that into the
			// command, system()'s c_str() would truncate there and lose the
			// closing quote -> shell syntax error -> no notification.
			if (c == '\0')
			{
				continue;
			}
			if (c == '\\' || c == '"' || c == '$' || c == '`')
			{
				out.push_back('\\');
			}
			out.push_back(c);
		}
		return out;
	}

	// Build the full notify-send command for a level, or "" for a
	// non-notifying level (caller then skips system()).
	inline std::string buildCommand(LogLevel lvl, const std::string& body)
	{
		const Spec s = specForLevel(lvl);
		if (!s.enabled)
		{
			return {};
		}

		std::string cmd = "notify-send -t ";
		cmd += std::to_string(s.timeoutMs);
		cmd += " -u \"";
		cmd += s.urgency;
		cmd += "\" \"SLSsteam-moon\" \"";
		cmd += shellEscapeDoubleQuoted(body);
		cmd += "\"";
		return cmd;
	}
}

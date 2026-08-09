#pragma once

#include <string>

namespace ConfigPath
{

// Resolve the directory shared by runtime configuration and cache artifacts.
// XDG_CONFIG_HOME follows the XDG convention: an unset or empty value falls
// back to HOME/.config rather than producing a root-level /SLSsteam path.
inline std::string slsteamConfigDir(const char* xdgConfigHome,
                                    const char* home)
{
	if (xdgConfigHome != nullptr && *xdgConfigHome != '\0')
		return std::string(xdgConfigHome) + "/SLSsteam";
	return std::string(home != nullptr ? home : "") + "/.config/SLSsteam";
}

} // namespace ConfigPath

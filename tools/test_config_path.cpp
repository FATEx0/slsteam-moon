// Standalone regression test for the canonical SLSsteam config directory.
//
// The runtime config reader and cache lock must resolve XDG_CONFIG_HOME in
// exactly the same way. In particular, an empty XDG_CONFIG_HOME is treated
// like an unset value and falls back to $HOME/.config, not /SLSsteam.

#include "../src/config_path.hpp"

#include <cstdio>

static int g_failures = 0;

static void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
    else
    {
        std::printf("ok:   %s\n", message);
    }
}

int main()
{
    using ConfigPath::slsteamConfigDir;

    check(slsteamConfigDir(nullptr, "/home/tester") ==
              "/home/tester/.config/SLSsteam",
          "unset XDG_CONFIG_HOME uses the HOME config directory");
    check(slsteamConfigDir("", "/home/tester") ==
              "/home/tester/.config/SLSsteam",
          "empty XDG_CONFIG_HOME uses the HOME config directory");
    check(slsteamConfigDir("/tmp/xdg", "/home/tester") ==
              "/tmp/xdg/SLSsteam",
          "nonempty XDG_CONFIG_HOME is used as the config directory base");

    if (g_failures == 0) std::puts("all config-path checks passed");
    else                 std::printf("%d config-path check(s) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

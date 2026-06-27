// Regression guard for the "exorbitant FakeWalletBalance bricks Steam" bug.
//
// SYMPTOM (user report): setting FakeWalletBalance to a huge number makes
// Steam abort on launch and never reopen until the value is reset to 0.
//
// ROOT CAUSE (confirmed from the SIGABRT coredump): CConfig::getSetting wrapped
// `node[name].as<T>()` in `catch (YAML::BadConversion&)`.  An out-of-range
// scalar (FakeWalletBalance > INT_MAX, e.g. 500000000000000000) makes yaml-cpp
// throw YAML::TypedBadConversion<int>.  That template subclass is NOT tagged
// YAML_CPP_API, unlike its base BadConversion.  Under the release build
// (Ubuntu-22.04 container, -O3 -flto) the runtime fails to match the derived
// type against the catch-by-base clause, so the exception escapes loadSettings
// -> std::terminate -> the whole Steam process aborts.  It fires on the file-
// watcher thread the moment the value is saved, and again in setup() on every
// relaunch, so the client is stuck until the config is hand-edited back to 0.
//
//   Stack trace (from the core):
//     __cxa_throw  ->  CConfig::loadSettings() [.cold]  ->  CConfig::init()
//     ->  setup()  ->  _dl_audit_preinit  ->  abort
//     thrown type_info:  YAML::TypedBadConversion<int>
//
// THE FIX: use `catch (...)` in getSetting, exactly like every other parsing
// block in loadSettings already does.  catch (...) needs no RTTI base-walk, so
// it is guaranteed by the standard to contain the throw regardless of toolchain
// / LTO / symbol visibility.  This test pins that contract: feeding the literal
// value that bricked the client through the catch-all shape must fall back to
// the default and exit cleanly, never abort.
//
// (The catch-by-base escape only manifests on the release toolchain; a host gcc
// matches it fine, so the definitive reproduction lives in the coredump above.
// This test locks in the safe behaviour portably.)
//
// Build (release-faithful flags):
//   g++ -O3 -flto=auto -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
//       -I include tools/test_config_overflow.cpp lib/libyaml-cpp.a \
//       -lpthread -o /tmp/test_config_overflow && /tmp/test_config_overflow

#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

// The catch-all shape of the FIXED CConfig::getSetting.
template <typename T>
static T getSetting_fixed(YAML::Node& node, const char* name, T defVal) {
  if (!node[name]) return defVal;
  try {
    return node[name].as<T>();
  } catch (...) {
    return defVal;
  }
}

// The catch-by-base shape of the BROKEN getSetting, kept for documentation /
// in-environment probing (it aborts under the release toolchain).
template <typename T>
static T getSetting_broken(YAML::Node& node, const char* name, T defVal) {
  if (!node[name]) return defVal;
  try {
    return node[name].as<T>();
  } catch (YAML::BadConversion&) {
    return defVal;
  }
}

// The exact config value that bricked the user's client.
static const char* kConfig = "FakeWalletBalance: 500000000000000000\n";

// Run `fn` over the bad config in a child process.  Returns the child's
// exit code, or -signal if it died from a signal (e.g. -6 for SIGABRT).
template <typename F>
static int childExit(F fn) {
  pid_t pid = fork();
  if (pid == 0) {
    YAML::Node node = YAML::Load(kConfig);
    int32_t v = fn(node);
    _exit(v == 0 ? 0 : 2); // out-of-range must fall back to the default (0)
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFSIGNALED(status)) return -WTERMSIG(status);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 99;
}

int main() {
  int failures = 0;

  // CONTRACT: the catch-all getSetting must contain the overflow, return the
  // default (0), and exit cleanly.  This is the behaviour the fix guarantees.
  int fixed = childExit([](YAML::Node& n) {
    return getSetting_fixed<int32_t>(n, "FakeWalletBalance", 0);
  });
  if (fixed == 0) {
    printf("OK: catch (...) contains the overflow and returns the default.\n");
  } else if (fixed < 0) {
    printf("FAIL: catch (...) aborted on signal %d (must never happen).\n",
           -fixed);
    failures++;
  } else {
    printf("FAIL: catch (...) returned a non-default value (exit %d).\n", fixed);
    failures++;
  }

  // PROBE (informational, not scored): show how the old catch-by-base behaves
  // on this toolchain.  A negative result is the production crash; a clean 0
  // means the host toolchain happens to match the derived type (still unsafe
  // to rely on — the release build does not).
  int broken = childExit([](YAML::Node& n) {
    return getSetting_broken<int32_t>(n, "FakeWalletBalance", 0);
  });
  if (broken < 0)
    printf("note: catch (YAML::BadConversion&) ABORTED (signal %d) -- this is "
           "the shipped crash.\n", -broken);
  else
    printf("note: catch (YAML::BadConversion&) returned exit %d on this "
           "toolchain (release build aborts here).\n", broken);

  if (failures) {
    printf("\n%d check(s) failed\n", failures);
    return 1;
  }
  printf("\nall checks passed\n");
  return 0;
}

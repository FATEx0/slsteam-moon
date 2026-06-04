# Build glue for slsteam-moon.
#
# Common invocations:
#   make             Build bin/SLSsteam.so + bin/library-inject.so on the host.
#                    Use scripts/build.sh --portable for a release-grade
#                    binary with broader glibc compatibility.
#   make clean       Remove all build artefacts.
#   make install     Install onto the host (delegates to setup.sh install).
#   make release     Build portable + package dist/slsteam-moon-linux-<ver>.zip.
#                    Same as scripts/release.sh.
#
# -MMD/-MP keep dependency files in sync so header edits trigger
# recompilation, see https://stackoverflow.com/q/52034997.

# Force g++; clang miscompiles a few hooks.
CXX := g++

libs := $(wildcard lib/*.a)
srcs := $(shell find src/ -type f -iname "*.cpp")
objs := $(srcs:src/%.cpp=obj/%.o)
deps := $(objs:%.o=%.d)

CXXFLAGS := -O3 -flto=auto -fPIC -m32 -std=c++20 \
            -Wall -Wextra -Wpedantic -Wno-error=format-security \
            -D_GLIBCXX_USE_CXX11_ABI=0

LDFLAGS := -shared -Wl,--no-undefined -lpthread -ldl

ifeq ($(shell echo $$NATIVE),1)
	CXXFLAGS += -march=native
endif

# Optional speed-ups picked up if installed.
ifeq ($(shell type ccache &> /dev/null && echo "found"),found)
	export PATH := /usr/lib/ccache/bin:$(PATH)
endif
ifeq ($(shell type mold &> /dev/null && echo "found"),found)
	LDFLAGS += -fuse-ld=mold
endif

.PHONY: all build rebuild clean install release
.NOTPARALLEL: clean rebuild

all: build
build: bin/SLSsteam.so bin/library-inject.so
rebuild: clean build

bin/SLSsteam.so: $(objs) $(libs)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Audit-side helper that redirects libcurl loading to a system copy.
# Loaded ahead of SLSsteam.so via $LD_AUDIT.
bin/library-inject.so: tools/library-inject/main.cpp
	@mkdir -p bin
	$(CXX) -O3 -m32 -fPIC -shared -std=c++20 $< -o $@

-include $(deps)
obj/update.o: src/update.cpp res/version.txt
	$(shell ./embed-version.sh)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

-include $(deps)
obj/config.o: src/config.cpp res/config.yaml
	$(shell ./embed-config.sh)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

-include $(deps)
obj/%.o : src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

clean:
	rm -rf obj/ bin/ dist/

install:
	sh setup.sh install

release:
	bash scripts/release.sh

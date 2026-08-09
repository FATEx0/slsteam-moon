// Regression test for CM libcurl initialization before a PICS worker fetch.

#include "../src/feats/cmclient.hpp"

#include <cstdio>
#include <thread>

int main()
{
	if (!CmClient::prepareForThreadedFetch())
	{
		std::fprintf(stderr, "FAIL: CM curl loader warmup failed\n");
		return 1;
	}

	bool workerReady = false;
	std::thread worker([&workerReady]
	{
		workerReady = CmClient::prepareForThreadedFetch();
	});
	worker.join();

	if (!workerReady)
	{
		std::fprintf(stderr, "FAIL: CM curl loader was not reusable from worker\n");
		return 1;
	}

	std::puts("CM curl loader warmup tests passed");
	return 0;
}

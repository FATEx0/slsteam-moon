#pragma once

#include <pthread.h>
#include <string>
#include <unordered_map>
#include <vector>

typedef void(*FileModifyEvent_t)();

class CFileWatcher
{
	pthread_t watchThread;

public:
	int notifyFd;
	std::unordered_map<int, std::string> fileFdMap;
	// Owned copies of every watched path, so the watch can be re-armed after an
	// atomic-rename replace swaps the file's inode (see rearm()).
	std::vector<std::string> watchedPaths;

	FileModifyEvent_t onModify;

	CFileWatcher(FileModifyEvent_t onModify);
	~CFileWatcher();

	bool addFile(const char* path);
	void rearm();
	bool start();
	void stop();
};

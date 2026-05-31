#pragma once

#include <pthread.h>
#include <unordered_map>

typedef void(*FileModifyEvent_t)();

class CFileWatcher
{
	pthread_t watchThread;

public:
	int notifyFd;
	std::unordered_map<int, const char*> fileFdMap;

	FileModifyEvent_t onModify;

	CFileWatcher(FileModifyEvent_t onModify);
	~CFileWatcher();

	bool addFile(const char* path);
	bool start();
	void stop();
};

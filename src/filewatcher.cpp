#include "filewatcher.hpp"

#include "log.hpp"

#include <sys/inotify.h>
#include <unistd.h>


//TODO: Investigate why gcc complains when put into CFileWatcher itself
void* watchLoop(void* args)
{
	auto watcher = reinterpret_cast<CFileWatcher*>(args);
	g_pLog->debug("Started FileWatcher %u\n", watcher->notifyFd);

	for(;;)
	{
		g_pLog->debug("Watching for changes...\n");

		inotify_event event {};
		size_t size = read(watcher->notifyFd, &event, sizeof(inotify_event));
		if (!size)
		{
			continue;
		}

		g_pLog->debug("inotify %u(%s) -> %u\n", event.wd, watcher->fileFdMap[event.wd], event.mask);
		watcher->onModify();
	}

	return nullptr;
}

CFileWatcher::CFileWatcher(FileModifyEvent_t onModify)
{
	this->onModify = onModify;

	notifyFd = inotify_init();
	g_pLog->debug("Created notify fd %i\n", notifyFd);
}

CFileWatcher::~CFileWatcher()
{
	if (watchThread)
	{
		stop();
	}

	if (notifyFd != -1)
	{
		close(notifyFd);

		for(const auto& fd : fileFdMap)
		{
			if (fd.first == -1)
			{
				continue;
			}

			close(fd.first);
		}
	}
}

bool CFileWatcher::addFile(const char* path)
{
	int fd = inotify_add_watch(notifyFd, path, IN_MODIFY);
	if (fd == -1)
	{
		return false;
	}

	fileFdMap[fd] = path;
	g_pLog->debug("Added %s to FileWatcher %i\n", path, notifyFd);
	return fd != -1;
}

bool CFileWatcher::start()
{
	int code = pthread_create(&watchThread, nullptr, &watchLoop, this);
	return code == 0;
}

void CFileWatcher::stop()
{
	pthread_cancel(watchThread);
}

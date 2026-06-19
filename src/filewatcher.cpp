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

		g_pLog->debug("inotify wd=%u mask=%u\n", event.wd, event.mask);
		watcher->onModify();

		// The config is rewritten via atomic rename (write tmp, then rename over
		// the target), which swaps the file's inode. inotify watches the inode,
		// so the original watch is auto-removed (IN_IGNORED) on the first such
		// write and every later change would go unnoticed — the live reload
		// would silently stop after one edit. Re-arm on the current inode after
		// each event so repeated writes (e.g. successive menu pin/unlock saves)
		// keep reloading.
		watcher->rearm();
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

	// Closing the inotify instance removes all of its watches; the per-watch
	// descriptors are watch ids (not file descriptors) and must not be close()d.
	if (notifyFd != -1)
	{
		close(notifyFd);
	}
}

bool CFileWatcher::addFile(const char* path)
{
	int fd = inotify_add_watch(notifyFd, path, IN_MODIFY);
	if (fd == -1)
	{
		return false;
	}

	watchedPaths.emplace_back(path);
	fileFdMap[fd] = path;
	g_pLog->debug("Added %s to FileWatcher %i\n", path, notifyFd);
	return true;
}

// Re-add the inotify watch for every tracked path. inotify_add_watch is
// idempotent for an unchanged inode (it returns the existing watch id), and
// attaches to the NEW inode after an atomic-rename replace — so this both
// refreshes a live watch and recovers one the kernel dropped via IN_IGNORED.
void CFileWatcher::rearm()
{
	for (const auto& path : watchedPaths)
	{
		int fd = inotify_add_watch(notifyFd, path.c_str(), IN_MODIFY);
		if (fd != -1)
		{
			fileFdMap[fd] = path;
		}
	}
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

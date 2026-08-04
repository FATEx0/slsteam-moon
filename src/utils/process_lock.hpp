// SPDX-License-Identifier: AGPL-3.0-only
//
// Small advisory-lock primitive shared by the audit setup and the on-disk
// cache writers.  flock() is deliberately used instead of a pid/sentinel
// protocol: the kernel releases it when the owning namespace/process dies.

#pragma once

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

namespace ProcessLock
{

class FileLock
{
public:
	explicit FileLock(const std::string& path, bool nonBlocking = true)
		: path_(path)
	{
		fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (fd_ < 0) return;

		const int mode = LOCK_EX | (nonBlocking ? LOCK_NB : 0);
		if (::flock(fd_, mode) != 0)
		{
			::close(fd_);
			fd_ = -1;
		}
	}

	~FileLock()
	{
		if (fd_ >= 0) ::close(fd_);
	}

	FileLock(const FileLock&) = delete;
	FileLock& operator=(const FileLock&) = delete;

	FileLock(FileLock&& other) noexcept
		: path_(std::move(other.path_)), fd_(other.fd_)
	{
		other.fd_ = -1;
	}

	FileLock& operator=(FileLock&& other) noexcept
	{
		if (this == &other) return *this;
		if (fd_ >= 0) ::close(fd_);
		path_ = std::move(other.path_);
		fd_ = other.fd_;
		other.fd_ = -1;
		return *this;
	}

	bool acquired() const { return fd_ >= 0; }
	int fd() const { return fd_; }
	const std::string& path() const { return path_; }

private:
	std::string path_;
	int fd_ = -1;
};

inline std::string perProcessPath(const char* prefix, pid_t pid = ::getpid())
{
	char path[128];
	std::snprintf(path, sizeof(path), "/tmp/%s.%d", prefix,
	              static_cast<int>(pid));
	return path;
}

} // namespace ProcessLock

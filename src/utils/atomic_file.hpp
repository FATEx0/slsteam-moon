// SPDX-License-Identifier: AGPL-3.0-only
//
// Write a complete file to a private temporary inode, fsync it, and publish it
// with rename().  Readers therefore see either the previous complete record or
// the new complete record; never a truncated buffer or half-written YAML.

#pragma once

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace AtomicFile
{

namespace detail
{

inline std::atomic<unsigned long> sequence{0};

inline bool writeAll(int fd, const char* data, std::size_t size)
{
	std::size_t written = 0;
	while (written < size)
	{
		const ssize_t n = ::write(fd, data + written, size - written);
		if (n > 0)
		{
			written += static_cast<std::size_t>(n);
			continue;
		}
		if (n < 0 && errno == EINTR) continue;
		return false;
	}
	return true;
}

inline void syncParent(const std::string& path)
{
	const auto parent = std::filesystem::path(path).parent_path();
	const std::string parentPath = parent.empty() ? "." : parent.string();
	const int fd = ::open(parentPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) return;
	(void)::fsync(fd);
	::close(fd);
}

} // namespace detail

inline bool write(const std::string& path, const char* data, std::size_t size,
	              std::string& error)
{
	error.clear();
	if (path.empty())
	{
		error = "empty path";
		return false;
	}

	mode_t mode = 0644;
	struct stat existing{};
	if (::stat(path.c_str(), &existing) == 0)
		mode = existing.st_mode & 07777;

	const auto serial = detail::sequence.fetch_add(1, std::memory_order_relaxed);
	const std::string tmp = path + ".tmp." + std::to_string(
		static_cast<long long>(::getpid())) + "." + std::to_string(serial);
	const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
	                      mode);
	if (fd < 0)
	{
		error = "open temporary file failed: " + std::string(std::strerror(errno));
		return false;
	}

	bool ok = detail::writeAll(fd, data, size);
	if (ok && ::fchmod(fd, mode) != 0) ok = false;
	if (ok && ::fsync(fd) != 0) ok = false;
	const int closeResult = ::close(fd);
	if (closeResult != 0) ok = false;

	if (!ok)
	{
		error = "write/fsync temporary file failed";
		::unlink(tmp.c_str());
		return false;
	}

	if (::rename(tmp.c_str(), path.c_str()) != 0)
	{
		error = "rename failed: " + std::string(std::strerror(errno));
		::unlink(tmp.c_str());
		return false;
	}
	detail::syncParent(path);
	return true;
}

inline bool write(const std::string& path, const std::string& contents,
	              std::string& error)
{
	return write(path, contents.data(), contents.size(), error);
}

} // namespace AtomicFile

#pragma once

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <unistd.h>

namespace AuditLog
{
	inline std::size_t textLength(const char* text) noexcept
	{
		const char* end = text;
		while (*end != '\0')
		{
			++end;
		}
		return static_cast<std::size_t>(end - text);
	}

	// Write a small diagnostic without consulting C++ runtime state. Steam may
	// close inherited descriptors in a fork child before the exec wrapper runs,
	// so EBADF means the path must be reopened before retrying the same bytes.
	inline void write(int& fd, const char* path, const char* text) noexcept
	{
		if (path == nullptr || path[0] == '\0' || text == nullptr)
		{
			return;
		}

		int current = fd;
		if (current < 0)
		{
			current = ::open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
			if (current < 0)
			{
				return;
			}
			fd = current;
		}

		const char* cursor = text;
		std::size_t remaining = textLength(text);
		while (remaining > 0)
		{
			const ssize_t written = ::write(current, cursor, remaining);
			if (written < 0)
			{
				if (errno == EBADF)
				{
					const int reopened =
						::open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
					if (reopened < 0)
					{
						fd = -1;
						return;
					}
					fd = reopened;
					current = reopened;
					continue;
				}
				if (errno == EINTR)
				{
					continue;
				}
				return;
			}
			if (written == 0)
			{
				return;
			}
			cursor += written;
			remaining -= static_cast<std::size_t>(written);
		}
	}
}

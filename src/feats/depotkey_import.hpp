// SPDX-License-Identifier: AGPL-3.0-only
//
// Synchronized retryable gate for the Lua depot-key importer.

#pragma once

#include <mutex>

namespace DepotKey
{
	// Serialize the importer called from preinit and the later startup/watcher
	// paths.  The gate is completed only after the body returns successfully:
	// missing prerequisites and exceptions therefore leave it retryable.
	class LuaScriptImportGate
	{
	public:
		template <typename Ready, typename Import>
		bool run(Ready&& ready, Import&& import) noexcept
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_done) return false;

			try
			{
				if (!ready()) return false;
				import();
				m_done = true;
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		bool done() const noexcept
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_done;
		}

	private:
		mutable std::mutex m_mutex;
		bool m_done = false;
	};
}

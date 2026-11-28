// pcsx2gs (Kamite): the shader / pipeline warm-up's lists.
//
// Every TFX pipeline (Vulkan) or shader (Direct3D 11) the GS builds is appended to a learned list in the shader cache
// folder; resources/gs_warm/<api>.bin is the list shipped with GHR (K's replays and play, merged by
// tools/gs/warm_merge.py). At the device's start the warm-up builds what this install's caches lack on worker
// threads: everything on a fresh cache (the first start, a new driver), afterwards only shipped entries this install
// has not built yet. The game then finds them in the caches, as on a second run.
//
// Entries are tagged with a hash of the device's features: a list recorded on one GPU only warms the same kind of
// GPU (a selector that needs a feature another GPU lacks is never built there). File: FileHeader, then entries of
// 8 bytes (tag) + entry_size bytes (the selector as the GS keeps it). Another version or size: the file is ignored
// (learned: started again).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include "common/RedtapeWindows.h"
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace GSWarm
{
	// Bump when a selector's layout or meaning changes (a PCSX2 update): old lists are then ignored.
	inline constexpr uint32_t kVersion = 1;

	struct FileHeader
	{
		char magic[4];
		uint32_t version;
		uint32_t entry_size;
		uint32_t api; // 1 Vulkan, 2 Direct3D 11
	};
	static_assert(sizeof(FileHeader) == 16);

	inline uint64_t Hash(const void* data, size_t size, uint64_t hash = 14695981039346656037ull)
	{
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i)
			hash = (hash ^ bytes[i]) * 1099511628211ull;
		return hash;
	}

	// PS2X_GS_WARMUP=0 turns the warm-up off (the lists are still learned).
	inline bool Enabled()
	{
		const char* value = std::getenv("PS2X_GS_WARMUP");
		return !(value && value[0] == '0');
	}

	// A worker thread that runs while the game plays: below normal priority, so the game's threads come first.
	inline void LowerCurrentThreadPriority()
	{
#if defined(_WIN32)
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__linux__)
		setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
	}

	class List
	{
	public:
		~List() { Close(); }

		// tag: the device's feature hash. Reads the shipped and learned entries with that tag.
		void Open(uint32_t api, uint32_t entry_size, uint64_t tag, const std::string& shipped, const std::string& learned)
		{
			Close();
			m_api = api;
			m_size = entry_size;
			m_tag = tag;
			m_path = learned;
			std::vector<std::string> mine;
			const bool learned_ok = Read(learned, mine, &m_foreign);
			for (std::string& entry : mine)
				m_learned.insert(std::move(entry));
			std::vector<std::string> theirs;
			Read(shipped, theirs, nullptr);
			for (std::string& entry : theirs)
				m_shipped.insert(std::move(entry));
			m_file = learned_ok ? std::fopen(learned.c_str(), "ab") : nullptr;
			if (!m_file)
				Restart();
		}

		void Close()
		{
			if (m_file)
				std::fclose(m_file);
			m_file = nullptr;
			m_learned.clear();
			m_shipped.clear();
			m_foreign.clear();
		}

		// The caches were started again (fresh cache, new driver): nothing this install learned is built any more.
		// Returns the old learned entries (to build again) and empties the file, keeping other GPUs' entries.
		std::vector<std::string> Forget()
		{
			std::lock_guard lock(m_mutex);
			std::vector<std::string> old(m_learned.begin(), m_learned.end());
			m_learned.clear();
			Restart();
			return old;
		}

		// Shipped entries this install has not built (with `extra`, deduplicated).
		std::vector<std::string> Missing(std::vector<std::string> extra = {})
		{
			std::lock_guard lock(m_mutex);
			for (const std::string& entry : m_shipped)
				if (!m_learned.count(entry))
					extra.push_back(entry);
			std::sort(extra.begin(), extra.end());
			extra.erase(std::unique(extra.begin(), extra.end()), extra.end());
			return extra;
		}

		// A pipeline / shader now in the caches: appended once (any thread).
		void Learn(const void* selector)
		{
			std::string entry(static_cast<const char*>(selector), m_size);
			std::lock_guard lock(m_mutex);
			if (!m_learned.insert(entry).second || !m_file)
				return;
			std::fwrite(&m_tag, sizeof(m_tag), 1, m_file);
			std::fwrite(entry.data(), 1, entry.size(), m_file);
			std::fflush(m_file);
		}

		uint32_t EntrySize() const { return m_size; }

	private:
		// Entries of this list's kind with our tag; others' raw records (tag + data) into `foreign` when given.
		bool Read(const std::string& path, std::vector<std::string>& out, std::vector<std::string>* foreign) const
		{
			std::FILE* file = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
			if (!file)
				return false;
			FileHeader header{};
			const bool ok = std::fread(&header, sizeof(header), 1, file) == 1 && std::memcmp(header.magic, "GHRW", 4) == 0 &&
			                header.version == kVersion && header.entry_size == m_size && header.api == m_api;
			if (ok)
			{
				std::string record(sizeof(uint64_t) + m_size, '\0');
				while (std::fread(record.data(), 1, record.size(), file) == record.size())
				{
					uint64_t tag;
					std::memcpy(&tag, record.data(), sizeof(tag));
					if (tag == m_tag)
						out.push_back(record.substr(sizeof(tag)));
					else if (foreign)
						foreign->push_back(record);
				}
			}
			std::fclose(file);
			return ok;
		}

		// A new learned file: the header and the other GPUs' records (a Deck docked to another GPU keeps both).
		void Restart()
		{
			if (m_file)
				std::fclose(m_file);
			m_file = m_path.empty() ? nullptr : std::fopen(m_path.c_str(), "wb");
			if (!m_file)
				return;
			FileHeader header{{'G', 'H', 'R', 'W'}, kVersion, m_size, m_api};
			std::fwrite(&header, sizeof(header), 1, m_file);
			for (const std::string& record : m_foreign)
				std::fwrite(record.data(), 1, record.size(), m_file);
			std::fflush(m_file);
		}

		std::mutex m_mutex;
		std::FILE* m_file = nullptr;
		std::string m_path;
		uint32_t m_api = 0;
		uint32_t m_size = 0;
		uint64_t m_tag = 0;
		std::unordered_set<std::string> m_learned, m_shipped;
		std::vector<std::string> m_foreign;
	};

	// Runs `work(item)` for every item on `threads` worker threads; Wait() joins them, Stop() makes them stop early.
	class Pool
	{
	public:
		~Pool() { Stop(); }

		template <typename Work>
		void Start(std::vector<std::string> items, unsigned threads, bool background, Work work)
		{
			Stop();
			m_items = std::move(items);
			m_next = 0;
			m_done = 0;
			m_stop = false;
			threads = std::max(1u, std::min<unsigned>(threads, static_cast<unsigned>(m_items.size())));
			for (unsigned i = 0; i < threads && !m_items.empty(); ++i)
				m_threads.emplace_back([this, background, work]() {
					if (background)
						LowerCurrentThreadPriority();
					for (;;)
					{
						const size_t index = m_next.fetch_add(1);
						if (m_stop || index >= m_items.size())
							break;
						work(m_items[index]);
						m_done.fetch_add(1);
					}
				});
		}

		void Wait()
		{
			for (std::thread& thread : m_threads)
				thread.join();
			m_threads.clear();
		}

		void Stop()
		{
			m_stop = true;
			Wait();
		}

		bool Running() const { return !m_threads.empty() && m_done.load() < m_items.size() && !m_stop; }
		size_t Done() const { return m_done.load(); }
		size_t Total() const { return m_items.size(); }

	private:
		std::vector<std::string> m_items;
		std::vector<std::thread> m_threads;
		std::atomic<size_t> m_next{0}, m_done{0};
		std::atomic<bool> m_stop{false};
	};

	// Worker threads for a warm-up: all cores but one when the game waits for it, one or two while it plays.
	inline unsigned Threads(bool background)
	{
		const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
		return background ? (cores >= 8 ? 2u : 1u) : std::max(1u, cores - 1);
	}
}

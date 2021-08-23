/*
 * This file is part of the Monero P2Pool <https://github.com/SChernykh/p2pool>
 * Copyright (c) 2021 SChernykh <https://github.com/SChernykh>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"
#include "pow_hash.h"
#include "p2pool.h"
#include "params.h"
#include "randomx.h"
#include "configuration.h"
#include "virtual_machine.hpp"
#include <thread>

static constexpr char log_category_prefix[] = "RandomX_Hasher ";

namespace p2pool {

RandomX_Hasher::RandomX_Hasher(p2pool* pool)
	: m_pool(pool)
	, m_cache{}
	, m_dataset(nullptr)
	, m_seed{}
	, m_index(0)
	, m_setSeedCounter(0)
{
	uint64_t memory_allocated = 0;

	if (!m_pool->params().m_lightMode) {
		m_dataset = randomx_alloc_dataset(RANDOMX_FLAG_LARGE_PAGES);
		if (!m_dataset) {
			LOGWARN(1, "couldn't allocate RandomX dataset using large pages");
			m_dataset = randomx_alloc_dataset(RANDOMX_FLAG_DEFAULT);
			if (!m_dataset) {
				LOGERR(1, "couldn't allocate RandomX dataset");
			}
		}
		if (m_dataset) {
			memory_allocated += RANDOMX_DATASET_BASE_SIZE + RANDOMX_DATASET_EXTRA_SIZE;
		}
	}

	const randomx_flags flags = randomx_get_flags();

	for (size_t i = 0; i < array_size(m_cache); ++i) {
		m_cache[i] = randomx_alloc_cache(flags | RANDOMX_FLAG_LARGE_PAGES);
		if (!m_cache[i]) {
			LOGWARN(1, "couldn't allocate RandomX cache using large pages");
			m_cache[i] = randomx_alloc_cache(flags);
			if (!m_cache[i]) {
				LOGERR(1, "couldn't allocate RandomX cache, aborting");
				panic();
			}
		}
		memory_allocated += RANDOMX_ARGON_MEMORY * 1024;
	}

	uv_rwlock_init_checked(&m_datasetLock);
	uv_rwlock_init_checked(&m_cacheLock);

	for (size_t i = 0; i < array_size(m_vm); ++i) {
		uv_mutex_init_checked(&m_vm[i].mutex);
		m_vm[i].vm = nullptr;
	}


	memory_allocated = (memory_allocated + (1 << 20) - 1) >> 20;
	LOGINFO(1, "allocated " << memory_allocated << " MB");
}

RandomX_Hasher::~RandomX_Hasher()
{
	m_stopped.exchange(1);
	{
		WriteLock lock(m_datasetLock);
		WriteLock lock2(m_cacheLock);
	}

	uv_rwlock_destroy(&m_datasetLock);
	uv_rwlock_destroy(&m_cacheLock);

	for (size_t i = 0; i < array_size(m_vm); ++i) {
		{
			MutexLock lock(m_vm[i].mutex);
			if (m_vm[i].vm) {
				randomx_destroy_vm(m_vm[i].vm);
			}
		}
		uv_mutex_destroy(&m_vm[i].mutex);
	}

	if (m_dataset) {
		randomx_release_dataset(m_dataset);
	}

	for (size_t i = 0; i < array_size(m_cache); ++i) {
		if (m_cache[i]) {
			randomx_release_cache(m_cache[i]);
		}
	}

	LOGINFO(1, "stopped");
}

void RandomX_Hasher::set_seed_async(const hash& seed)
{
	struct Work
	{
		p2pool* pool;
		RandomX_Hasher* hasher;
		hash seed;
		uv_work_t req;
	};

	Work* work = new Work{};
	work->pool = m_pool;
	work->hasher = this;
	work->seed = seed;
	work->req.data = work;

	uv_queue_work(uv_default_loop(), &work->req,
		[](uv_work_t* req)
		{
			num_running_jobs.fetch_add(1);
			Work* work = reinterpret_cast<Work*>(req->data);
			if (!work->pool->stopped()) {
				work->hasher->set_seed(work->seed);
			}
		},
		[](uv_work_t* req, int)
		{
			delete reinterpret_cast<Work*>(req->data);
			num_running_jobs.fetch_sub(1);
		}
	);
}

void RandomX_Hasher::set_old_seed_async(const hash& seed)
{
	struct Work
	{
		p2pool* pool;
		RandomX_Hasher* hasher;
		hash seed;
		uv_work_t req;
	};

	Work* work = new Work{};
	work->pool = m_pool;
	work->hasher = this;
	work->seed = seed;
	work->req.data = work;

	uv_queue_work(uv_default_loop(), &work->req,
		[](uv_work_t* req)
		{
			num_running_jobs.fetch_add(1);
			Work* work = reinterpret_cast<Work*>(req->data);
			if (!work->pool->stopped()) {
				work->hasher->set_old_seed(work->seed);
			}
		},
		[](uv_work_t* req, int)
		{
			delete reinterpret_cast<Work*>(req->data);
			num_running_jobs.fetch_sub(1);
		}
	);
}

void RandomX_Hasher::set_seed(const hash& seed)
{
	if (m_stopped.load()) {
		return;
	}

	WriteLock lock(m_datasetLock);
	uv_rwlock_wrlock(&m_cacheLock);

	m_setSeedCounter.fetch_add(1);

	if (m_seed[m_index] == seed) {
		uv_rwlock_wrunlock(&m_cacheLock);
		return;
	}

	{
		ON_SCOPE_LEAVE([this]() { uv_rwlock_wrunlock(&m_cacheLock); });

		if (m_stopped.load()) {
			return;
		}

		m_index ^= 1;
		m_seed[m_index] = seed;

		LOGINFO(1, "new seed " << log::LightBlue() << seed);
		randomx_init_cache(m_cache[m_index], m_seed[m_index].h, HASH_SIZE);

		MutexLock lock2(m_vm[m_index].mutex);

		if (m_vm[m_index].vm) {
			m_vm[m_index].vm->setCache(m_cache[m_index]);
		}
		else {
			const randomx_flags flags = randomx_get_flags();

			m_vm[m_index].vm = randomx_create_vm(flags | RANDOMX_FLAG_LARGE_PAGES, m_cache[m_index], nullptr);
			if (!m_vm[m_index].vm) {
				LOGWARN(1, "couldn't allocate RandomX light VM using large pages");
				m_vm[m_index].vm = randomx_create_vm(flags, m_cache[m_index], nullptr);
				if (!m_vm[m_index].vm) {
					LOGERR(1, "couldn't allocate RandomX light VM, aborting");
					panic();
				}
			}
		}
	}

	LOGINFO(1, log::LightCyan() << "cache updated");

	if (m_dataset) {
		const uint32_t numItems = randomx_dataset_item_count();
		uint32_t numThreads = std::thread::hardware_concurrency();

		// Use only half the cores to let other threads do their stuff in the meantime
		if (numThreads > 1) {
			numThreads /= 2;
		}

		LOGINFO(1, log::LightCyan() << "running " << numThreads << " threads to update dataset");

		ReadLock lock2(m_cacheLock);

		if (numThreads > 1) {
			std::vector<std::thread> threads;
			threads.reserve(numThreads);

			for (uint32_t i = 0; i < numThreads; ++i) {
				const uint32_t a = (numItems * i) / numThreads;
				const uint32_t b = (numItems * (i + 1)) / numThreads;

				threads.emplace_back([this, a, b]()
					{
						make_thread_background();
						randomx_init_dataset(m_dataset, m_cache[m_index], a, b - a);
					});
			}

			for (std::thread& t : threads) {
				t.join();
			}
		}
		else {
			randomx_init_dataset(m_dataset, m_cache[m_index], 0, numItems);
		}

		MutexLock lock3(m_vm[FULL_DATASET_VM].mutex);

		if (!m_vm[FULL_DATASET_VM].vm) {
			const randomx_flags flags = randomx_get_flags();
			m_vm[FULL_DATASET_VM].vm = randomx_create_vm(flags | RANDOMX_FLAG_LARGE_PAGES | RANDOMX_FLAG_FULL_MEM, nullptr, m_dataset);
			if (!m_vm[FULL_DATASET_VM].vm) {
				LOGWARN(1, "couldn't allocate RandomX VM using large pages");
				m_vm[FULL_DATASET_VM].vm = randomx_create_vm(flags, nullptr, m_dataset);
				if (!m_vm[FULL_DATASET_VM].vm) {
					LOGERR(1, "couldn't allocate RandomX VM");
				}
			}
		}

		LOGINFO(1, log::LightCyan() << "dataset updated");
	}
}

void RandomX_Hasher::set_old_seed(const hash& seed)
{
	// set_seed() must go first, wait for it
	while (m_setSeedCounter.load() == 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	LOGINFO(1, "old seed " << log::LightBlue() << seed);

	{
		WriteLock lock(m_cacheLock);

		const uint32_t old_index = m_index ^ 1;
		m_seed[old_index] = seed;

		randomx_init_cache(m_cache[old_index], m_seed[old_index].h, HASH_SIZE);

		MutexLock lock2(m_vm[old_index].mutex);

		if (m_vm[old_index].vm) {
			m_vm[old_index].vm->setCache(m_cache[old_index]);
		}
		else {
			const randomx_flags flags = randomx_get_flags();

			m_vm[old_index].vm = randomx_create_vm(flags | RANDOMX_FLAG_LARGE_PAGES, m_cache[old_index], nullptr);
			if (!m_vm[old_index].vm) {
				LOGWARN(1, "couldn't allocate RandomX light VM using large pages");
				m_vm[old_index].vm = randomx_create_vm(flags, m_cache[old_index], nullptr);
				if (!m_vm[old_index].vm) {
					LOGERR(1, "couldn't allocate RandomX light VM, aborting");
					panic();
				}
			}
		}
	}
	LOGINFO(1, log::LightCyan() << "old cache updated");
}

bool RandomX_Hasher::calculate(const void* data, size_t size, const hash& seed, hash& result)
{
	// First try to use the dataset if it's ready
	if (uv_rwlock_tryrdlock(&m_datasetLock) == 0) {
		ON_SCOPE_LEAVE([this]() { uv_rwlock_rdunlock(&m_datasetLock); });

		if (m_stopped.load()) {
			return false;
		}

		MutexLock lock(m_vm[FULL_DATASET_VM].mutex);

		if (m_vm[FULL_DATASET_VM].vm && (seed == m_seed[m_index])) {
			randomx_calculate_hash(m_vm[FULL_DATASET_VM].vm, data, size, &result);
			return true;
		}
	}

	// If dataset is not ready, use the cache and wait if necessary
	ReadLock lock(m_cacheLock);

	if (m_stopped.load()) {
		return false;
	}

	{
		MutexLock lock2(m_vm[m_index].mutex);
		if (m_vm[m_index].vm && (seed == m_seed[m_index])) {
			randomx_calculate_hash(m_vm[m_index].vm, data, size, &result);
			return true;
		}
	}

	const uint32_t prev_index = m_index ^ 1;

	MutexLock lock2(m_vm[prev_index].mutex);

	if (m_vm[prev_index].vm && (seed == m_seed[prev_index])) {
		randomx_calculate_hash(m_vm[prev_index].vm, data, size, &result);
		return true;
	}

	return false;
}

} // namespace p2pool
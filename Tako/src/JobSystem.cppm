module;
#include "Utility.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <deque>
#include <chrono>
#ifdef EMSCRIPTEN
#include <emscripten.h>
#include <emscripten/proxying.h>
#endif
export module Tako.JobSystem;

import Tako.Allocators.FreeListAllocator;
import Tako.Allocators.PoolAllocator;

namespace tako
{

	export struct JobFunctorBase
	{
		virtual ~JobFunctorBase() {}
		virtual void operator()() = 0;
	};

	export template <typename Functor>
	struct JobFunctor final : JobFunctorBase
	{
		JobFunctor(Functor&& func) : f(std::move(func)) {}

		void operator()() override
		{
			f();
		}

		Functor f;
	};

	export class JobSystem;
	export class Job
	{
	public:
		Job(size_t functorSize): m_functorSize(functorSize)
		{
			Reset();
		}

		template<typename Functor>
		void SetFunctor(Functor&& func)
		{
			if (m_functorActive)
			{
				std::destroy_at(reinterpret_cast<JobFunctorBase*>(&m_functorData[0]));
			}
			new (&m_functorData) JobFunctor(std::move(func));
			m_functorActive = true;
		}

		void operator()()
		{
			(*reinterpret_cast<JobFunctorBase*>(&m_functorData[0]))();
		}

		void Reset()
		{
			if (m_functorActive)
			{
				std::destroy_at(reinterpret_cast<JobFunctorBase*>(&m_functorData[0]));
				m_functorActive = false;
			}

			m_parent = nullptr;
			m_continuation = nullptr;
			m_jobsLeft = 1;
		}
	private:
		friend class JobSystem;
		Job* m_parent = nullptr;
		Job* m_continuation = nullptr;
		bool m_functorActive = false;
		std::atomic<int> m_jobsLeft = 1;
		size_t m_functorSize;
		char m_functorData[];
	};

	export class JobQueue
	{
	public:
		JobQueue() noexcept {}
		JobQueue(const JobQueue&) noexcept {};
		~JobQueue() {}
		void Push(Job* job)
		{
			while (m_lock.test_and_set(std::memory_order_acquire));
			m_queue.push_back(job);
			m_lock.clear(std::memory_order_release);
		}

		Job* Pop()
		{
			Job* job = nullptr;
			while (m_lock.test_and_set(std::memory_order_acquire));
			if (!m_queue.empty())
			{
				job = m_queue.front();
				m_queue.pop_front();
			}

			m_lock.clear(std::memory_order_release);
			return job;
		}
	private:
		std::deque<Job*> m_queue;
		std::atomic_flag m_lock = ATOMIC_FLAG_INIT;
	};

	class JobSystem
	{
	public:
		JobSystem(): m_allocator(malloc(16384 * 128), 16384 * 128, 128)
		{
			g_allocator = &m_allocator;
		}

		void Init()
		{
			m_threadIndex = 0;
			#ifdef EMSCRIPTEN
				m_threadCount = emscripten_run_script_int("navigator.hardwareConcurrency");
			#else
				m_threadCount = std::thread::hardware_concurrency();
			#endif
			LOG("Threads: {}", m_threadCount);
			m_localQueues.reserve(m_threadCount);
			m_globalQueues.reserve(m_threadCount);
			for (unsigned int i = 0; i < m_threadCount; i++)
			{
				m_localQueues.emplace_back();
				m_globalQueues.emplace_back();
			}

			int workerTarget = m_threadCount - 1;
			m_workers.resize(workerTarget);
			for (unsigned int i = 0; i < workerTarget; i++)
			{
				int threadIndex = i + 1;
				LOG("Creating Thread: {}", threadIndex);
				std::thread& thread = m_workers[i] = std::thread(&JobSystem::WorkerThread, this, threadIndex);
				thread.detach();
			}
		}

		void JoinAsWorker()
		{
			WorkerThread(0);
		}

		void Stop()
		{
			m_stop = true;
			m_cv.notify_all();
		}

		template<typename Functor>
		static Job* Schedule(Functor&& job)
		{
			auto allocatedJob = AllocateJob(std::move(job));
			return Schedule(allocatedJob);
		}

		static Job* Schedule(Job* job)
		{
			return ScheduleRaw(m_globalQueues[m_threadIndex], job);
		}

		template<typename Functor>
		static Job* ScheduleDetached(Functor&& job)
		{
			auto allocatedJob = AllocateJob(std::move(job));
			return ScheduleRaw(m_globalQueues[m_threadIndex], allocatedJob, true);
		}

		template<typename Functor>
		static Job* ScheduleForThread(unsigned int thread, Functor&& job)
		{
			auto allocatedJob = AllocateJob(std::move(job));
			return ScheduleForThread(thread, allocatedJob);
		}

		static Job* ScheduleForThread(unsigned int thread, Job* job)
		{
			return ScheduleRaw(m_localQueues[thread], job);
		}

		template<typename Functor>
		static void Continuation(Functor&& job)
		{
			m_runningJob->m_continuation = AllocateJob(std::move(job));
		}

		template<typename Functor>
		static void RunJob(Functor&& func)
		{
			ASSERT(m_runningJob == nullptr);

			Job* job = AllocateJob(std::move(func));
			while (job)
			{
				m_runningJob = job;
				(*job)();
				m_runningJob = nullptr;
				Job* cont = job->m_continuation;
				while (job->m_jobsLeft > 1) // Work to "wait" for dependencies to be done
				{
					Work();
					#ifdef TAKO_EMSCRIPTEN
						emscripten_proxy_execute_queue(emscripten_proxy_get_system_queue());
					#else
						WaitFor();
					#endif
				}
				job->m_continuation = nullptr;
				OnJobDone(job);
				job = cont;
			}

		}

	private:
		std::vector<std::thread> m_workers;
		static inline std::vector<JobQueue> m_localQueues;
		static inline std::vector<JobQueue> m_globalQueues;
		std::atomic<bool> m_stop = false;
		static inline std::mutex m_cvMutex;
		static inline std::condition_variable m_cv;
		static inline thread_local Job* m_runningJob = nullptr;
		static inline thread_local unsigned int m_threadIndex;
		static inline thread_local Job* m_freeJobList = nullptr;
		static inline thread_local size_t m_freeJobListCount = 0;
		static inline thread_local Job* m_deleteJobList = nullptr;
		static inline thread_local size_t m_deleteJobListCount = 0;
		Allocators::PoolAllocator m_allocator;
		static inline Allocators::PoolAllocator* g_allocator;
		static inline std::mutex m_allocMutex;
		static inline unsigned int m_threadCount;

		template<typename Functor>
		static Job* AllocateJob(Functor&& func)
		{
			auto job = m_freeJobList;
			Job* prevJob = nullptr;
			size_t functorSize = std::max<size_t>(sizeof(JobFunctor<Functor>), 24);
			while (job && job->m_functorSize < functorSize)
			{
				prevJob = job;
				job = job->m_parent;
			}
			if (job)
			{
				if (prevJob)
				{
					prevJob->m_parent = job->m_parent;
				}
				else
				{
					m_freeJobList = job->m_parent;
				}
				job->m_parent = nullptr;
				m_freeJobListCount--;
			}
			else
			{
				void* ptr;
				{
					std::lock_guard lk(m_allocMutex);
					//TODO: fix freelistallocator
					//ptr = malloc(sizeof(Job) + functorSize);
					ptr = g_allocator->Allocate();
				}
				ASSERT(ptr);
				//job = new (ptr) Job(functorSize);
				job = new (ptr) Job(128 - sizeof(Job));
			}
			job->SetFunctor<Functor>(std::move(func));
			return job;
		}

		static void DeallocateJob(Job* job)
		{
			job->Reset();
			if (m_freeJobListCount >= 100)
			{
				job->m_parent = m_deleteJobList;
				m_deleteJobList = job;
				m_deleteJobListCount++;
				return;
			}
			job->m_parent = m_freeJobList;
			m_freeJobList = job;
			m_freeJobListCount++;
		}

		static Job* ScheduleRaw(JobQueue& queue, Job* job, bool detached = false)
		{
			if (m_runningJob && job->m_parent == nullptr && !detached)
			{
				m_runningJob->m_jobsLeft++;
				job->m_parent = m_runningJob;
			}
			queue.Push(job);
			m_cv.notify_all();
			return job;
		}

		static void ExecuteJob(Job* job)
		{
			//LOG("Start job({})", m_threadIndex);
			(*job)();
			m_runningJob = nullptr;
			OnJobDone(job);
		}

		void WorkerThread(unsigned int threadIndex)
		{
			m_threadIndex = threadIndex;
			while (!m_stop)
			{
				if (Work())
				{
					WaitFor();
				}
			}
		}

		static bool Work()
		{
			auto job = m_localQueues[m_threadIndex].Pop();
			for (unsigned int i = 0; job == nullptr && i < m_threadCount; i++)
			{
				job = m_globalQueues[(i + m_threadIndex) % m_threadCount].Pop();
			}
			m_runningJob = job;
			if (job != nullptr)
			{
				ExecuteJob(job);
			}
			else
			{
				if (m_deleteJobList)
				{
					DeleteOldJobs();
					return false;
				}
			}

			return true;
		}

		static void WaitFor()
		{
			std::unique_lock lk(m_cvMutex);
			m_cv.wait_for(lk, std::chrono::microseconds(1000));
		}

		static void OnJobDone(Job* job)
		{
			auto left = job->m_jobsLeft.fetch_sub(1);
			if (left == 1)
			{
				// Job done, cleanup
				if (job->m_continuation)
				{
					if (job->m_parent)
					{
						job->m_parent->m_jobsLeft++;
						job->m_continuation->m_parent = job->m_parent;
					}
					Schedule(job->m_continuation); //TODO: save schedule params
				}
				if (job->m_parent)
				{
					OnJobDone(job->m_parent);
				}
				DeallocateJob(job);
			}
		}

		static void DeleteOldJobs()
		{
			std::lock_guard lk(m_allocMutex);
			while (m_deleteJobList)
			{
				auto job = m_deleteJobList;
				m_deleteJobList = m_deleteJobList->m_parent;
				std::destroy_at(job);
				//g_allocator->Deallocate(job, sizeof(Job) + job->m_functorSize);
				g_allocator->Deallocate(job);
				//free(job);
			}
			m_deleteJobListCount = 0;
		}
	};
}

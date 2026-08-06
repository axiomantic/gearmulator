#pragma once

// executor.h — SCH-6
//
// A minimal single-thread executor that serialises submitted work items.
// Owns a std::thread that runs until shutdown() is called.

#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

namespace g2
{
	/* Executor — SCH-6 / SCH-7
	 *
	 * Owns one worker thread.  Work items submitted via submit() are
	 * executed in FIFO order on that thread.  shutdown() drains the
	 * outstanding queue and joins the worker; it is idempotent.
	 *
	 * Not copyable.  Not movable.  Thread-safe submit / shutdown.
	 */
	class Executor final
	{
	public:
		Executor();
		~Executor();

		Executor(const Executor&) = delete;
		Executor& operator=(const Executor&) = delete;
		Executor(Executor&&) = delete;
		Executor& operator=(Executor&&) = delete;

		/* Submit a callable to the worker thread.
		 * Throws std::runtime_error if called after shutdown(). */
		void submit(std::function<void()> _fn);

		/* Drain the pending queue and join the worker thread.
		 * Safe to call multiple times. */
		void shutdown();

	private:
		void workerLoop();

		std::thread                    m_thread;
		std::mutex                     m_mutex;
		std::condition_variable        m_cv;
		std::queue<std::function<void()>> m_queue;
		bool                           m_shutdown{false};
	};
} // namespace g2

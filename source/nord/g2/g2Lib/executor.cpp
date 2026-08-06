// executor.cpp — SCH-6 / SCH-7

#include "executor.h"
#include <stdexcept>

namespace g2
{

Executor::Executor()
    : m_thread(&Executor::workerLoop, this)
{
}

Executor::~Executor()
{
	shutdown();
}

void Executor::submit(std::function<void()> _fn)
{
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		if (m_shutdown)
			throw std::runtime_error("g2::Executor::submit called after shutdown");
		m_queue.push(std::move(_fn));
	}
	m_cv.notify_one();
}

void Executor::shutdown()
{
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		if (m_shutdown)
			return; // idempotent
		m_shutdown = true;
	}
	m_cv.notify_one();

	if (m_thread.joinable())
		m_thread.join();
}

void Executor::workerLoop()
{
	for (;;)
	{
		std::function<void()> fn;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait(lock, [this]{ return m_shutdown || !m_queue.empty(); });

			if (!m_queue.empty())
			{
				fn = std::move(m_queue.front());
				m_queue.pop();
			}
			else if (m_shutdown)
			{
				return;
			}
		}
		if (fn)
			fn();
	}
}

} // namespace g2

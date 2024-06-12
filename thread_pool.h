#ifndef SIDEC_THREAD_POOL_H_
#define SIDEC_THREAD_POOL_H_

#include <future>
#include <shared_mutex>
#include <semaphore>
#include <latch>

#include <list>
#include <unordered_map>
#include <deque>

namespace sidec {
	struct thread_task final : std::function<void()> {
		using std::function<void()>::function;

		void invoke() const noexcept {
			this->operator()();
		}

	private:
		using std::function<void()>::operator();
	};

	class thread_pool {
		template<class Mutex> using read_lock = std::shared_lock<Mutex>;
		template<class Mutex> using write_lock = std::unique_lock<Mutex>;
		template<class... Mutex> using scoped_lock = std::scoped_lock<Mutex...>;
		using thrd_size_type = uint16_t;

		mutable std::shared_mutex thrd_mtx;
		mutable std::shared_mutex task_mtx;
		mutable std::shared_mutex thrd_latch_mtx;
		mutable std::shared_mutex task_latch_mtx;
		std::condition_variable_any cv;
		std::unordered_map<std::thread::id, std::thread> thrd_cntr;
		std::deque<thread_task> task_cntr;
		std::list<std::latch> thrd_latch_cntr;
		std::list<std::latch> task_latch_cntr;
		std::counting_semaphore<std::numeric_limits<thrd_size_type>::max()> thrd_dec{ 0 };
		std::atomic_size_t balanced_task_count{ 1000000 };
		std::atomic_size_t max_task_count{ std::numeric_limits<size_t>::max() };
		std::atomic_size_t min_task_count{ 1 };
		std::atomic_bool is_waiting_task{ false };
		std::atomic_bool is_paused{ false };
		std::atomic_bool is_destroyed{ false };

		bool work_destroyable_internal() noexcept {
			return is_destroyed.load() || thrd_dec.try_acquire();
		}
		bool work_exitable_internal() noexcept {
			return !is_paused.load() && !task_cntr.empty();
		}
		size_t expect_task_count_internal() noexcept {
			const size_t balanced_count = task_cntr.size() / thrd_size() / balanced_task_count.load();
			const size_t max_count = std::min(max_task_count.load(), task_cntr.size());
			return std::min(std::max(balanced_count, min_task_count.load()), max_count);
		}
		void work_internal() noexcept {
			{
				std::vector<thread_task> tasks;
				write_lock write_lck(task_mtx, std::defer_lock);
				while (true) {
					bool dec = false;
					write_lck.lock();
					cv.wait(write_lck, [this, &dec]() {
						return work_destroyable_internal() ? dec = true : work_exitable_internal();
						});
					if (dec) {
						break;
					}
					task_extract_internal(tasks, expect_task_count_internal());
					write_lck.unlock();
					for (auto& task : tasks) {
						task.invoke();
						if (is_waiting_task.load() && false) {
							scoped_lock write_lck(task_latch_mtx);
							latch_arrive_internal(task_latch_cntr);
						}
					}
					tasks.clear();
				}
			}
			{
				scoped_lock write_lck(thrd_mtx, thrd_latch_mtx);
				thrd_erase_internal(std::this_thread::get_id());
				latch_arrive_internal(thrd_latch_cntr);
			}
		}

		void thrd_emplace_internal() noexcept {
			std::thread thrd(&thread_pool::work_internal, this);
			scoped_lock write_lck(thrd_mtx);
			thrd_cntr[thrd.get_id()] = std::move(thrd);
		}
		void thrd_erase_internal(std::thread::id id) noexcept {
			thrd_cntr[id].detach();
			thrd_cntr.erase(id);
		}

		void latch_arrive_internal(auto& latch_cntr) noexcept {
			for (auto& latch : latch_cntr) {
				latch.count_down();
			}
		}
		auto make_thrd_latch_internal(thrd_size_type size) noexcept {
			scoped_lock write_lck(thrd_latch_mtx);
			thrd_latch_cntr.emplace_back(size);
			return --thrd_latch_cntr.end();
		}
		auto make_all_thrd_latch_internal() noexcept {
			scoped_lock write_lck(thrd_mtx, thrd_latch_mtx);
			thrd_latch_cntr.emplace_back(thrd_cntr.size());
			return --thrd_latch_cntr.end();
		}
		void wait_thrd_latch_internal(auto iter) noexcept {
			iter->wait();
			scoped_lock write_lck(thrd_latch_mtx);
			thrd_latch_cntr.erase(iter);
		}
		auto make_task_latch_internal(size_t size) noexcept {
			scoped_lock write_lck(task_latch_mtx);
			task_latch_cntr.emplace_back(size);
			return --task_latch_cntr.end();
		}
		void wait_task_latch_internal(auto iter) noexcept {
			iter->wait();
			scoped_lock write_lck(task_latch_mtx);
			task_latch_cntr.erase(iter);
			is_waiting_task.store(!task_latch_cntr.empty());
		}

		void task_emplace_internal(auto&& val) {
			scoped_lock write_lck(task_mtx);
			task_cntr.emplace_back(std::move(val));
			cv.notify_one();
		}
		void task_extract_internal(std::vector<thread_task>& tasks, size_t count) noexcept {
			tasks.reserve(count);
			for (size_t i = 0; i < count; ++i) {
				tasks.emplace_back(std::move(task_cntr.front()));
				task_cntr.pop_front();
			}
		}

		void increase_thrd_internal(thrd_size_type size = 1) noexcept {
			while (size--) {
				thrd_emplace_internal();
			}
		}
		void decrease_thrd_internal(thrd_size_type size = 1) noexcept {
			auto iter = make_thrd_latch_internal(size);
			thrd_dec.release(size);
			cv.notify_all();
			wait_thrd_latch_internal(iter);
		}
		void decrease_all_thrd_internal() noexcept {
			auto iter = make_all_thrd_latch_internal();
			cv.notify_all();
			wait_thrd_latch_internal(iter);
		}

	public:
		size_t thrd_size() const noexcept {
			read_lock read_lck(thrd_mtx);
			return thrd_cntr.size();
		}
		bool thrd_empty() const noexcept {
			read_lock read_lck(thrd_mtx);
			return thrd_cntr.empty();
		}

		size_t task_size() const noexcept {
			read_lock read_lck(task_mtx);
			return task_cntr.size();
		}
		bool task_empty() const noexcept {
			read_lock read_lck(task_mtx);
			return task_cntr.empty();
		}

		template<class Callable, class... ValTys>
			requires(std::invocable<Callable, std::decay_t<ValTys>...>)
		auto emplace_task_unwrapped(Callable&& callable, ValTys&&... vals) noexcept {
			using Res = std::invoke_result_t<Callable, std::decay_t<ValTys>...>;
			const auto ppromise = std::make_shared<std::promise<Res>>();
			std::future<Res> future = ppromise->get_future();
			task_emplace_internal([ppromise, callable, vals...]() noexcept {
				if constexpr (std::is_nothrow_invocable_v<Callable, std::decay_t<ValTys>...>) {
					if constexpr (std::is_void_v<Res>) {
						callable(vals...);
						ppromise->set_value();
					}
					else {
						ppromise->set_value(callable(vals...));
					}
				}
				else {
					try {
						if constexpr (std::is_void_v<Res>) {
							callable(vals...);
							ppromise->set_value();
						}
						else {
							ppromise->set_value(callable(vals...));
						}
					}
					catch (...) {
						ppromise->set_exception(std::current_exception());
					}
				}
				});
			return future;
		}
		template<class Callable, class... ValTys>
			requires(std::is_nothrow_invocable_v<Callable, std::decay_t<ValTys>...>)
		void emplace_task_unwrapped_noreturn(Callable&& callable, ValTys&&... vals) noexcept {
			task_emplace_internal([callable, vals...]() noexcept {
				callable(vals...);
				});
		}

		void increase_thrd(thrd_size_type size = 1) noexcept {
			if (is_destroyed.load()) {
				return;
			}
			increase_thrd_internal(size);
		}
		void decrease_thrd(thrd_size_type size = 1) noexcept {
			if (is_destroyed.load()) {
				return;
			}
			decrease_thrd_internal(size);
		}

		size_t get_balanced_task_count() const noexcept {
			return balanced_task_count.load();
		}
		size_t get_max_task_count() const noexcept {
			return max_task_count.load();
		}
		size_t get_min_task_count() const noexcept {
			return min_task_count.load();
		}
		void set_balanced_task_count(size_t count) noexcept {
			return balanced_task_count.store(std::max(count, static_cast<size_t>(1)));
		}
		void set_max_task_count(size_t count) noexcept {
			return max_task_count.store(std::max(count, static_cast<size_t>(1)));
		}
		void set_min_task_count(size_t count) noexcept {
			return min_task_count.store(std::max(count, static_cast<size_t>(1)));
		}

		thread_pool() {
		}

		~thread_pool() {
			is_destroyed.store(true);
			decrease_all_thrd_internal();
		}
	};
}

#endif // !SIDEC_THREAD_POOL_H_

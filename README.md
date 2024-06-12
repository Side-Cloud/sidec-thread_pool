# thread_pool

**thread_pool**是基于C++20的轻量级异步执行框架，支持：通用任务异步执行、高效静态线程池、异常处理机制、低占用静默等。

## 目录
- [特点](#特点)
- [模块简介](#主要模块)
	- [thread_pool](#thread_pool)
- [注意事项](#注意事项)

## 特点

- 轻量的：Header-Only & 代码量 <= 1000行 & 接口简单。
- 高效的：任务支持批量执行，提高了框架的并发性能。
- 灵活的：支持多种任务类型、可通过`thread_pool`构建不同的池模型。
- 稳定的：利用`std::function`的小任务优化减少内存碎片、拥有良好的异步线程异常处理机制。
- 兼容性：纯C++20实现，跨平台，且兼容C++20以上版本。

## 主要模块
### **thread_pool**

**thread_pool** 内置了一条线程安全的任务队列用于同步任务。其管理的每一条异步工作线程负责从任务队列不断获取任务并执行。

让我们先简单地提交一点任务。`thread_pool`可返回一个`std::future`，也可使用不返回的函数（更高效，但必须noexcept）。

```c++
#include <thread_pool.h>

int main() {
    thread_pool pool;
    // 增加 1 条线程（初始 0 线程）
    pool.increase_thrd();
    
    // 提交一个任务（并返回）
    auto fu = pool.emplace_task([]() { return 1; });
    std::cout << "task1 got "<< fu.get() << std::endl;
    // 输出： task1 got 1
    
    // 提交一个任务（不返回）
    pool.emplace_task([]() { std::cout<< "task2 noreturn" << std::endl; return 2; });
    // 输出： task2 noreturn

    while(pool.task_size());
    // 循环判断是否执行完毕
}
```
由于返回一个`std::future`会带来一定的开销，如果你不需要返回值并且希望程序跑得更快，那么你应通过`emplace_task_noreturn`提交任务。

剩下懒得详写

线程每次批量获取任务
```c++
pool.set_equilibrium_value(value) //设置均衡值，越高任务时间分配越均匀
pool.set_max_task_count(count) //设置单次最大任务量
pool.set_min_task_count(count) //设置单次最小任务量
```
同时均有 get 版本

提交非`noexcept`任务时，自动包装`try` `catch`并将异常置入返回的`future`

## 注意事项
#### 雷区
不要在任务中操纵组件，会阻塞线程
#### 接口安全性
|组件接口|是否线程安全|
| :-- | :--: |
|thread_pool|是|

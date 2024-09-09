#include <mutex>
#include <Chunk.h>

template <typename T>
class AsyncCircularQueue {
public:
    void Add(T item);
    T Get();
private:
    static const size_t LOAD_QUEUE_SIZE = 1024;
    std::mutex loadQueueLock;
    std::array<T, LOAD_QUEUE_SIZE> loadQueue;
    std::atomic_int head_;
    std::atomic_int tail_;
};
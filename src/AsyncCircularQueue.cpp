#include "AsyncCircularQueue.h"

template<typename T>
inline void AsyncCircularQueue<T>::Add(T item)
{
    std::lock_guard<std::mutex> guard(loadQueueLock);

    assert((tail_ + 1) % LOAD_QUEUE_SIZE != head_);

    // Add to the end of the list.
    loadQueue[tail_] = item;
    tail_ = (tail_ + 1) % LOAD_QUEUE_SIZE;
}

template<typename T>
T AsyncCircularQueue<T>::Get()
{
    // If there are no pending requests, do nothing.
    if (head_ == tail_) return NULL;

    std::lock_guard<std::mutex> guard(loadQueueLock);

    T item = loadQueue[head_];

    head_ = (head_ + 1) % LOAD_QUEUE_SIZE;

    return item;
}
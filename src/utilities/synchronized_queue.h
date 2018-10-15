#ifndef SYNC_QUEUE_H_
#define SYNC_QUEUE_H_
#include <list>
#include <mutex>
template<typename T>
class SynchronizedQueue {
public:
SynchronizedQueue(){}
void put(const T & data);
T get();
int size();

int size() {
  return queue.size();
}
private:
SynchronizedQueue(const SynchronizedQueue &)=delete;
SynchronizedQueue & operator=(const SynchronizedQueue &)=delete;
std::list<T> queue;
std::mutex myMutex;
// std::condition_variable myCv;
};

template<typename T>
void SynchronizedQueue<T>::put(const T & data)
{
std::unique_lock<std::mutex> lck(myMutex);
queue.push_back(data);
//myCv.notify_one();
}
template<typename T>
T SynchronizedQueue<T>::get() {
for(;;) {
std::unique_lock<std::mutex> lck(myMutex);
if(queue.empty()) continue;
T result=queue.front();
queue.pop_front();
return result;
}
}

#endif // SYNC_QUEUE_H_

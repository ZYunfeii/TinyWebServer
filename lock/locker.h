#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

// 信号量实现
class sem
{
public:
    sem()
    {
        // int sem_init(sem_t *__sem, int __pshared, unsigned int __value)
        // 其中__sem为信号量 __value指定信号量的初始值 __pshared为0时信号量被进程内线程共享 
        // 并且应该放置在这个进程的所有线程都可见的地址上（如全局变量，或者堆上动态分配的变量）
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            throw std::exception();
        }
    }
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    bool wait()
    {
        // sem_wait是一个函数，也是一个原子操作，它的作用是从信号量的值减去一个“1”，但它永远会先等待该信号量为一个非零值才开始做减法
        // 如果对一个值为0的信号量调用sem_wait()，这个函数就会原地等待直到有其它线程增加了这个值使它不再是0为止
        return sem_wait(&m_sem) == 0;
    }
    bool post()
    {
        // sem_post是给信号量的值加上一个“1”
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};

// 线程锁实现
class locker
{
public:
    locker()
    {
        // 如果参数attr为空(NULL)，则使用默认的互斥锁属性，默认属性为快速互斥锁
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

// 条件变量实现
// 条件变量（cond）使在多线程程序中用来实现“等待--->唤醒”逻辑常用的方法，是进程间同步的一种机制
// 一般条件变量有两个状态：（1）一个/多个线程为等待“条件变量的条件成立“而挂起；（2）另一个线程在“条件变量条件成立时”通知其他线程。
// pthread_cond_wait()
// 用于阻塞当前线程，等待别的线程使用pthread_cond_signal()或pthread_cond_broadcast来唤醒它 
// pthread_cond_wait() 必须与pthread_mutex配套使用。
// pthread_cond_wait()函数一进入wait状态就会自动release mutex。
// 当其他线程通过pthread_cond_signal()或pthread_cond_broadcast，把该线程唤醒，使pthread_cond_wait()通过（返回）时，该线程又自动获得该mutex。
// pthread_cond_signal函数的作用是发送一个信号给另外一个正在处于阻塞等待状态的线程,使其脱离阻塞状态,继续执行.如果没有线程处在阻塞等待状态,pthread_cond_signal也会成功返回。
// 使用pthread_cond_signal一般不会有“惊群现象”产生，他最多只给一个线程发信号。
// 假如有多个线程正在阻塞等待着这个条件变量的话，那么是根据各等待线程优先级的高低确定哪个线程接收到信号开始继续执行。
// 如果各线程优先级相同，则根据等待时间的长短来确定哪个线程获得信号。但无论如何一个pthread_cond_signal调用最多发信一次。

//----------------------------------函数解析----------------------------------------//
// 1.初始化一个条件变量 int pthread_cond_init(pthread_cond_t *restrict cond, const pthread_condattr_t *restrict attr); 

// 2.阻塞等待一个条件变量 int pthread_cond_wait(pthread_cond_t *restrict cond, pthread_mutex_t *restrict mutex); 
// 函数作用：1. 阻塞等待条件变量cond（参1）满足  2. 释放已掌握的互斥锁（解锁互斥量）相当于pthread_mutex_unlock(&mutex); 这两步为一个原子操作
//         3. 当被唤醒，pthread_cond_wait函数返回时，解除阻塞并重新申请获取互斥锁pthread_mutex_lock(&mutex);

// 3.限时等待一个条件变量 int pthread_cond_timedwait(pthread_cond_t *restrict cond, pthread_mutex_t *restrict mutex, const struct timespec *restrict abstime); 

// 4.唤醒至少一个阻塞在条件变量上的线程  int pthread_cond_signal(pthread_cond_t *cond); 

// 5.唤醒全部阻塞在条件变量上的线程  int pthread_cond_broadcast(pthread_cond_t *cond); 

// 6.销毁一个条件变量  int pthread_cond_destroy(pthread_cond_t *cond); 
class cond
{
public:
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            //pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex)
    {
        // 它在另一个线程中，用来接收信号
        // 必须和一个互斥锁配合，以防止多个线程同时请求pthread_cond_wait()
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex); // 阻塞等待一个条件变量
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()
    {
        // 它在一个线程中，用来发送信号
        return pthread_cond_signal(&m_cond) == 0; // 唤醒至少一个阻塞在条件变量上的线程
    }
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond; // 其本质是一个结构体。为简化理解，应用时可忽略其实现细节，简单当成整数看待。变量m_cond只有两种取值1、0。
};
#endif

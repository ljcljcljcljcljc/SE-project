#ifndef LOCKER_H
#define LOCKER_H

// 线程同步机制封装类
// 用于解决任务队列的同步问题（任务队列是临界区资源）
// 两个类：互斥锁类，信号量类


#include <exception>
#include <pthread.h>
#include <semaphore.h>


// 互斥锁类
class locker {
public:
    locker() {
        if(pthread_mutex_init(&m_mutex, NULL) != 0) { // 互斥锁初始化函数第二个参数为互斥锁的属性，设为NULL为默认互斥锁属性
            throw std::exception();         // 如果初始化失败，那么抛出异常
        }
    }

    ~locker() {
        pthread_mutex_destroy(&m_mutex);
    }

    // 上锁
    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0; // 返回值为0，说明上锁成功，否则说明上锁失败
    }

    // 解锁
    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    // 获取互斥量成员
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex; // 只有一个数据成员，即位互斥锁类型的变量
};


// 信号量类
class sem {
public:
    // 两个构造函数，一个无参，一个有参
    sem() {
        if( sem_init( &m_sem, 0, 0 ) != 0 ) {
            throw std::exception();
        }
    }
    // 无参构造函数，num为信号量初始值
    sem(int num) {
        if( sem_init( &m_sem, 0, num ) != 0 ) {
            throw std::exception();
        }
    }
    ~sem() {
        sem_destroy( &m_sem );
    }
    // 等待信号量，即P操作
    bool wait() {
        return sem_wait( &m_sem ) == 0;
    }
    // 释放信号量，即V操作
    bool post() {
        return sem_post( &m_sem ) == 0;
    }
private:
    sem_t m_sem;
};

#endif
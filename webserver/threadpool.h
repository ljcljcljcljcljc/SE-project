#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类，使其更为通用
template<typename T>
class threadpool {
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 8, int max_requests = 10000); // 构造函数，含有默认实际参
    ~threadpool();
    bool append(T* request); // 向请求队列中添加任务的方法成员

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    // worker回调函数，由于是一个可调用对象，所以必须为静态函数，
    // 由于无法访问非静态成员，所以传入的arg必须为this指针，才能让worker访问非静态成员
    static void* worker(void* arg);
    // run函数，工作线程实际执行的代码
    void run();

private:
    // 线程的数量
    int m_thread_number;  
    
    // 线程池容器，是一个数组就行了，这个就是我们的线程池
    // 描述线程池的数组，大小为m_thread_number    
    pthread_t * m_threads;

    // 请求队列中最多允许的、等待处理的请求的数量  
    int m_max_requests; 
    
    // 请求队列/工作队列，用容器list
    // 是所有线程共享的，是临界区资源
    std::list<T*> m_workqueue;  

    // 保护请求队列的互斥锁（用到的即为locker.h中定义的互斥锁类locker）
    locker m_queuelocker;   

    // 信号量，用来判断是否有任务需要处理
    // 它的值即为任务请求队列中现有的任务数量
    sem m_queuestat;

    // 是否结束线程的标志，
    // 线程池不终止，线程池里的线程就不会终止（叫池子麻）；线程池一旦终止，线程池里的线程也要终止      
    bool m_stop;                    
};

// 构造函数
template< typename T >
threadpool< T >::threadpool(int thread_number, int max_requests) : 
        m_thread_number(thread_number), m_max_requests(max_requests), 
        m_stop(false), m_threads(NULL) {

    if((thread_number <= 0) || (max_requests <= 0) ) { // 如果传递来的是负数，抛出异常
        throw std::exception();
    }

    // 通过new动态创建线程池
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads) { // 如果创建不成功，抛出异常
        throw std::exception();
    }

    // 创建thread_number 个线程
    // 并将他们设置为线程分离（使得当线程终止时，自动释放资源，无需再由父线程回收资源）
    for ( int i = 0; i < thread_number; ++i ) {
        printf( "create the %dth thread\n", i);
        if(pthread_create(m_threads + i, NULL, worker, this ) != 0) {
            // 线程创建函数：
            // 第一个参数为创建的线程要保存到那里，应传入指针类型
            // 第二个参数为线程属性，默认为NULL即可
            // 第三个参数为子线程要执行的代码，即回调函数，线程会跑到回调函数处执行代码，
            //     worker函数指针的类型为 void* worker(void* arg);
            // 第四个参数为需要传递给worker的实参arg（是一个指针类型）
            delete [] m_threads;        // 如果创建第i个线程失败，释放资源并抛出异常
            throw std::exception();
        }
        
        if( pthread_detach( m_threads[i] ) ) {
            delete [] m_threads;        // 如果分离第i个线程失败，释放资源并抛出异常
            throw std::exception();
        }
    }
}

// 析构函数
template< typename T >
threadpool< T >::~threadpool() {
    delete [] m_threads;        // 释放new出来的资源，即线程池资源
    m_stop = true;              // 设置线程停止标志位为真
}

// 向请求队列中添加任务
template< typename T >
bool threadpool< T >::append( T* request )
{
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    m_queuelocker.lock();
    if ( m_workqueue.size() > m_max_requests ) { 
        // 如果超出请求队列限定的任务数量最大值，函数返回false，添加任务失败
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();   // V操作，相当于向任务队列中增加了一个proudct（任务），让工作线程（消费者）去消费
    return true;
}

// 回调函数worker代码
template< typename T >
void* threadpool< T >::worker( void* arg )
{
    threadpool* pool = ( threadpool* )arg;  // 强制类型转换
    pool->run();                            // run函数，工作线程实际执行的代码
    return pool;                            // 此处worker函数的返回值其实没啥用
}

// run函数，工作线程实际执行的代码
template< typename T >
void threadpool< T >::run() {

    while (!m_stop) {
        m_queuestat.wait();         // P操作，工作线程消费一个product（任务）去处理
        m_queuelocker.lock();       // 由于要操作队列，所以要先对队列上锁
        if ( m_workqueue.empty() ) {
            m_queuelocker.unlock();
            continue;
        }
        // 走到这里，说明请求队列中有任务，可以进行处理
        T* request = m_workqueue.front();
        m_workqueue.pop_front();     // 取出任务
        m_queuelocker.unlock();      // 解锁，释放临界区资源
        if ( !request ) {
            continue;
        }
        request->process();          // 任务的实际处理（对http报文的实际处理）
    }

}

#endif

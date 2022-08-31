#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量

// 向epoll中添加文件描述符
extern void addfd( int epollfd, int fd, bool one_shot );
// 从epoll中移除文件描述符
extern void removefd( int epollfd, int fd );
// 修改epoll对象中的文件描述符
// #################原来的版本怎么没有，难道没有用这个函数马？###########
//extern void modfd(int epollfd, int fd, int ev);

// 添加信号捕捉
void addsig(int sig, void( handler )(int)){
    struct sigaction sa;                  // 创建sigaction结构体用于信号处理函数sigaction
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;              //  将信号处理函数设置为handler
    sigfillset( &sa.sa_mask );            // 设置临时阻塞信号集          
    assert( sigaction( sig, &sa, NULL ) != -1 );    
}


// main函数
// 需要在命令行中传入端口号
int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {
        // 提示用户需要传入端口号
        printf( "please set port: %s port_number\n", basename(argv[0]));
        return 1;
    }

    // 获取端口号，需要将字符串转换为整数
    int port = atoi( argv[1] );
    // 对SIGPIPE信号进行处理，本来默认操作是终止进程，但现在我们让他设置为SIG_IGN，即忽略该信号
    // 产生SIGPIPE信号的原因：在网络通信时，如果有一端断开连接了，另一端不知道，
    //          此时另一端还往缓冲区中写数据，就会产生SIGPIPE信号'
    addsig( SIGPIPE, SIG_IGN );

    // 程序一启动，就要初始化线程池
    // 创建线程池，初始化线程池，就是一个threadpool<http_conn>*类型（指针类型）
    // 注意不是一个数组类型（只有new一个，没有new数组）
    // http_conn就是任务类T（实例化模板类）
    // 有数据到达时，主线程负责读取数据，将读取到的数据封装为一个任务对象（即http_conn类型）
    // 插入到请求队列中，然后由工作线程来处理
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {  // 如果捕捉到异常，就退出程序
        return 1;
    }

    // 创建一个数组用于保存所有的客户端连接信息
    http_conn* users = new http_conn[ MAX_FD ];

    // -------- 下面的代码就是之前网络通信的代码

    // 创建监听套接字并进行初始化
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );  // port就是从命令行中传入的参数，我们main函数之前的代码有收集过

    // 设置端口复用（注意，设置端口复用一定要在bind之前设置）
    int reuse = 1; // 值为1，表示开启端口复用
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    // 绑定端口
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );

    // 设置监听
    // 第二个参数表示的是半连接队列和全连接队列的二者加起来的元素的最大值，
    // 一般不用太大，指定5就行，因为全连接队列不会存太多的，会立即被accept的
    ret = listen( listenfd, 5 );  

    // 创建epoll对象，和事件数组（即epoll_event数组）
    // 只应该创建一个epoll对象，多个epoll_event
    // 每个epoll_event对应一个
    int epollfd = epoll_create( 100 );       // epoll对象
    epoll_event events[ MAX_EVENT_NUMBER ];  // epoll_event数组
    // 将监听的文件描述符添加到epoll对象中
    // 监听的文件描述符不需要设置oneshot，所以第三个参数为false
    addfd( epollfd, listenfd, false );      // addfd是自己定义的向epoll对象中添加文件描述符的函数
    // 所有socket上的事件都被注册到同一个epoll内核事件中，m_epollfd是静态成员
    // （所有http_conn类的实例共享该静态成员）
    http_conn::m_epollfd = epollfd;


    // ----------------- 上面通信的准备工作完成
    // 下面就是循环处理事件，通信的代码
    while(true) {
        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            // 调用epoll失败
            printf( "epoll failure\n" );
            break;
        }

        // 循环遍历事件数组
        for ( int i = 0; i < number; i++ ) {
            int sockfd = events[i].data.fd;
            if( sockfd == listenfd ) {
                // 监听的文件描述符有事件，说明有客户端连接进来了

                // 调用accept接收新的客户端连接
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                // 返回值为新连接进来的客户端socket的文件描述符
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 
                // 如果连接数满了
                if( http_conn::m_user_count >= MAX_FD ) {
                    close(connfd);
                    continue;
                }

                // 将新连接进来的客户的数据（就是任务）初始化，然后放到users数组中
                // 为了方便起见，直接让文件描述符的值作为下标
                // 不可能有两个相同的文件描述符，所以不会冲突
                users[connfd].init( connfd, client_address);

            // --------------- 下面的都是非监听套接字的事件发生的处理

            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                // 如果检测到对方异常断开或者错误等事件

                users[sockfd].close_conn();   // 一个成员函数，专门用来关闭连接的函数

            } else if(events[i].events & EPOLLIN) {
                // 如果该fd是读事件发生

                if(users[sockfd].read()) {  // read函数一次性把数据读完
                    pool->append(users + sockfd);  // append的形参需要的是指针类型
                } else {
                    // 如果读取失败，相当于出现异常的情况，关闭连接
                    users[sockfd].close_conn();
                }

            }  else if( events[i].events & EPOLLOUT ) {
                // 如果该fd检测到的是写事件

                // 这里再说以下写事件是怎么产生的
                // 就是一个sockfd接收到了要读取的数据，处理http请求（比如GET）就要返回数据麻
                // 这个要返回的数据首先要由sokfd将写事件注册到epoll对象中，然后下次检测epoll对象时
                // 就把要返回的数据写入套接字中，返回给客户端。

                if( !users[sockfd].write() ) {  // write一次性发送完所有数据
                    // 发送HTTP响应
                    // 响应数据的准备过程已经在之前的read函数中完成了，此处write函数只需要负责发送就行了
                    // 实际上更确切的来说是一个send函数
                    
                    // write要发送的数据有两部分，就就是一个http响应
                    // 第一部分m_write_buf：包括响应行，响应头，响应空行
                    // 第二部分m_file_address：即响应体

                    // 如果write执行不成功，相当于出现异常的情况，关闭连接
                    users[sockfd].close_conn();
                }

            }
        }
    }
    
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}
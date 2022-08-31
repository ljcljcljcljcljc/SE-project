Linux轻量级Web服务器
参考牛客网C++项目

项目简述：
    在Linux环境下使用C++搭建轻量级web服务器，服务器能够支持相对数量的客户端并发访问并进行响应
（客户端使用GET请求访问服务器，服务器响应一个带有图片的web页面）。
环境：
    Ubuntu18.04  VSCode  C++
主要工作：
    • 利用Socket实现不同主机间的通信
    • 模拟Proactor模式处理事件
    • 利用线程池机制提供服务，增加并行服务数量
    • 使用互斥锁和信号量保证线程同步
    • 使用epoll实现I/O多路复用，提高服务器处理事件的效率
    • 使用有限状态机解析HTTP报文
    • 使用Webbench进行压力测试，本机最大支持10000个并发的http GET请求


文件目录：
webserver---|----README.txt
            |----（说明文档）
            |----mian.cpp
            |   （主函数）
            |----http_conn.h
            |   （http任务类）
            |----http_conn.cpp
            |   （http任务类实现）
            |----threadpool.h
            |   （线程池类）
            |----locker.h
            |   （互斥锁和信号量类，用于实现线程同步）
            |----resources------|----images
            |    (服务器资源)    |   （图像文件）
            |                   |----index.html
            |                   |   （web页面）
            |----presure_test----webbench-1.5
            |                   （使用webbench进行压力测试）



如何运行（Linux下）：
（1）client-server的测试
    进入webserver目录，使用下述命令编译源文件
        g++ *.cpp -o server -pthread
    运行server文件并指定端口
        ./server 10000
    打开web浏览器，在地址栏中输入下述命令进行测试（其中ip地址应修改为本机ip）
        http://172.26.70.100:10000/index.html
    如果测试成功，在web浏览器中会看到一张柯基小狗图片
（2）使用webbench进行压力测试
    进入webserver/presure_test/webbench-1.5/目录下
    使用下述make命令编译文件
        make
    会生成webbench可执行文件，运行下述命令进行测试
        ./webbench -c 10000 -t 10 http://127.26.70.100:10000:/index.html
    （-c 10000）表示并发10000个http GET请求
    （-t 10）表示测试10秒钟
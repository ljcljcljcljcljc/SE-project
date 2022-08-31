#include "http_conn.h"

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录（就是网站资源的路径）
const char* doc_root = "/home/ljchen/webserver/resources";


// 设置文件描述符为非阻塞的函数
int setnonblocking( int fd ) {
    // 使用fcntl函数设置文件描述符的flags属性，将其设置为非阻塞
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;  // 就是在原有flags的基础上加上一个O_NONBLOCK的标志
    fcntl( fd, F_SETFL, new_option );
    return old_option;   // 返回值其实没啥用，不接收也行
}

// 向epoll中添加需要监听的文件描述符
// 最后一个形参表示是否需要one_shot
void addfd( int epollfd, int fd, bool one_shot ) {
    epoll_event event;
    event.data.fd = fd;
    // 默认为水平触发模式
    // 如果对方连接断开，会触发EPOLLRDHUP事件，这样就不用通过返回值来判断对方是否断开了
    event.events = EPOLLIN | EPOLLRDHUP;   
    if(one_shot) 
    {
        // 防止同一个socket被不同的线程处理
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞，否则你读完数据的话，线程就会阻塞在那里
    // 自定义的函数
    setnonblocking(fd);  
}

// 从epoll中移除监听的文件描述符
void removefd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

// ----- 静态变量的值必须初始化
// 所有的客户数，所有http_conn共用一个m_user_count
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;


// 关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;   // 置为-1即表示该http_conn没有用了
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}


// 初始化连接,外部调用初始化套接字地址
// 这个函数其实进行了http_conn类的初始化工作
void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;
    
    // 设置端口复用
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    // 将新连接进来的sockfd加入到epoll对象中
    addfd( m_epollfd, sockfd, true );

    // 更新m_user_count
    // 此处m_user_count不用考虑互斥问题和数据同步问题
    // 因为m_user_count只有主线程进行操作，工作线程不会操作m_user_count
    // 主线程只有一个，当然不用考虑互斥同步问题了
    m_user_count++; 
    init(); // 初始化连接的其他数据
}


// 初始化连接的其他数据
void http_conn::init()
{
    // ---------- 这个函数初始化的数据成员大部分是http报文的一些字段
    // ---------- 真正的实际开发我们应该使用库

    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              // 要获取的文件资源 
    m_version = 0;          // http版本号
    m_content_length = 0;   //  
    m_host = 0;             //
    m_start_line = 0;       // 
    m_checked_idx = 0;      // 
    m_read_idx = 0;         // 
    m_write_idx = 0;        // 
    bzero(m_read_buf, READ_BUFFER_SIZE);    // 清空读缓冲
    bzero(m_write_buf, READ_BUFFER_SIZE);   // 清空写缓冲
    bzero(m_real_file, FILENAME_LEN);       // 
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    // 如果要读的数据大于缓冲区的大小，返回失败
    if( m_read_idx >= READ_BUFFER_SIZE ) {
        return false;
    }
    // 读取到的字节（就是recv函数的返回值）
    int bytes_read = 0;
    while(true) {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 );
        if (bytes_read == -1) {
            if( errno == EAGAIN || errno == EWOULDBLOCK ) {
                // 没有数据
                break;
            }
            return false;  // 否则就是出错的情况，返回失败 
        } else if (bytes_read == 0) {   // 对方关闭连接
            return false;
        }
        // 否则走到这里就是能正确的读到数据，bytes_read > 0
        m_read_idx += bytes_read;
    }
    return true;
}

// 解析一行，判断依据\r\n（就是从状态机了）
http_conn::LINE_STATE http_conn::parse_line() {
    char temp;
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx ) { // 条件：检查的索引要小于已经读的索引
        temp = m_read_buf[ m_checked_idx ]; // 逐个字符地进行检查
        if ( temp == '\r' ) { 
            if ( ( m_checked_idx + 1 ) == m_read_idx ) {
                // 这种情况我们认为是不完整的数据
                return LINE_OPEN;
            } else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' ) {
                // 这种情况就是\r\n连起来了，
                // 将\r\n都设置为字符串结束符（方便为了后面的读取）
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } 
        else if( temp == '\n' )  {
            if( ( m_checked_idx > 1) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) ) {
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    // 如果上述return都没有执行，那么就返回一个LINE_OPEN，说明数据还不完整
    return LINE_OPEN;
}

// 解析HTTP请求行，获得请求方法，目标URL,以及HTTP版本号
// 我们只支持GET方法和http版本1.1
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // 解析类似下面这一行信息
    // GET /index.html HTTP/1.1
    // 其实就是三个信息：请求方法，url，http协议版本号

    // strpbrk是在源字符串(s1)中找出最先含有搜索字符串(s2)中任一字符的位置并返回,若找不到则返回空指针。
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (! m_url) { 
        return BAD_REQUEST;
    }

    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符（先解引用将m_url对应位置的字符设置为\0，然后将m_url往后移一个位置）
    char* method = text; // 此时method即是GET\0/index.html HTTP/1.1，即GET
    // strcasecmp是忽略大小写的比较
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较，如果是GET方法
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }

    // 此时m_url为：/index.html\0HTTP/1.1
    // 或者：http://172.168.1.3:10000/index.html\0HTTP/1.1（举例而已）
    // strncasecmp函数比strcasecmp多了一个长度参数
    // strncasecmp()用来比较参数s1和s2字符串前n个字符，比较时会自动忽略大小写的差异。
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        // strchr函数功能为在一个串中查找给定字符的第一个匹配之处。
        m_url = strchr( m_url, '/' );
    }
    // 此时m_url应该为：/index.html\0HTTP/1.1 即：/index.html
    if ( !m_url || m_url[0] != '/' ) { // 如果不是，就返回错误
        return BAD_REQUEST;
    }

    // 更改检查状态（主状态机状态）为检查请求头状态
    m_check_state = CHECK_STATE_HEADER; 
    return NO_REQUEST;
}

// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {   
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求（也就是说这个HTTP请求没有请求体，只有请求头和请求行）
        return GET_REQUEST;
    } 
    // ----------- 下面就是请求头的各个字段的处理（我们只做简单的分析，不解析太多字段，只解析必要字段）
    // Connection字段
    else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } 
    // Content-Length字段
    else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } 
    // Host字段
    else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } 
    // 未知字段
    else {
        printf( "oop! unknow header %s\n", text );
    }

    return NO_REQUEST;
}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text ) {
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    HTTP_CODE ret = NO_REQUEST;     
    LINE_STATE line_state = LINE_OK;  // 从状态机初始状态定义为LINE_OK
    char* text = 0;     // 要读取的数据
    
    // while循环的两个条件（注意：条件顺序不可改变）
    // 1 解析到了请求体
    // 2 或者解析到了完整的一行数据
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_state == LINE_OK))
                || ((line_state = parse_line()) == LINE_OK)) {
        // 获取一行数据，一行一行地解析，所以while循环每一次需要一行一行地获取数据，用自定义的get_line函数
        text = get_line();
        // m_checked_idx当前正在分析的字符在读缓冲区中的位置（因为我们解析报文肯定也是一个一个字符往后遍历的）
        // m_start_line当前正在解析的行的第一个字符（即该行的起始位置）在所有报文字符中的位置
        m_start_line = m_checked_idx;   
        printf( "got 1 http line: %s\n", text );

        switch ( m_check_state ) {
            case CHECK_STATE_REQUESTLINE: {
                // 解析请求行
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST ) { // 如果检测到语法错误，直接结束
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                // 解析请求头
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    // 如果是一个正确的GET请求，那么就由do_request具体进行解析
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                // 解析请求体
                ret = parse_content( text );
                if ( ret == GET_REQUEST ) {
                    // 如果是一个正确的GET请求，那么就由do_request具体进行解析
                    return do_request();
                }
                line_state = LINE_OPEN;
                break;
            }
            default: {
                // 否则读取失败
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    // 资源路径
    // "/home/ljchen/webserver/resources"
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    // 拼接资源路径和要请求的文件名（即m_url）得到真正的要请求的文件路径
    // FILENAME_LEN指的是一个文件名能有的最大长度
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射，把文件映射到内存当中
    // 第一个参数为NULL表示让OS指定内存映射区的位置
    // PROT_READ表示只读权限
    // MAP_PRIVATE表示内存映射区的文件与磁盘上的文件不同步（就是内存映射区的文件改变了，磁盘文件不会改变）
    // fd需要操作的/映射的文件的文件描述符
    // 最后一个参数为0表示从内存映射区从文件开始位置偏移0开始映射（一般为0就行）
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作（释放内存映射区的资源）
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

// 发送HTTP响应
// 响应数据的准备过程已经在之前的read函数中完成了，此处write函数只需要负责发送就行了
// 实际上更确切的来说是一个send函数
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;    // 已经发送的字节
    int bytes_to_send = m_write_idx;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // writev表示分散写（库函数），将分散的多块内存的数据写出去
        // 我们这里其实就是两块分散的内存，一个是m_write_buf，一个是m_file_address
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_to_send <= bytes_have_send ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            // 成功写完数据之后，释放内存映射，从新设置检测事件
            unmap();
            if(m_linger) {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            } else {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}

// 往写缓冲中写入待发送的数据
// 就是具体的可变参数操作函数
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        // 如果当前要写的数据大于写缓冲区的大小，返回失败
        return false;
    }
    // --------- 下面的操作就是将传入的参数数据写入到写缓冲区m_write_buf里
    va_list arg_list; // 多参数列表
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

// 添加响应行（类似：http/1.1 200 OK)
bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

// 添加响应头
bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

// 添加响应体（如果请求不到数据，添加的是返回的错误信息）如果能正确请求到数据，就没有这个cotent字段了
bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

// 添加响应头的Content-Length字段
bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

// 添加响应头的Connection字段（就是是否keep-alive）
bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

// 添加响应头和响应体之间的空白行
bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}



// 添加响应头的Content-Type字段
bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}


// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            // 如果是正确的数据
            // 数据部分就包括两部分m_write_buf和m_file_address
            // 后续会用到write函数分散写
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数，是http_conn类的成员函数
void http_conn::process() {
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST ) {
        // 如果请求不完整，需要重新检测该socket上的读事件，然后再读取数据检测
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }
    
    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}
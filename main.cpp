#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "heap_timer.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5

extern int setnonblocking( int fd );
extern int addfd( int epollfd, int fd, bool one_shot );
extern int removefd( int epollfd, int fd );
static int pipefd[2];
static time_heap t_h(10);

void addsig( int sig, void( handler )(int), bool restart = true ) {
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart ) {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

void timer_handler()
{
    printf("tick\n");
    /* 定时处理任务，实际上就是调用tick函数 */
    t_h.tick();
    /* 因为一次alarm调用只会引起一次SIGALRM信号，所以我们要重新定时，以不断触发SIGALRM信号 */
    time_t timeout_value=TIMESLOT;
    if(!t_h.empty()){
    	timeout_value=t_h.top()->expire-time(NULL);
    }
    if (timeout_value <= 0) {
        // 如果计算出的超时时间小于等于0，说明有定时器已经超时或即将超时
        // 应该尽快再次检查。设置为1秒可以避免alarm(0)可能带来的未定义行为或立即触发
        timeout_value = 1;
    }
    
    alarm( timeout_value );
    
    
}

/* 定时器回调函数，它删除非活动socket上的注册事件，并关闭之 */
void cb_func( http_conn* user )
{
    printf( "close fd %d\n", user->m_sockfd );
    user->close_conn();
    
}

void show_error( int connfd, const char* info ) {
    printf( "%s", info );
    send( connfd, info, strlen( info ), 0 );
    close( connfd );
}

int main( int argc, char* argv[] ) {
    if( argc <= 2 ) {
        printf( "usage: %s ip_address port_number\n", basename( argv[0] ) );
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi( argv[2] );

    /* 忽略SIGPIPE信号 */
    addsig( SIGPIPE, SIG_IGN );
    addsig( SIGALRM,sig_handler);
    /* 创建线程池 */
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool< http_conn >;
    } catch( ... ) {
        return 1;
    }

    /* 预先为每个可能的客户连接分配一个http_conn对象 */
    http_conn* users = new http_conn[ MAX_FD ];
    
    assert( users );
    int user_count = 0;

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );
    struct linger tmp = { 1, 0 };
    setsockopt( listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof( tmp ) );

    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret >= 0 );

    ret = listen( listenfd, 5 );
    assert( ret >= 0 );
    
    ret = socketpair( PF_UNIX, SOCK_STREAM, 0, pipefd );
    assert( ret != -1 );

    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd;
    
    setnonblocking( pipefd[1] );
    addfd( epollfd, pipefd[0],false );
    
    bool timeout = false;
    alarm( TIMESLOT );  /* 定时 */
    while( true ) {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ ) {
            int sockfd = events[i].data.fd;
            heap_timer* timer = users[sockfd].timer;
            if( sockfd == listenfd ) {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                }
                if( http_conn::m_user_count >= MAX_FD ) {
                    show_error( connfd, "Internal server busy" );
                    continue;
                }
                /* 初始化客户连接 */
                users[connfd].init( connfd, client_address );
             
            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                /* 如果有异常，直接关闭客户连接 */
                cb_func( &users[sockfd] );
                if( timer )
                {
                    t_h.del_timer( timer );
                }
            }else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) )
            {
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 )
                {
                    // handle the error
                    continue;
                }
                else if( ret == 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        switch( signals[i] )
                        {
                            case SIGALRM:
                            {
                                /* 用timeout变量标记有定时任务需要处理，但不立即处理定时任务。
                                 * 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。 */
                                timeout = true;
                                break;
                            }
                            
                        }
                    }
                }
            } else if( events[i].events & EPOLLIN ) {
            	if( timer )
                    {
                        t_h.del_timer( timer );
                    }
                /* 根据读的结果，决定是将任务添加到线程池，还是关闭连接 */
                if( users[sockfd].read() ) {
                    pool->append( users + sockfd );
                } else {
                    cb_func( &users[sockfd] );
                    if( timer )
                    {
                        t_h.del_timer( timer );
                    }
                }
            } else if( events[i].events & EPOLLOUT ) {
                /* 根据写的结果，决定是否关闭连接 */
                if( !users[sockfd].write() ) {
                    cb_func( &users[sockfd] );
                    if( timer )
                    {
                        t_h.del_timer( timer );
                    }
                }
                else{
                    if( users[sockfd].m_linger==true ){
                        /*创建定时器*/
                	heap_timer* timer = new heap_timer( TIMESLOT*2 );
                	timer->user_data = &users[sockfd];
                	timer->cb_func = cb_func;
                	users[sockfd].timer = timer;
                	t_h.add_timer(timer);
                	if(timer==t_h.top()){
                    	    time_t timeout = timer->expire - time(NULL);
                    	    // 确保超时时间是正数
                    	    alarm(timeout > 0 ? timeout : 1); 
                	}
                    }
                }
            } else {}
        }
        if( timeout )
        {
            timer_handler();
            timeout = false;
        }
    }

    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}

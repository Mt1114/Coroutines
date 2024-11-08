#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <iostream>

#define MAX_EVENTS 10
#define PORT 8888

int main()
{
    int listen_fd, conn_fd, epoll_fd, event_count;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    struct epoll_event events[MAX_EVENTS], event;
    // 创建监听套接字
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        return -1;
    }
    // 设置服务器地址和端⼝
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // 绑定监听套接字到服务器地址和端⼝
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("bind");
        return -1;
    }

    // 监听连接
    if (listen(listen_fd, 5) == -1)
    {
        perror("listen");
        return -1;
    }
    // 创建 epoll 实例

    if ((epoll_fd = epoll_create1(0)) == -1)
    {
        perror("epoll_create1");
    }
    // 添加监听套接字到 epoll 实例中
    event.events = EPOLLIN;
    event.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) == -1)
    {
        perror("epoll_ctl");
        return -1;
    }
    while (1)
    {
        // 等待事件发⽣
        event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        if (event_count == -1)
        {
            perror("epoll_wait");
            return -1;
        }
        // 处理事件
        for (int i = 0; i < event_count; i++)
        {
            if (events[i].data.fd == listen_fd)
            {
                // 有新连接到达
                conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
                std::cout << "New client connected, fd: " <<conn_fd << std::endl; // 打印新连接信息
                if (conn_fd == -1)
                {
                    perror("accept");
                    continue;
                }

                // 将新连接的套接字添加到 epoll 实例中
                event.events = EPOLLIN | EPOLLET;
                event.data.fd = conn_fd;
                fcntl(conn_fd, F_SETFL, O_NONBLOCK);
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &event) == -1)
                {
                    perror("epoll_ctl");
                    return -1;
                }
            }
            else
            {
                // 有数据可读
                while (1)
                {
                    char buf[1024];
                    int len = recv(events[i].data.fd, buf, sizeof(buf) - 1, 0);
                    usleep(1);
                    // 发⽣错误或连接关闭，关闭连接

                    if (len <= 0)
                    {
                        if (len == -1 && errno == EAGAIN)
                            break;
                        close(events[i].data.fd);
                    }
                    else
                    {
                        // printf("接收到消息：%s\n", buf);
                        // 发送回声消息给客户端
                        buf[len] = '\0';
                        send(events[i].data.fd, buf, len, 0);
                    }
                }
            }
        }
    }
    // 关闭监听套接字和 epoll 实例
    close(listen_fd);
    close(epoll_fd);
    return 0;
}
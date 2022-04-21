#include "tools.h"

void tools::m_sleep(int timeout){
    struct timeval sTime;
    sTime.tv_sec = 0;
    sTime.tv_usec = timeout * 1000; // ms
    select(0, NULL, NULL, NULL, &sTime);
}

void tools::save_diary2txt(char* text, char* username, char* file_root){
    char data[1024];
    std::ofstream outfile;
    char c[128];
    sprintf(c, "/%s.txt", username);
    strcat(file_root, c);
    outfile.open(file_root, std::ios::app);
    outfile << text << '\n';
}

void tools::cpp_write_html(char* data_show){
    std::ofstream myhtml;
    myhtml.open("./root/my_dynamic_html.html", std::ios::out | std::ios::trunc);
    myhtml << "<!DOCTYPE html>";
    myhtml << "<html>";
    myhtml << "<head><meta charset=\"UTF-8\"><title>elegant</title></head>";
    myhtml << "<body><br/><br/>";

    char src[1024];
    sprintf(src, "<div align=\"center\"><font size=\"5\"> <strong>%s</strong></font></div>", data_show);
    myhtml << src;

    myhtml << "</body>";
    myhtml << "</html>";
}

//对文件描述符设置非阻塞
int tools::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void tools::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void tools::removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 将事件重置为EPOLLONESHOT
// 对于注册了EPOLLONESHOT事件的文件描述符，操作系统最多触发其上注册的一个可读、可写或者异常事件，且只触发一次
// EPOLLRDHUP是从Linux内核2.6.17开始由GNU引入的事件。 当socket接收到对方关闭连接时的请求之后触发，有可能是TCP连接被对方关闭，也有可能是对方关闭了写操作。
void tools::modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event); // 修改epollfd上的注册事件
}
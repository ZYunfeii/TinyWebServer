#ifndef __TOOLS_H__
#define __TOOLS_H__
#include <time.h>
#include <sys/socket.h>
#include <iostream>
#include <fstream>
#include <string.h>
#include <signal.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>

namespace  tools{
void m_sleep(int timeout);
void save_diary2txt(char* text, char* username, char* file_root);
void cpp_write_html(char* data_show);
int setnonblocking(int fd);
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
void removefd(int epollfd, int fd);
void modfd(int epollfd, int fd, int ev, int TRIGMode);
}
#endif
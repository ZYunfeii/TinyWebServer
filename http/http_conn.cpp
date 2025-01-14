#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>
#include <time.h>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;
locker m_lock;
map<string, string> users;
Cookie *m_cookie = Cookie::get_instance();        // 获取Cookie单例
 
void http_conn::binary_write_data(){
    // std::cout<<m_string<<std::endl;
    char* ptr_real_data = m_string;
    int count_r_n = 4;
    while(count_r_n){
        if((*ptr_real_data == '\r') && (*(ptr_real_data + 1) == '\n')){
            count_r_n--;
        }
        ptr_real_data++;
    }
    ptr_real_data++;
    int offset = ptr_real_data - m_string;

    ofstream ofs;
    ofs.open("./user_messages/fileuploadData", ios::out | ios::binary);
    ofs.write(ptr_real_data, m_content_length - 44 - offset);
    ofs.close();
}

inline void http_conn::construct_file_path(char *file_path, int len){
    char *m_url_real = (char *)malloc(sizeof(char) * 200);
    strcpy(m_url_real, file_path);
    strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

    free(m_url_real);
}

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    
    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string name(row[0]);
        string passwd(row[1]);
        users[name] = passwd;
        m_cookie->add_user_str(name);
    } 
}

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        tools::removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    tools::addfd(m_epollfd, sockfd, true, m_TRIGMode); // 把和client的tcp套接字加入epollfd
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    if_use_cookie = false;  // 一开始发送报文不带cookie

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    // 把\r \n换成\0后返回LINE_OK
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 循环读取客户数据，直到无数据可读或对方关闭连接
// 非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()  // 线程池将自动调用它读取消息
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0); // ssize_t recv(int sockfd,void *buf,size_t len,int flags)
        m_read_idx += bytes_read; // m_read_idx 当前读到的位置

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据 （ET只会触发一次，所以要一次性读完）
    else
    {
        // 弥天bug，没想通原理。项目在调试的时候断点加在recv后就能上传，如果不加断点上线运行就不行
        
        // tools::m_sleep(200);
        int count = 0; 
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            std::cout<<bytes_read<<std::endl;
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK){ // EAGAIN的意思：就是要你再次尝试 这两个标志是一个东西 意思就是资源短暂不可用
                    break;
                }
                std::cout<<"errno:"<<errno<<std::endl;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
// 为方便理解，给出一个HTTP请求报文案例：
// GET /somedir/page.html HTTP/1.1
// Host:www.someschool.edu
// Connection:Close
// User-agent:Mozilla/5.0
// Accept-language:fr
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t"); // 返回的是制表符位置的指针
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0'; // 把制表符换成\0后加1
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET; // 浏览器输入网址后是GET
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1; // POST就要调用cgi了
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t"); // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标 （这句话相当于没有加）
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t"); //（这句话相当于没有加）
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER; // 下一步将进行switch中的CHECK_STATE_HEADER
    return NO_REQUEST;
}

//解析http请求的一个头部信息
// 为方便理解，给出一个HTTP请求报文案例：
// POST / HTTP1.1
// Host:www.wrox.com
// User-Agent:Mozilla/4.0 (compatible; MSIE 6.0; Windows NT 5.1; SV1; .NET CLR 2.0.50727; .NET CLR 3.0.04506.648; .NET CLR 3.5.21022)
// Content-Type:application/x-www-form-urlencoded
// Content-Length:40
// Connection: Keep-Alive
// 空行
// name=Professional%20Ajax&publisher=Wiley
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 这个函数是循环被执行的（因为首部行有多个，当所有行信息都读入后text[0] == '\0'）
    if (text[0] == '\0') // 空行后面有数据部分
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT; 
            return NO_REQUEST;
        }
        return GET_REQUEST; // m_content_length == 0 说明这是一个请求HTTP报文
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t"); // 该函数返回 str1 中第一个不在字符串 str2 中出现的字符下标
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else if (strncasecmp(text, "Cookie:", 7) == 0)
    {
        text += 7;
        text += strspn(text, " \t");
        int i = 0;
        while(text[i] != '=') ++i;
        strcpy(m_recv_cookie, text + i + 1);
        std::cout<<"Cookie from client:"<<m_recv_cookie<<std::endl;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 判断http请求是否被完整读入
// text传入时候是数据部分的头指针
// m_checked_idx正常情况也指向数据部分头指针前一个？
// m_read_idx正常情况下指向数据尾部
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为数据
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主从状态机解析HTTP 主状态机内部调用从状态机 从状态机驱动主状态机
// 每解析一部分都会将整个请求的m_check_state状态改变，状态机也就是根据这个状态来进行不同部分的解析跳转
// parse_line从状态机
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line(); // 获取缓存中的消息（已经通过parse_line处理好一行的内容了也就是末尾为\0可以直接通过char*接收这一行内容）
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        // 第一次进走CHECK_STATE_REQUESTLINE，解析HTTP报文的请求行
        case CHECK_STATE_REQUESTLINE: 
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        // 在parse_request_line中m_check_state改为CHECK_STATE_HEADER
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request(); // 正常第一次第三步进这个
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

// http响应核心逻辑在下面这个函数中，扩展web服务器功能、二次开发在这里进行
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);  // doc_root就是当前server运行的根目录
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) // 2 和 3是登录和注册
    {
        
        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2); // 把 src 所指向的字符串追加到 dest 所指向的字符串的结尾
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&password=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        // HTML中：<form action="3CGISQL.cgi" method="post">，所以这里检验‘3’就可以了
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据

            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end()) // 没有重名的
            {
                if_use_cookie = true; // 开启cookie
                strcpy(m_send_cookie, MD5::md5_encryption(name).c_str());  // cookie设置为用户name MD5码
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        else if (*(p + 1) == '2') 
        {
            //如果是登录，直接判断
            //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
            // HTML中：<form action="2CGISQL.cgi" method="post">，所以这里检验‘2’就可以了
            
            if (users.find(name) != users.end() && users[name] == password){
                if_use_cookie = true;
                strcpy(m_send_cookie, MD5::md5_encryption(name).c_str());
                strcpy(m_url, "/welcome.html");
            }
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0'){ // 0 1是写在html中的，点击网页按钮会发布post报文
        construct_file_path("/register.html", len);
    }
    else if (*(p + 1) == '1' && *(p + 2) == '2'){
        construct_file_path("/log.html", len);
    }
    else if (*(p + 1) == '5'){
        construct_file_path("/picture.html", len);
    }
    else if (*(p + 1) == '6'){
        construct_file_path("/video.html", len);
    }
    else if (*(p + 1) == '7'){
        construct_file_path("/fans.html", len);
    }
    else if (*(p + 1) == '8'){   // 日记功能
        construct_file_path("/diary.html", len);
    }
    else if (*(p + 1) == 'a' || *(p + 1) == 'b')   // 日记提交功能
    {
        // 获取文本框输入
        char diary_content[100];
        int i = 0;
        while(m_string[i] != '=') ++i;
        strcpy(diary_content, m_string + i + 1);

        if(*(p + 1) == 'a'){
            // 目前cookie就是user_name MD5
            std::string md5_str = m_recv_cookie;
            char *user_str = (char*)m_cookie->find_user_from_md5(md5_str).c_str(); 
            if (strcmp(user_str, "No user find") != 0){
                char sql_update[256];
                sprintf(sql_update, "update user set content =  '%s' where username = '%s'", diary_content, user_str);

                m_lock.lock();
                // 可能多个线程同时修改数据库，因此要加锁
                int res = mysql_query(mysql, sql_update);
                m_lock.unlock();
                if (res) std::cout<<"diary insert fail"<<std::endl;

                char folder_path[128] = "./user_messages";
                tools::save_diary2txt(diary_content, user_str, folder_path);
                construct_file_path("/diary.html", len);
            }else{
                construct_file_path("/judge.html", len);
            }
        }else if(*(p + 1) == 'b'){
            std::string md5_str = m_recv_cookie;
            char *user_str = (char*)m_cookie->find_user_from_md5(md5_str).c_str(); 
            if (strcmp(user_str, "No user find") != 0){
                char sql_get[256];
                sprintf(sql_get, "select replay from user where username = '%s'", user_str);
                int ret = mysql_query(mysql, sql_get);
                if(ret != 0) std::cout << "mysql get replay error" << std::endl;
                MYSQL_RES* sql_res;
                MYSQL_ROW row;
                sql_res = mysql_store_result(mysql);
                row = mysql_fetch_row(sql_res); // 默认第一个匹配内容
                tools::cpp_write_html(row[0]);
                construct_file_path("/my_dynamic_html.html", len);
            }else{
                construct_file_path("/judge.html", len);
            }
        }   
    }
    else if(*(p + 1) == 'c'){ // 进入上传文件界面
        construct_file_path("/fileupload.html", len);
    }
    else if(*(p + 1) == 'd'){ // client提交上传文件post请求
        binary_write_data();
        construct_file_path("/fileupload.html", len);
    }
    else if(*(p + 1) == 'e'){ // 个人博客
        construct_file_path("/index.html", len);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1); // 把 src 所指向的字符串复制到 dest，最多复制 n 个字符。当 src 的长度小于 n 时，dest 的剩余部分将用空字节填充

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY); // 打开请求的资源
    // 将fd对应的文件全部映射到一块随机分配的内存（第一个参数为0随机分）
    // PORT_READ表示内存段可读
    // MAP_PRIVATE表示内存段为调用进程所私有，对内存段的修改不会反映到被映射的文件中（也就是m_real_file不会被影响）
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0); 

    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        tools::modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp =  writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                tools::modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            tools::modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}

bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    if (if_use_cookie) {
        // Add Cookies
        return add_content_length(content_len) && add_linger() 
                && add_cookie(m_send_cookie) && add_blank_line();
    }else{
        return add_content_length(content_len) && add_linger() &&
           add_blank_line();
    }   
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::add_cookie(const char *cookie){
    return add_response("Set-Cookie:sessionid=%s; HttpOnly; Path=/\r\n", cookie);
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST: // 正常情况
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 调用这个之前线程会先调用read_once把套接字中的数据读到http_conn的缓存中
// reactor是在子线程中read_once的，proactor是在主线程中read_once的
void http_conn::process() 
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) // 没有请求 
    {
        tools::modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    tools::modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}

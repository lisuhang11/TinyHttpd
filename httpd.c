#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdint.h>

/*
功能：检查字符 x 是否为空白字符。
实现：通过调用标准库函数 isspace() 实现。
*/
#define ISspace(x) isspace((int)(x))

// 功能：定义 HTTP 响应中服务器标识头字段的内容
#define SERVER_STRING "Server: jdbhttpd/0.1.0\r\n"
// 定义 UNIX/Linux 系统中标准输入输出的文件描述符常量
#define STDIN   0
#define STDOUT  1
#define STDERR  2

// 函数声明
void accept_request(void *);
void bad_request(int);
void cat(int, FILE *);
void cannot_execute(int);
void error_die(const char *);
void execute_cgi(int, const char *, const char *, const char *);
int get_line(int, char *, int);
void headers(int, const char *);
void not_found(int);
void serve_file(int, const char *);
int startup(u_short *);
void unimplemented(int);

/************************************************* 
* accept_request 是一个处理 HTTP 请求的核心函数，
* 它从客户端接收 HTTP 请求，解析请求内容，
* 然后根据请求类型和资源特性决定是返回静态文件还是执行 CGI 程序。
* 这个函数体现了 Web 服务器的基本工作原理。
*
* 参数 arg：客户端套接字描述符，通过 (intptr_t) 转换为整数类型
**************************************************/

void accept_request(void *arg){
    int client = (intptr_t)arg;
    char buf[1024];     //buf 用于读取客户端请求
    size_t numchars;
    char method[255];   //存储 HTTP 方法
    char url[255];      //存储请求的 URL 
    char path[512];     //存储服务器文件系统路径
    size_t i, j;
    struct stat st;     //stat 结构体用于获取文件属性
    int cgi=0;          //CGI 标志：cgi 决定是否作为 CGI 程序执行，初始为 0（否）
    char* query_string = NULL;

    // 调用 get_line 读取 HTTP 请求的第一行（如 GET /index.html HTTP/1.1）
    numchars =get_line(client,buf,sizeof(buf));
    i=0;
    j=0;
    // 提取 HTTP 方法（如 GET、POST），并检查是否为支持的方法
    while(!ISspace(buf[i])&&i<(sizeof(method)-1)){
        method[i]=buf[i];
        i++;
    }
    j=i;
    method[i]='\0';
    // 如果不是 GET 或 POST，调用 unimplemented 返回 501 错误
    if(strcasecmp(method,"GET")&&strcasecmp(method,"POST")){
        unimplemented(client);
        return;
    }
    // POST 请求默认标记为 CGI 处理
    if(strcasecmp(method,"POST")==0){
        cgi=1;
    }
    i=0;
    while(ISspace(buf[j])&&(j<numchars)){
        j++;
    }
    // 提取 URL：从请求行中提取 URL 部分（如 /index.html）
    while(!ISspace(buf[j])&&(j<numchars)&&(i<sizeof(url)-1)){
        url[i]=buf[j];
        i++;
        j++;
    }
    url[i]='\0';

    // 查询字符串的处理
    // 对于 GET 请求，查找 ? 字符分割路径和查询参数
    // 例如 GET /search?query=test 会被分割为路径 /search 和查询字符串 query=test
    // 带有查询字符串的 GET 请求标记为 CGI 处理
    if(strcasecmp(method,"GET")==0){
        query_string = url;
        while((*query_string!='?')&&(*query_string!='\0')){
            query_string++;
        }
        if(*query_string=='?'){
            cgi=1;
            *query_string='\0';
            query_string++;
        }
    }
    // 将 URL 映射到服务器文件系统路径（如 / → htdocs/index.html）
    sprintf(path,"htdocs%s",url);
    // 如果请求的是目录（路径以 / 结尾），自动添加 index.html
    if(path[strlen(path)-1]=='/'){
        strcat(path,"index.html");
    }
    // 如果文件不存在，读取并忽略剩余的 HTTP 头，然后调用 not_found 返回 404 错误
    if(stat(path,&st)==-1){
        // 读取并丢弃 HTTP 请求中除了首行之外的所有头部字段，直到遇到一个空行（即\r\n）
        // 服务器必须正确解析头部，但对于静态文件请求，通常只需要请求行中的信息（如 URL），因此可以忽略头部字段。

        /*循环条件：
        * numchars > 0：确保读取未结束（连接未关闭）
        *strcmp("\n", buf) != 0：当前行不是空行（空行仅含\n，因为get_line会移除\r）*/
        while ((numchars > 0) && strcmp("\n", buf)){
            numchars = get_line(client, buf, sizeof(buf));
        }

        not_found(client);
    }
    else
    {   
        // 再次检查是否为目录，确保添加 index.html
        if ((st.st_mode & S_IFMT) == S_IFDIR){
            strcat(path, "/index.html");
        }
        // 检查文件是否有可执行权限（用户、组或其他），如果有则标记为 CGI
        if ((st.st_mode & S_IXUSR) ||
                (st.st_mode & S_IXGRP) ||
                (st.st_mode & S_IXOTH)    )
            cgi = 1;
        if (!cgi){  // 静态文件：调用 serve_file 读取文件内容并返回给客户端
            serve_file(client, path);
        }
        else{   // CGI 程序：调用 execute_cgi 创建子进程执行外部程序，并返回程序输出
            execute_cgi(client, path, method, query_string);
        }
    }

    close(client);
}
/*
CGI 判断逻辑：
POST 请求自动作为 CGI 处理
GET 请求带查询字符串作为 CGI 处理
有执行权限的文件作为 CGI 处理
*/


/**************************************************
* bad_request 函数用于向客户端返回 400 Bad Request 错误响应，
* 这是 HTTP 协议中表示请求格式错误的标准状态码。该函数通过构造完整的 HTTP 响应报文，
* 告知客户端请求存在语法或格式问题。
**************************************************/
void bad_request(int client){
    char buf[1024];

    // 发送HTTP状态行
    sprintf(buf,"HTTP/1.0 400 BAD REQUEST\r\n");
    send(client,buf,sizeof(buf),0);

    // 发送Content-Type头部
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, sizeof(buf), 0);

    // 发送空行（标志HTTP头部结束）
    sprintf(buf,"\r\n");
    send(client, buf, sizeof(buf), 0);

    // 发送HTML格式的错误信息
    sprintf(buf, "<P>Your browser sent a bad request, ");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "such as a POST without a Content-Length.\r\n");
    send(client, buf, sizeof(buf), 0);

}

/**************************************************
* cat 函数的作用是将指定文件的内容逐行读取并通过套接字发送给客户端，
* 类似于 UNIX 系统中的cat命令（用于显示文件内容）
* 它是服务器向客户端返回静态文件内容的核心函数，体现了 "读取文件→网络发送" 的基本流程
**************************************************/
void cat(int client,FILE* resource){
    char buf[1024];
    fgets(buf,sizeof(buf),resource);
    while (!feof(resource))
    {
        send(client, buf, strlen(buf), 0);
        fgets(buf, sizeof(buf), resource);
    }
}

/**********************************************************************
* cannot_execute 函数用于向客户端返回 500 Internal Server Error 错误响应，
* 指示服务器在执行 CGI 脚本时发生了内部错误
**********************************************************************/
void cannot_execute(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
    send(client, buf, strlen(buf), 0);
}


/**********************************************************************
* error_die 函数是一个错误处理辅助函数，用于在系统调用失败时打印错误信息并终止程序。
**********************************************************************/
void error_die(const char *sc)
{
    perror(sc);
    exit(1);
}


/**********************************************************************
* execute_cgi 函数是 Web 服务器执行 CGI（通用网关接口）脚本的核心组件。
* 它通过创建子进程运行外部程序，并通过管道实现服务器与 CGI 脚本的通信，
* 从而将客户端请求传递给脚本并返回处理结果。该函数体现了 CGI 协议的核心实现原理。
**********************************************************************/
void execute_cgi(int client, const char *path,
        const char *method, const char *query_string)
{
    // 变量声明与初始化
    char buf[1024];
    int cgi_output[2];  // 用于CGI输出的管道
    int cgi_input[2];   // 用于CGI输入的管道
    pid_t pid;          // 子进程ID
    int status;         // 子进程状态
    int i;
    char c;
    int numchars = 1;
    int content_length = -1;

    // 1. 读取并处理HTTP请求头部
    buf[0] = 'A'; buf[1] = '\0';
    if (strcasecmp(method, "GET") == 0)
        while ((numchars > 0) && strcmp("\n", buf))
            numchars = get_line(client, buf, sizeof(buf));
    else if (strcasecmp(method, "POST") == 0) {
        // 处理POST请求的Content-Length
        numchars = get_line(client, buf, sizeof(buf));
        while ((numchars > 0) && strcmp("\n", buf)) {
            buf[15] = '\0';
            if (strcasecmp(buf, "Content-Length:") == 0)
                content_length = atoi(&(buf[16]));
            numchars = get_line(client, buf, sizeof(buf));
        }
        if (content_length == -1) {
            bad_request(client);
            return;
        }
    }

    // 2. 创建双向通信管道
    if (pipe(cgi_output) < 0 || pipe(cgi_input) < 0) {
        cannot_execute(client);
        return;
    }

    // 3. 分叉子进程执行CGI脚本
    if ((pid = fork()) < 0) {
        cannot_execute(client);
        return;
    }
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);  // 先发送200响应

    if (pid == 0) {  // 子进程: 执行CGI脚本
        // 4. 重定向标准输入输出到管道
        dup2(cgi_output[1], STDOUT_FILENO);  // CGI输出到管道
        dup2(cgi_input[0], STDIN_FILENO);    // CGI从管道读取输入
        close(cgi_output[0]); close(cgi_input[1]);  // 关闭不需要的管道端

        // 5. 设置CGI环境变量
        char meth_env[255];
        char query_env[255];
        char length_env[255];
        sprintf(meth_env, "REQUEST_METHOD=%s", method);
        putenv(meth_env);
        if (strcasecmp(method, "GET") == 0) {
            sprintf(query_env, "QUERY_STRING=%s", query_string);
            putenv(query_env);
        } else {
            sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
            putenv(length_env);
        }

        // 6. 执行CGI脚本
        execl(path, NULL);
        exit(0);  // 仅当execl失败时执行
    } else {  // 父进程: 处理客户端与CGI的通信
        close(cgi_output[1]); close(cgi_input[0]);  // 关闭不需要的管道端

        // 7. 处理POST请求的请求体
        if (strcasecmp(method, "POST") == 0)
            for (i = 0; i < content_length; i++) {
                recv(client, &c, 1, 0);
                write(cgi_input[1], &c, 1);  // 发送到CGI输入管道
            }

        // 8. 读取CGI输出并转发给客户端
        while (read(cgi_output[0], &c, 1) > 0)
            send(client, &c, 1, 0);

        // 9. 清理资源
        close(cgi_output[0]);
        close(cgi_input[1]);
        waitpid(pid, &status, 0);  // 等待子进程结束
    }
}



/**********************************************************************
* get_line 函数是一个用于从套接字读取单行数据的辅助函数，
* 它能够处理不同类型的行终止符（\n、\r 或 \r\n），并将读取的内容存储为以 \0 结尾的 C 字符串。
* 这个函数在处理 HTTP 请求头部时特别有用，因为 HTTP 协议允许使用不同的行结束符。
**********************************************************************/

/*它能够处理不同操作系统的行结束符差异，包括：
*    \n (Unix/Linux)
*    \r (Mac OS Classic)
*    \r\n (Windows)
*/
int get_line(int sock,char* buf,int size){
    int i=0;
    char c ='\0';
    int n;

    while((i<size-1)&&(c!='\n')){
        n=recv(sock,&c,1,0);
        if(n>0){
            if(c=='\r'){
                n = recv(sock, &c, 1, MSG_PEEK);
                if ((n > 0) && (c == '\n')){
                    recv(sock, &c, 1, 0);
                }
                else{
                    c='\n';
                }
            }
            buf[i]=c;
            i++;
        }
        else{
            c='\n';
        }
    }
    buf[i]='\0';

    return i;

}


/**********************************************************************
* headers 函数用于向客户端发送 HTTP 响应头部信息，
* 这些信息包含了关于请求资源的元数据，如状态码、服务器类型、内容类型等。
**********************************************************************/
void headers(int client,const char* filename){
    char buf[1024];
    (void)filename;  /* could use filename to determine file type */

    // 发送状态行
    strcpy(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    // 发送服务器标识
    strcpy(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    // 发送内容类型（固定为text/html）
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    // 发送空行（标记头部结束）
    strcpy(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
}


/**********************************************************************
* not_found 函数用于向客户端返回 404 Not Found 错误响应，指示请求的资源在服务器上不存在。
**********************************************************************/
void not_found(int client)
{
    char buf[1024];

    // 发送HTTP状态行
    sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
    send(client, buf, strlen(buf), 0);
    
    // 发送服务器标识头部
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    
    // 发送内容类型头部
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    
    // 发送空行（分隔头部与响应体）
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    
    // 发送HTML格式的错误页面
    sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "your request because the resource specified\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "is unavailable or nonexistent.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}


/**********************************************************************
* serve_file 函数是 Web 服务器处理静态文件请求的核心组件，它负责将服务器文件系统中的文件读取并发送给客户端。
**********************************************************************/
void serve_file(int client, const char *filename)
{
    FILE *resource = NULL;  // 文件指针
    int numchars = 1;       // 读取的字符数
    char buf[1024];         // 缓冲区

    // 1. 读取并丢弃HTTP请求头部
    buf[0] = 'A'; buf[1] = '\0';
    while ((numchars > 0) && strcmp("\n", buf))
        numchars = get_line(client, buf, sizeof(buf));

    // 2. 打开请求的文件
    resource = fopen(filename, "r");
    if (resource == NULL)
        not_found(client);  // 文件不存在，返回404错误
    else
    {
        // 3. 发送HTTP响应头部
        headers(client, filename);
        // 4. 读取文件内容并发送给客户端
        cat(client, resource);
    }
    // 5. 关闭文件
    fclose(resource);
}


/**********************************************************************
* startup 函数是 Web 服务器的初始化核心，负责创建套接字、绑定端口并启动监听。
**********************************************************************/
int startup(u_short *port)
{
    int httpd = 0;          // 服务器套接字描述符
    int on = 1;             // 套接字选项值
    struct sockaddr_in name; // 服务器地址结构

    // 1. 创建TCP套接字
    httpd = socket(PF_INET, SOCK_STREAM, 0);
    if (httpd == -1)
        error_die("socket");

    // 2. 初始化地址结构
    memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;          // IPv4地址族
    name.sin_port = htons(*port);       // 端口号（网络字节序）
    name.sin_addr.s_addr = htonl(INADDR_ANY); // 绑定所有网络接口

    // 3. 设置套接字选项（SO_REUSEADDR）
    if ((setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) < 0)  
    {  
        error_die("setsockopt failed");
    }

    // 4. 绑定套接字到地址
    if (bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0)
        error_die("bind");

    // 5. 动态端口分配（端口为0时）
    if (*port == 0)  {
        socklen_t namelen = sizeof(name);
        if (getsockname(httpd, (struct sockaddr *)&name, &namelen) == -1)
            error_die("getsockname");
        *port = ntohs(name.sin_port); // 将分配的端口号返回给调用者
    }

    // 6. 启动监听
    if (listen(httpd, 5) < 0)
        error_die("listen");

    return(httpd); // 返回服务器套接字描述符
}


/**********************************************************************
* unimplemented 函数用于向客户端返回 501 Not Implemented 错误响应，
* 表明服务器不支持客户端请求的 HTTP 方法（如 PUT、DELETE 等）
**********************************************************************/
void unimplemented(int client)
{
    char buf[1024];

    // 1. 发送HTTP状态行
    sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    
    // 2. 发送服务器标识头部
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    
    // 3. 发送内容类型头部
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    
    // 4. 发送空行（分隔头部与响应体）
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    
    // 5. 发送HTML格式的错误页面
    sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</TITLE></HEAD>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/

int main(void)
{
    int server_sock = -1;
    u_short port = 4000;
    int client_sock = -1;
    struct sockaddr_in client_name;
    socklen_t  client_name_len = sizeof(client_name);
    pthread_t newthread;

    server_sock = startup(&port);
    printf("httpd running on port %d\n", port);

    while (1)
    {
        client_sock = accept(server_sock,
                (struct sockaddr *)&client_name,
                &client_name_len);
        if (client_sock == -1)
            error_die("accept");
        /* accept_request(&client_sock); */
        if (pthread_create(&newthread , NULL, (void *)accept_request, (void *)(intptr_t)client_sock) != 0)
            perror("pthread_create");
    }

    close(server_sock);

    return(0);
}
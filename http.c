#include "http.h"
#include "dns.h"

#define SSL_RSP "HTTP/1.1 200 Connection established\r\n\r\n"
#define HTTP_TYPE 0
#define OTHER_TYPE 1

conn_t cts[MAX_CONNECTION];
char *local_header, *proxy_header, *ssl_proxy;
int lisFd, proxy_header_len, local_header_len;
uint8_t strict_spilce;

/*
    有时候由于程序代码原因glibc的memcpy不能src + len > dst，调试起来比较麻烦
    这个函数正是为了解决这样的问题，但是效率会比memcpy低
*/
#ifdef XMEMCPY
typedef struct byte256 {char data[256];} byte256_t;
typedef struct byte64 {char data[64];} byte64_t;
typedef struct byte16 {char data[16];} byte16_t;
static void xmemcpy(char *src, const char *dst, size_t len)
{
    static byte256_t *to256, *from256;
    static byte64_t *to64, *from64;
    static byte16_t *to16, *from16;

    to256 = (byte256_t *)src;
    from256 = (byte256_t *)dst;
    while (len >= sizeof(byte256_t))
    {
        *to256++ = *from256++;
        len -= sizeof(byte256_t);
    }
    to64 = (byte64_t *)to256;
    from64 = (byte64_t *)from256;
    while (len >= sizeof(byte64_t))
    {
        *to64++ = *from64++;
        len -= sizeof(byte64_t);
    }
    to16 = (byte16_t *)to64;
    from16 = (byte16_t *)from64;
    while (len >= sizeof(byte16_t))
    {
        *to16++ = *from16++;
        len -= sizeof(byte16_t);
    }
    src = (char *)to16;
    dst = (char *)from16;
    while (len--)
        *src++ = *dst++;
}
#else
#define xmemcpy memcpy
#endif

int8_t connectionToServer(char *ip, conn_t *server)
{
    server->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->fd < 0)
        return 1;
    fcntl(server->fd, F_SETFL, fcntl(server->fd, F_GETFL)|O_NONBLOCK);
    addr.sin_addr.s_addr = inet_addr(ip);
    addr.sin_port = htons(server->destPort);
    connect(server->fd, (struct sockaddr *)&addr, sizeof(addr));
    ev.data.ptr = server;
    ev.events = EPOLLIN|EPOLLOUT|EPOLLET;
    epoll_ctl(efd, EPOLL_CTL_ADD, server->fd, &ev);

    return 0;
}

void close_connection(conn_t *conn)
{
    epoll_ctl(efd, EPOLL_CTL_DEL, conn->fd, NULL);
    close(conn->fd);
    if ((conn - cts) & 1)
    {
        static char *server_data;

        server_data = conn->ready_data;
        memset(conn, 0, sizeof(conn_t));
        conn->ready_data = server_data;
        conn-- ->fd = -1;
    }
    else
    {
        static struct dns *d;

        d =  dns_list + ((conn - cts) >> 1);
        d->request_len = d->sent_len = 0;
        free(conn->ready_data);
        free(conn->incomplete_data);
        memset(conn, 0, sizeof(conn_t));
        conn++ ->fd = -1;
    }
    if (conn->fd >= 0)
        close_connection(conn);
}


/* 判断请求类型 */
static int8_t request_type(char *data)
{
    if (strncmp(data, "GET", 3) == 0 ||
    strncmp(data, "POST", 4) == 0 ||
    strncmp(data, "CONNECT", 7) == 0 ||
    strncmp(data, "HEAD", 4) == 0 ||
    strncmp(data, "PUT", 3) == 0 ||
    strncmp(data, "OPTIONS", 7) == 0 ||
    strncmp(data, "MOVE", 4) == 0 ||
    strncmp(data, "COPY", 4) == 0 ||
    strncmp(data, "TRACE", 5) == 0 ||
    strncmp(data, "DELETE", 6) == 0 ||
    strncmp(data, "LINK", 4) == 0 ||
    strncmp(data, "UNLINK", 6) == 0 ||
    strncmp(data, "PATCH", 5) == 0 ||
    strncmp(data, "WRAPPED", 7) == 0)
        return HTTP_TYPE;
    return OTHER_TYPE;
}

static char *read_data(conn_t *in, char *data, int *data_len)
{
    static char *new_data;
    static int read_len;

    do {
        new_data = (char *)realloc(data, *data_len + BUFFER_SIZE + 1);
        if (new_data == NULL)
        {
            free(data);
            return NULL;
        }
        data = new_data;
        read_len = read(in->fd, data + *data_len, BUFFER_SIZE);
        /* 判断是否关闭连接 */
        if (read_len <= 0)
        {
            if (read_len == 0 || *data_len == 0 || errno != EAGAIN)
            {
                free(data);
                return NULL;
            }
            break;
        }
        *data_len += read_len;
    } while (read_len == BUFFER_SIZE);
    *(data + *data_len) = '\0';
   
   return data;
}

static char *get_host(char *data)
{
    static char *hostEnd, *host;

    host = strstr(data, local_header);
    if (host != NULL)
    {
        static char *local_host;
        
        host += local_header_len;
        while (*host == ' ')
            host++;
        for (hostEnd = host; *hostEnd < 58 && *hostEnd > 48; hostEnd++);
        //判断该头域是否正确使用
        if (hostEnd - host > 5 || *hostEnd != '\r')
            return NULL;
        local_host = (char *)malloc(16);
        if (local_host == NULL)
            return NULL;
        strcpy(local_host, "127.0.0.1:");
        strncpy(local_host + 10, host, hostEnd - host);
        local_host[10 + (hostEnd - host)] = '\0';
        puts(local_host);
        return local_host;
    }
    host= strstr(data, proxy_header);
    if (host == NULL)
        return NULL;
    host += proxy_header_len;
    while (*host == ' ')
        host++;
    hostEnd = strchr(host, '\r');
    if (hostEnd)
        return strndup(host, hostEnd - host);
    else
        return strdup(host);
}

/* 删除请求头中的头域 */
static void del_hdr(char *header, int *header_len)
{
    static char *key_end, *line_begin, *line_end;
    static int key_len;

    for (line_begin = strchr(header, '\n'); line_begin++ && *line_begin != '\r'; line_begin = line_end)
    {
        key_end = strchr(line_begin, ':');
        if (key_end == NULL)
            return;
        key_len = key_end - line_begin;
        line_end = strchr(key_end, '\n');
        if (strncasecmp(line_begin, "host", key_len) == 0 || strncmp(line_begin, local_header + 1, key_len) == 0 || strncmp(line_begin, proxy_header + 1, key_len) == 0)
        {
            if (line_end++)
            {
                xmemcpy(line_begin, line_end, *header_len - (line_end - header) + 1);
                (*header_len) -= line_end - line_begin;
                line_end = line_begin - 1;  //新行前一个字符
            }
            else
            {
                *line_begin = '\0';
                *header_len = line_begin - header;
                return;
            }
        }
    }
}

/* 构建新请求头 */
static char *build_request(char *client_data, int *data_len, char *host)
{
    static char *uri, *url, *p, *lf, *header, *new_data, *proxy_host;
    static int len;

    header = client_data;
    proxy_host = host;
    do {
        del_hdr(client_data, data_len);
        /* 将完整url转换为uri */
        url = strchr(header, ' ');
        lf = strchr(header, '\n');
        if (url == NULL || lf == NULL || lf - 10 <= header)
            return client_data;
        if (url < lf && *(++url) != '/')
        {
            uri = strchr(url + 7, '/');
            p = lf - 10;  //指向HTTP版本前面的空格
            if (uri != NULL && uri < p)
            {
                xmemcpy(url, uri, *data_len - (uri - client_data) + 1);
                *data_len -= uri - url;
                lf -= uri - url;
            }
            else
            {
                *url++ = '/';
                xmemcpy(url, p, *data_len - (p - client_data) + 1);
                *data_len -= p - url;
                lf -= p - url;
            }
        }

        *data_len += strlen(proxy_host) + 8;  //8为 "Host: " + "\r\n"的长度
        new_data = (char *)malloc(*data_len  + 1);
        if (new_data == NULL)
        {
            if (proxy_host != host)
                free(proxy_host);
            free(client_data);
            return NULL;
        }
        /* 请求行后面添加Host行 */
        len = lf + 1 - client_data;
        memcpy(new_data, client_data, len);  //复制请求行
        strcpy(new_data + len, "Host: ");
        strcpy(new_data + len + 6, proxy_host);
        len += strlen(proxy_host) + 6;
        new_data[len++] = '\r';
        new_data[len++] = '\n';
        //len += sprintf(new_data + len, "Host: %s\r\n", proxy_host);
        memcpy(new_data + len, lf + 1, *data_len - len + 1);
        free(client_data);
        if (proxy_host != host)
            free(proxy_host);
        if (strict_spilce == 0)
            return new_data;
        client_data = new_data;
        //如果请求头只有一个头域，则必须-1才能搜索到\n\r
        header = strstr(client_data + len - 1, "\n\r");
        //如果是连续的多个请求头，则全部修改
        if (header == NULL || request_type(header + 3) == OTHER_TYPE)
            return client_data;
        header += 3;
        proxy_host = get_host(header);
        if (proxy_host == NULL)
            proxy_host = host;
    } while (1);
}

/* 解析Host */
int8_t parse_host(conn_t *server, char *host)
{
    static char *port, *p;

    port = strchr(host, ':');
    if (port)
    {
        server->destPort = atoi(port+1);
        *port = '\0';
    }
    else
        server->destPort = 80;  //默认80端口
    for (p = host; (*p > 47 && *p < 58) || *p == '.'; p++);
    if (*p == '\0')
    {
        if (connectionToServer(host, server) != 0)
            return 1;
    }
    else if (build_dns_req(dns_list + ((server - cts) >> 1), host) == -1)
        return 1;
    if (port)
        *port = ':';
    
    return 0;
}

/* 读取到的数据全部就绪，将incomplete_data复制到ready_data */
static int8_t copy_data(conn_t *ct)
{
    if (ct->ready_data)
    {
        static char *new_data;
        
        new_data = (char *)realloc(ct->ready_data, ct->ready_data_len + ct->incomplete_data_len);
        if (new_data == NULL)
            return 1;
        ct->ready_data = new_data;
        memcpy(new_data + ct->ready_data_len, ct->incomplete_data, ct->incomplete_data_len);
        ct->ready_data_len += ct->incomplete_data_len;
        free(ct->incomplete_data);
    }
    else
    {
        ct->ready_data = ct->incomplete_data;
        ct->ready_data_len = ct->incomplete_data_len;
    }
    ct->incomplete_data = NULL;
    ct->incomplete_data_len = 0;

    return 0;
}

static void serverToClient(conn_t *server)
{
    static conn_t *client;
    static int write_len;

    client = server - 1;
    while ((server->ready_data_len = read(server->fd, server->ready_data, BUFFER_SIZE)) > 0)
    {
        write_len = write(client->fd, server->ready_data, server->ready_data_len);
        if (write_len == -1)
        {
            if (errno != EAGAIN)
                close_connection(server);
            else
                server->sent_len = 0;
            return;
        }
        else if (write_len < server->ready_data_len)
        {
            server->sent_len = write_len;
            ev.events = EPOLLIN|EPOLLOUT|EPOLLET;
            ev.data.ptr = client;
            epoll_ctl(efd, EPOLL_CTL_MOD, client->fd, &ev);
            return;
        }
    }
    //判断服务端是否关闭连接
    if (server->ready_data_len == 0 || errno != EAGAIN)
        close_connection(server);
    else
        server->ready_data_len = server->sent_len = 0;
}

void tcp_out(conn_t *to)
{
    static conn_t *from;
    static int write_len;

    if (to->fd == -1)
        return;
    else if ((to - cts) & 1)
        from = to - 1;
    else
        from = to + 1;
    write_len = write(to->fd, from->ready_data + from->sent_len, from->ready_data_len - from->sent_len);
    if (write_len == from->ready_data_len - from->sent_len)
    {
        //服务端的数据可能没全部写入到客户端
        if ((from - cts) & 1)
        {
            serverToClient(from);
            if (from->fd >= 0 && from->ready_data_len == 0)
            {
                ev.events = EPOLLIN|EPOLLET;
                ev.data.ptr = to;
                epoll_ctl(efd, EPOLL_CTL_MOD, to->fd, &ev);
            }
            return;
        }
        else
        {
            ev.events = EPOLLIN|EPOLLET;
            ev.data.ptr = to;
            epoll_ctl(efd, EPOLL_CTL_MOD, to->fd, &ev);
            free(from->ready_data);
            from->ready_data = NULL;
            from->ready_data_len = 0;
        }
    }
    else if (write_len > 0)
    {
        from->sent_len += write_len;
        ev.events = EPOLLIN|EPOLLOUT|EPOLLET;
        ev.data.ptr = to;
        epoll_ctl(efd, EPOLL_CTL_MOD, to->fd, &ev);
    }
    else if (errno != EAGAIN)
    {
        close_connection(to);
    }
}

void tcp_in(conn_t *in)
{
    static int write_len;
    static conn_t *server;
    static char *host, *headerEnd;
    
    if (in->fd < 0)
        return;

    //如果in - cts是奇数，那么是服务端触发事件
    if ((in - cts) & 1)
    {
        if (in->ready_data_len == 0)
            serverToClient(in);
        return;
    }

    in->incomplete_data = read_data(in, in->incomplete_data, &in->incomplete_data_len);
    if (in->incomplete_data == NULL)
    {
        close_connection(in);
        return;
    }
    server = in + 1;
    if (request_type(in->incomplete_data) == OTHER_TYPE)
    {
        //如果是第一次读取数据，并且不是HTTP请求的，关闭连接。复制数据失败的也关闭连接
        if (in->reread_data == 0 || copy_data(in) != 0)
        {
            close_connection(in);
            return;
        }
        goto handle_data_complete;
    }
    headerEnd = strstr(in->incomplete_data, "\n\r");
    //请求头不完整，等待下次读取
    if (headerEnd == NULL)
        return;
    host = get_host(in->incomplete_data);
    if (host == NULL)
    {
        close_connection(in);
        return;
    }
    /* 第一次读取数据 */
    if (in->reread_data == 0)
    {
        in->reread_data = 1;
        if (parse_host(server, host) != 0)
        {
            free(host);
            close_connection(in);
            return;
        }
        if (strstr(in->incomplete_data, ssl_proxy))
        {
            write_len = write(in->fd, SSL_RSP, 39);
            if (write_len == 39)
            {
                ;
            }
            else if (write_len > 0)
            {
                memcpy(server->ready_data, SSL_RSP + write_len, 39 - write_len);
            }
            else
            {
                free(host);
                close_connection(in);
                return;
            }
            headerEnd += 3;
            if (headerEnd - in->incomplete_data < in->incomplete_data_len)
            {
                in->incomplete_data_len -= headerEnd - in->incomplete_data;
                xmemcpy(in->incomplete_data, headerEnd, in->incomplete_data_len + 1);
                if (request_type(in->incomplete_data) == OTHER_TYPE)
                {
                    copy_data(in);
                    free(host);
                    return;
                }
            }
            else
            {
                free(in->incomplete_data);
                in->incomplete_data = NULL;
                in->incomplete_data_len = 0;
                free(host);
                return;
            }
        }
    }
    in->incomplete_data = build_request(in->incomplete_data, &in->incomplete_data_len, host);
    free(host);
    if (in->incomplete_data == NULL || copy_data(in) != 0)
    {
        close_connection(in);
        return;
    }
    //数据处理完毕，可以发送
    handle_data_complete:
    //多次读取客户端数据，但是和服务端建立连接
    if (server->fd >= 0)
        tcp_out(server);
}

void *accept_loop(void *ptr)
{
    conn_t *client;
    
    while (1)
    {
        /* 偶数为客户端，奇数为服务端 */
        for (client = cts; client - cts < MAX_CONNECTION; client += 2)
            if (client->fd < 0)
                break;
        if (client - cts >= MAX_CONNECTION)
        {
            sleep(3);
            continue;
        }
        client->fd = accept(lisFd, (struct sockaddr *)&addr, &addr_len);
        if (client->fd >= 0)
        {
            fcntl(client->fd, F_SETFL, fcntl(client->fd, F_GETFL)|O_NONBLOCK);
            ev.data.ptr = client;
            ev.events = EPOLLIN|EPOLLET;
            epoll_ctl(efd, EPOLL_CTL_ADD, client->fd, &ev);
        }
    }
    
    return NULL;
}

void create_listen(char *ip, int port)
{
    int optval = 1;

    if ((lisFd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("socket");
        exit(1);
    }
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);
    if (setsockopt(lisFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
    {
        perror("setsockopt");
        exit(1);
    }
    if (bind(lisFd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        perror("bind");
        exit(1);
    }
    if (listen(lisFd, 500) != 0)
    {
        perror("listen");
        exit(1);
    }
}





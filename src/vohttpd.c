﻿/* vohttpd: simple, light weight, embedded web server.
 *
 * author: Qin Wei(me@vonger.cn)
 * compile: cc -g vohttpd.c vohttpdext.c -o vohttpd
 *
 * TODO: add auth function.
 *   move to plugin loader compose.
 * TODO: add https function.
 *   maybe use polarSSL.
 */

#include <stdlib.h>
#include <memory.h>
#include <stdio.h>

#include "vohttpd.h"

#define safe_free(p)  if(p) { free(p); p = NULL; }

/* linear hash, every unit must start with its key. */
#define LINEAR_HASH_NULL         ((uint)(-1))
#define linear_hash_key(h, p)    (*(uint *)((h)->data + (p) * (h)->unit))
#define linear_hash_value(h, p)  ((h)->data + (p) * (h)->unit)
#define linear_hash_empty(h, p)  (linear_hash_key((h), (p)) == LINEAR_HASH_NULL)
#define linear_hash_clear(h, p)  {linear_hash_key((h), (p)) = LINEAR_HASH_NULL;}

linear_hash* linear_hash_alloc(uint unit, uint max)
{
    linear_hash *lh =
        (linear_hash *)malloc(max * unit + sizeof(linear_hash));
    if(lh == NULL)
        return NULL;

    lh->unit = unit;
    lh->max = max;

    while(max--)
        linear_hash_clear(lh, max);
    return lh;
}

uchar* linear_hash_get(linear_hash *lh, uint key)
{
    uint pos = key % lh->max, i;
    // match node in the first hit.
    if(linear_hash_key(lh, pos) == key)
        return linear_hash_value(lh, pos);

    // try to hit next node if we miss the first.
    for(i = pos + 1; ; i++) {
        if(i >= lh->max)
            i = 0;
        if(i == pos)
            break;

        if(linear_hash_key(lh, i) == key)
            return linear_hash_value(lh, i);
    }
    return NULL;
}

uchar* linear_hash_set(linear_hash *lh, uint key)
{
    uint pos = key % lh->max, i;
    // first hit, this hash node is empty.
    if(linear_hash_empty(lh, pos))
        return linear_hash_value(lh, pos);

    // try to find another empty node.
    for(i = pos + 1; ; i++) {
        if(i >= lh->max)
            i = 0;
        if(i == pos)
            break;

        if(linear_hash_empty(lh, i))
            return linear_hash_value(lh, i);
    }
    return NULL;
}

void linear_hash_remove(linear_hash *lh, uint key)
{
    uchar* d = linear_hash_get(lh, key);
    if(d == NULL)
        return;
    linear_hash_clear(lh, (d - lh->data) / lh->unit);
}


/* string hash, get data by string, make it faster. */
#define string_hash_key(h, p)    ((char *)(((h)->data + (p)) + sizeof(uchar *)))
#define string_hash_val(h, p)    (*((uchar **)((h)->data + (p))))
#define string_hash_empty(h, p)  (*string_hash_key((h), (p)) == '\0')
#define string_hash_clear(h, p)  {*string_hash_key((h), (p)) = '\0';}

string_hash* string_hash_alloc(uint unit, uint max)
{
    string_hash *sh = (string_hash *)
        malloc(max * (unit + sizeof(uchar *)) + sizeof(string_hash));
    if(sh == NULL)
        return NULL;
    memset(sh, 0, max * (unit + sizeof(uchar *)) + sizeof(string_hash));

    sh->unit = unit + sizeof(uchar *);
    sh->max = max;
    return sh;
}

uint string_hash_from(const char *str)
{
    uint hash = *str;
    while(*str++)
        hash = hash * 31 + *str;
    return hash;
}

uchar* string_hash_get(string_hash *sh, const char *key)
{
    uint pos = (string_hash_from(key) % sh->max) * sh->unit, i;
    // match node in the first hit.
    if(strcmp(string_hash_key(sh, pos), key) == 0)
        return string_hash_val(sh, pos);

    // try to hit next node if we miss the first.
    for(i = pos + sh->unit;; i += sh->unit) {
        if(i >= sh->unit * sh->max)
            i = 0;
        if(i == pos)
            break;
        if(strcmp(string_hash_key(sh, i), key) == 0)
            return string_hash_val(sh, i);
    }
    return NULL;
}

uchar* string_hash_set(string_hash *sh, const char *key, uchar *value)
{
    uint pos = (string_hash_from(key) % sh->max) * sh->unit, i;
    // first hit, this hash node is empty.
    if(string_hash_empty(sh, pos)) {
        strcpy(string_hash_key(sh, pos), key);
        memcpy(&string_hash_val(sh, pos), &value, sizeof(uchar *));
        return string_hash_val(sh, pos);
    }

    // try to find another empty node.
    for(i = pos + sh->unit;; i += sh->unit) {
        if(i >= sh->unit * sh->max)
            i = 0;
        if(i == pos)
            break;

        if(string_hash_empty(sh, i)) {
            strcpy(string_hash_key(sh, i), key);
            memcpy(&string_hash_val(sh, i), &value, sizeof(uchar *));
            return string_hash_val(sh, i);
        }
    }
    return NULL;
}

void string_hash_remove(string_hash *sh, const char *key)
{
    uint pos = (string_hash_from(key) % sh->max) * sh->unit, i;
    // match node in the first hit.
    if(strcmp(string_hash_key(sh, pos), key) == 0) {
        string_hash_clear(sh, pos);
        return;
    }

    // try to hit next node if we miss the first.
    for(i = pos + sh->unit;; i += sh->unit) {
        if(i >= sh->unit * sh->max)
            i = 0;
        if(i == pos)
            break;
        if(strcmp(string_hash_key(sh, i), key) == 0) {
            string_hash_clear(sh, i);
            return;
        }
    }
}

int get_name_from_path(const char *path, char *name, size_t size)
{
    char *p1, *p2, *p;

    p1 = strrchr(path, '/');
    p2 = strrchr(path, '\\');
    if(p1 == NULL && p2 == NULL) {
        strncpy(name, path, size);
        return strlen(path);
    }

    p = max(p1, p2) + 1;
    strncpy(name, p, size);
    return strlen(p);
}

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/signal.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <dlfcn.h>

#define HTTP_HEADER_END     "\r\n\r\n"
#define HTTP_GET            "GET"
#define HTTP_POST           "POST"
#define HTTP_FONT           "Helvetica,Arial,sans-serif"

#define TIMEOUT             3000

static vohttpd g_set;


socket_data* socketdata_new(linear_hash *socks, int sock)
{
    socket_data *d;

    d = (socket_data *)linear_hash_set(socks, (uint)sock);
    if(d == NULL)
        return d;      // buffer has full...show error page?

    memset(d, 0, sizeof(socket_data));
    d->sock = sock;
    d->set = &g_set;
    return d;
}

void socketdata_delete(linear_hash *socks, int sock)
{
    socket_data *d = (socket_data *)linear_hash_get(socks, sock);
    close(sock);
    // check if the body point to head, if so we do not release it.
    if(d->body && (d->body < d->head || d->body >= d->head + RECVBUF_SIZE))
        safe_free(d->body);

    linear_hash_remove(socks, sock);
}

uint vohttpd_decode_content_size(socket_data *d)
{
    char *p;

    p = strstr(d->head, HTTP_CONTENT_LENGTH);
    if(p == NULL)
        return 0;
    p += sizeof(HTTP_CONTENT_LENGTH);

    while((*p < '0' || *p > '9') && *p != '\r' && *p != '\n')
        p++;

    return (uint)atoi(p);
}

uint vohttpd_file_size(const char *path)
{
    FILE *fp;
    uint size;

    fp = fopen(path, "rb");
    if(fp == NULL)
        return (uint)(-1);
    fseek(fp, 0, SEEK_END);
    size = (uint)ftell(fp);

    fclose(fp);
    return size;
}

const char *vohttpd_file_extend(const char *path)
{
    const char *p = strrchr(path, '.');
    return p == NULL ? p : (p + 1);
}

int vohttpd_http_file(socket_data *d, const char *path)
{
    char buf[SENDBUF_SIZE];
    const char* ext;
    int size, total = 0;
    FILE *fp;

    total = vohttpd_file_size(path);
    if(total == -1)
        return d->set->error_page(d, 404, NULL);

    ext = vohttpd_file_extend(path);
    size = vohttpd_reply_head(buf, 200);
    size += sprintf(buf + size, "%s: %d\r\n", HTTP_CONTENT_LENGTH, total);
    size += sprintf(buf + size, "%s: %s\r\n", HTTP_DATE_TIME, vohttpd_gmtime());
    size += sprintf(buf + size, "%s: %s\r\n", HTTP_CONTENT_TYPE, vohttpd_mime_map(ext));
    size += sprintf(buf + size, "%s: %s\r\n", HTTP_CONNECTION, "close");
    strcat(buf + size, "\r\n"); size += 2;

    size = send(d->sock, buf, size, 0);
    if(size <= 0)
        return -1;

    fp = fopen(path, "rb");
    if(fp == NULL)
        return d->set->error_page(d, 405, NULL);

    while(!feof(fp)) {
        size = fread(buf, 1, SENDBUF_SIZE, fp);
        if(size <= 0)
            break;
        size = send(d->sock, buf, size, 0);
        if(size <= 0)
            break;
        total += size;
    }
    fclose(fp);
    return total;
}

int vohttpd_default(socket_data *d, string_reference *fn)
{
    char head[MESSAGE_SIZE];
    char path[MESSAGE_SIZE];

    if(fn->size >= MESSAGE_SIZE)
        return d->set->error_page(d, 413, NULL);
    string_reference_dup(fn, head);
    if(strstr(head, ".."))
        return d->set->error_page(d, 403, NULL);

    if(head[fn->size - 1] == '/') {
        sprintf(path, "%s/%sindex.html", g_set.base, head);
        if(vohttpd_file_size(path) != (uint)(-1)) {
            // there is index file in folder.
            return vohttpd_http_file(d, path);
        } else {
            sprintf(path, "%s%s", g_set.base, head);
            // no such index file, we try to load default, always show folder.
            return d->set->error_page(d, 501, NULL);
        }
    }

    // the address might be a file.
    sprintf(path, "%s%s", g_set.base, head);
    return vohttpd_http_file(d, path);
}

int vohttpd_function(socket_data *d, string_reference *fn, string_reference *pa)
{
    char name[FUNCTION_SIZE];
    _plugin_func func;

    if(fn->size >= FUNCTION_SIZE)
        return d->set->error_page(d, 413, NULL);
    string_reference_dup(fn, name);
    if(strchr(name, '.') != NULL)   // this is library handle.
        return d->set->error_page(d, 403, NULL);
    func = (_plugin_func)string_hash_get(d->set->funcs, name);
    if(func == NULL)
        return d->set->error_page(d, 404, NULL);
    return func(d, pa);
}

// return:
//  0: get header do not have special request.
//  1: get header contains function.
//  2: get header contains function and parameters.
//  < 0: not a valid header.
int vohttpd_decode_get(socket_data *d, string_reference *fn, string_reference *pa)
{
    char *p, *e, *f1, *f2;
    int ret;

    p = d->head + sizeof(HTTP_GET);
    e = strstr(p, "\r\n");
    if(e == NULL)
        return -1;

    while(*p == ' ' && e != p)
        p++;
    if(e == p)
        return -1;
    while(*e != ' ' && e != p)
        e--;
    if(e == p)
        return -1;

    *e = '\0';

    f1 = strstr(p, HTTP_CGI_BIN);
    if(f1 == NULL) {
        ret = 0;
        fn->ref = p;
        fn->size = e - p;
    } else {
        f1 += sizeof(HTTP_CGI_BIN) - 1;
        f2 = strchr(f1, '?');
        if(f2 == NULL) {
            fn->ref = f1;
            fn->size = e - f1;
            ret = 1;
        } else {
            fn->ref = f1;
            fn->size = f2 - f1;
            ret = 2;
            pa->ref = ++f2;
            pa->size = e - f2;
        }
    }

    *e = ' ';
    return ret;
}

int vohttpd_decode_post(socket_data *d, string_reference *fn, string_reference *pa)
{
    char *p, *e, *f1;
    int ret;

    p = d->head + sizeof(HTTP_POST);
    e = strstr(p, "\r\n");
    if(e == NULL)
        return -1;

    while(*p == ' ' && e != p)
        p++;
    if(e == p)
        return -1;
    while(*e != ' ' && e != p)
        e--;
    if(e == p)
        return -1;

    *e = '\0';
    f1 = strstr(p, HTTP_CGI_BIN);
    *e = ' ';

    ret = 0;
    if(f1 != NULL) {
        f1 += sizeof(HTTP_CGI_BIN) - 1;

        fn->ref = f1;
        fn->size = e - f1;

        pa->ref = d->body;
        pa->size = d->recv;

        ret = 2;
    }

    return ret;
}

// return:
//  0: "Connection: close", close and remove socket.
//  1: "Connection: keep-alive", wait for next request.
int vohttpd_data_filter(socket_data *d)
{
    string_reference fn, pa;

    if(memcmp(d->head, "GET", 3) == 0) {
        switch(vohttpd_decode_get(d, &fn, &pa)) {
        case 0: {
            vohttpd_default(d, &fn);
            break; }
        case 1: {
            pa.ref = ""; pa.size = 0;
            vohttpd_function(d, &fn, &pa);
            break; }
        case 2: {
            vohttpd_function(d, &fn, &pa);
            break; }
        default:
            d->set->error_page(d, 404, NULL);
            break;
        }
    } else if(memcmp(d->head, "POST", 4) == 0) {
        switch(vohttpd_decode_post(d, &fn, &pa)) {
        case 2:
            vohttpd_function(d, &fn, &pa);
            break;
        default:
            d->set->error_page(d, 404, NULL);
            break;
        }
    } else {
        d->set->error_page(d, 501, NULL);
    }

    return 0;
}

int vohttpd_error_page(socket_data *d, int code, const char *err)
{
    char head[MESSAGE_SIZE], body[MESSAGE_SIZE];
    const char *msg;
    int  size, total = 0;

    if(err == NULL)
        err = "Sorry, I have tried my best... :'(";

    msg = vohttpd_code_message(code);
    total = sprintf(body,
        "<html><head><title>%s</title></head><body style=\"font-family:%s;\">"
        "<h1 style=\"color:#0040F0\">%d %s</h1><p style=\"font-size:14px;\">%s"
        "</p></body></html>", msg, HTTP_FONT, code, msg, err);

    size = vohttpd_reply_head(head, code);
    size += sprintf(head + size, "%s: %s\r\n", HTTP_CONTENT_TYPE, vohttpd_mime_map("html"));
    size += sprintf(head + size, "%s: %s\r\n", HTTP_DATE_TIME, vohttpd_gmtime());
    size += sprintf(head + size, "%s: %d\r\n", HTTP_CONTENT_LENGTH, total);
    strcat(head, "\r\n"); size += 2;

    size = send(d->sock, head, size, 0);
    if(size <= 0)
        return -1;
    total = send(d->sock, body, total, 0);
    if(total <= 0)
        return -1;
    return total + size;
}

const char* vohttpd_unload_plugin(const char *path)
{
    char name[FUNCTION_SIZE];
    void *h = NULL;
    int  id = 0;

    _plugin_query query;
    _plugin_cleanup clean;
    plugin_info info;

    if(get_name_from_path(path, name, FUNCTION_SIZE) >= FUNCTION_SIZE)
        return "library name is too long.";
    h = (void *)string_hash_get(g_set.funcs, name);
    if(h == NULL)
        return "library has not been loaded.";

    query = dlsym(h, LIBRARY_QUERY);
    if(query == NULL)
        return "can not find query interface.";
    string_hash_remove(g_set.funcs, name);

    while(query(id++, &info) >= 0) {
        _plugin_func func = dlsym(h, info.name);
        if(func == NULL)
            continue;       // no such interface.
        if(func != (_plugin_func)string_hash_get(g_set.funcs, info.name))
            continue;       // not current interface.
        string_hash_remove(g_set.funcs, info.name);
    }

    clean = dlsym(h, LIBRARY_CLEANUP);
    if(clean)
        clean();

    dlclose(h);
    return NULL;
}

const char* vohttpd_load_plugin(const char *path)
{
    char name[FUNCTION_SIZE];
    void *h = NULL;
    int id = 0;

    _plugin_query query;
    plugin_info info;

    if(get_name_from_path(path, name, FUNCTION_SIZE) >= FUNCTION_SIZE)
        return "library name is too long.";
    if(strchr(name, '.') == NULL)
        return "library name is not correct.";
    if(string_hash_get(g_set.funcs, name))
        return "library has already loaded.";

    h = dlopen(path, RTLD_NOW);
    if(h == NULL)
        return "can not find file.";
    query = dlsym(h, LIBRARY_QUERY);
    if(query == NULL) {
        dlclose(h);
        return "can not find the library.";
    }
    if(string_hash_set(g_set.funcs, name, (uchar *)h) == NULL) {
        dlclose(h);
        return "hash is full.";
    }

    while(query(id++, &info) >= 0) {
        _plugin_func func = dlsym(h, info.name);
        if(func == NULL)
            continue;       // no such interface.
        if(string_hash_get(g_set.funcs, info.name))
            continue;       // already exists same name interface.
        if(!string_hash_set(g_set.funcs, info.name, (uchar *)func))
            continue;       // hash full?
        printf("func: %s %016lX\n", info.name, (unsigned long)func);
    }
    return NULL;
}

void vohttpd_init()
{
    // default parameters.
    if(!g_set.port)
        g_set.port = 8080;
    if(!g_set.base)
        g_set.base = "/var/www/html";

    // alloc buffer for globle pointer(maybe make them to static is better?)
    g_set.funcs = string_hash_alloc(FUNCTION_SIZE, FUNCTION_COUNT);
    g_set.socks = linear_hash_alloc(sizeof(socket_data), BUFFER_COUNT);

    // set default callback.
    g_set.http_filter = vohttpd_data_filter;
    g_set.error_page = vohttpd_error_page;
    g_set.load_plugin = vohttpd_load_plugin;
    g_set.unload_plugin = vohttpd_unload_plugin;

    // ignore the signal, or it will stop our server once client disconnected.
    signal(SIGPIPE, SIG_IGN);
}

void vohttpd_uninit()
{
    safe_free(g_set.funcs);
    safe_free(g_set.socks);
}

void vohttpd_loop()
{
    int socksrv, sockmax, sock, b = 1;
    uint i, count, size;
    char *p;

    struct sockaddr_in addr;
    socklen_t len;
    struct timeval tmv;
    fd_set fdr;
    socket_data *d;

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_set.port);
    len = sizeof(struct sockaddr_in);

    socksrv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    setsockopt(socksrv, SOL_SOCKET, SO_REUSEADDR, (const char *)&b, sizeof(int));
    if(bind(socksrv, (struct sockaddr*)&addr, sizeof(struct sockaddr)) < 0) {
        printf("can not bind to address, %d:%s.\n", errno, strerror(errno));
        return;
    }

    // we can not handle much request at same time, so limit listen backlog.
    if(listen(socksrv, min(SOMAXCONN, BUFFER_COUNT / 2)) < 0) {
        printf("can not listen to port, %d:%s.\n", errno, strerror(errno));
        return;
    }

    // for simple embed server, my choice is select for better compatible.
    // but for heavy load situation in linux, better to change this to epoll.
    while(1) {
        FD_ZERO(&fdr);
        FD_SET(socksrv, &fdr);

        // queue the hash and pickout the max socket.
        sockmax = socksrv;
        for(i = 0; i < g_set.socks->max; i++) {
            d = (socket_data *)linear_hash_value(g_set.socks, i);
            sockmax = max(d->sock, sockmax);
            if(d->sock != (int)LINEAR_HASH_NULL)
                FD_SET(d->sock, &fdr);
        }

        tmv.tv_sec = TIMEOUT / 1000;
        tmv.tv_usec = TIMEOUT % 1000 * 1000;

        count = select(sockmax + 1, &fdr, NULL, NULL, &tmv);
        if(count <= 0) {
            // clean up all sockets, they are time out.
            for(i = 0; i < g_set.socks->max; i++) {
                d = (socket_data *)linear_hash_value(g_set.socks, i);
                if(d->sock == (int)LINEAR_HASH_NULL)
                    continue;
                socketdata_delete(g_set.socks, d->sock);
            }
            continue;
        }

        if(FD_ISSET(socksrv, &fdr)) {
            count--;
            memset(&addr, 0, sizeof(struct sockaddr_in));

            sock = accept(socksrv, (struct sockaddr*)&addr, &len);
            if(socketdata_new(g_set.socks, sock) == NULL)
                close(sock);
        }

        for(i = 0; i < g_set.socks->max; i++) {
            if(count <= 0)
                break;

            d = (socket_data *)linear_hash_value(g_set.socks, i);
            if((uint)d->sock == LINEAR_HASH_NULL)
                continue;

            if(FD_ISSET(d->sock, &fdr)) {
                count--;

                if(d->size == 0) {

                    // receive http head data.
                    size = recv(d->sock, d->head + d->used, RECVBUF_SIZE - d->used, 0);
                    if(size <= 0) {
                        socketdata_delete(g_set.socks, d->sock);
                        continue;
                    }
                    d->used += size;

                    // OPTIMIZE: we do not have to check from beginning every time.
                    // if new recv size > 4, we can check new recv.
                    p = strstr(d->head, HTTP_HEADER_END);
                    if(p == NULL) {
                        if(d->used >= RECVBUF_SIZE) {
                            g_set.error_page(d, 413, NULL);
                            socketdata_delete(g_set.socks, d->sock);
                        }
                        // not get the header end, so we wait next recv.
                        continue;
                    }

                    p += sizeof(HTTP_HEADER_END) - 1;

                    // now check the content size.
                    d->recv = p - d->head;
                    d->size = vohttpd_decode_content_size(d);
                    if(d->size == 0) {// no content.
                        if(!g_set.http_filter(d))
                            socketdata_delete(g_set.socks, d->sock);
                        // no body in this http request, it should be GET.
                        continue;
                    }

                    // the head buffer can not contain the body data(too big)
                    // we have to alloc memory for it.
                    //
                    // TODO: change this part to file mapping might be better.
                    // in most situation, embed device do not have to transfer
                    // much data unless receiving a file, so file mapping will
                    // save a lot of memory and save cost on store file.
                    // one thread process make this simple, map file name can be
                    // temp.[sock], once the socket is closed, delete that temp
                    // file. In plugin, we can move that file to another folder
                    // when we get full of it to save upload file.
                    // add a function: vohttpd_temp_filename(socket_data)
                    if(d->size > RECVBUF_SIZE - d->used + d->recv) {
                        d->body = malloc(d->size);
                        memset(d->body, 0, d->size);
                        memcpy(d->body, d->head, d->recv);

                        // now we should goto body data receive process.
                    }

                } else {

                    // receive http body data.
                    size = recv(d->sock, d->body + d->recv, d->size - d->recv, 0);
                    if(size <= 0) {
                        socketdata_delete(g_set.socks, d->sock);
                        continue;
                    }
                    d->recv += size;
                    if(d->recv >= d->size) {
                        if(!g_set.http_filter(d))
                            socketdata_delete(g_set.socks, d->sock);
                        continue;
                    }
                }
            }
        }

        // do some clean up for next loop.
    }

    close(socksrv);
}

void vohttpd_show_status()
{
    uint i, pos, count = 0;

    printf("PORT:\t%d\n", g_set.port);
    printf("PATH:\t%s\n", g_set.base);

    printf("PLUGINS:\n");
    for(i = 0; i < g_set.funcs->max; i++) {
        pos = i * g_set.funcs->unit;
        if(string_hash_empty(g_set.funcs, pos))
            continue;
        if(!strchr(string_hash_key(g_set.funcs, pos), '.'))
            continue;
        printf("\t%s\n", string_hash_key(g_set.funcs, pos));
        count++;
    }
    if(count == 0)
        printf("\t(empty)\n");
}

void vohttpd_show_usage()
{
    printf("usage: vohttpd [-bdhp?]\n\n");
    printf("\t-b[path]  set www home/base folder, default /var/www/html.\n"
           "\t-d[path]  preload plugin.\n"
           "\t-h,-?     show this usage.\n"
           "\t-p[port]  set server listen port, default 8080.\n"
           "\n");
}

int main(int argc, char *argv[])
{
    vohttpd_init();

    while(argc--) {
        const char *errstr;
        if(argv[argc][0] != '-' && argv[argc][0] != '/')
            continue;
        switch(argv[argc][1]) {

        case 'd':   // preload plugins.
            errstr = vohttpd_load_plugin(argv[argc] + 2);
            if(errstr != NULL)
                printf("load_plugin(%s) error: %s\n", argv[argc] + 2, errstr);
            break;

        case 'p':   // default port.
            g_set.port = atoi(argv[argc] + 2);
            break;

        case 'b':   // default home/base path.
            g_set.base = argv[argc] + 2;
            break;

        case 'h':
        case '?':
            vohttpd_show_usage();
            break;
        }
    }

    vohttpd_show_status();

    vohttpd_loop();
    vohttpd_uninit();
    return 0;
}


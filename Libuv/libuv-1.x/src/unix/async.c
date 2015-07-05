/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* This file contains both the uv__async internal infrastructure and the
 * user-facing uv_async_t functions.
 */

#include "uv.h"
#include "internal.h"
#include "atomic-ops.h"

#include <errno.h>
#include <stdio.h>  /* snprintf() */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
*
* 本地可见
*
*/
static void uv__async_event(uv_loop_t* loop,
                            struct uv__async* w,
                            unsigned int nevents);
static int uv__async_eventfd(void);

/*
*
* 初始化 uv_async_t handle ( 异步服务 ) (对外部提供的函数接口)
*
* uv_async_cb cb : 异步回掉函数
*
*  该函数在 uv_loop_init(...) 中被调用,异步回掉函数被设置成
* 线程池中的 uv_work_done(...)
*
*/

int uv_async_init(uv_loop_t* loop, uv_async_t* handle, uv_async_cb async_cb) {
  int err;
                  // uv_loop_t    uv__async         uv__async_cb
  err = uv__async_start(loop, &loop->async_watcher, uv__async_event);  //uv__async_event : 整个异步回掉的核心函数 
  if (err)
    return err;

  uv__handle_init(loop, (uv_handle_t*)handle, UV_ASYNC);
  handle->async_cb = async_cb;                      // 被设置成为线程池中的 uv_work_done(...) 
  handle->pending = 0;

  QUEUE_INSERT_TAIL(&loop->async_handles, &handle->queue); //将 this un_async_t 插入到 loop->async_handles 末尾
  uv__handle_start(handle);

  return 0;
}

/*
*
* 往一个异步服务(handle)中发生信号
* 
* 一般一个异步服务( uv_async_t ) 是注册到一个 EventLoop( uv_loop_t )中
*
*/
int uv_async_send(uv_async_t* handle) {
  /* Do a cheap read first. */
  if (ACCESS_ONCE(int, handle->pending) != 0)
    return 0;

  if (cmpxchgi(&handle->pending, 0, 1) == 0)
    uv__async_send(&handle->loop->async_watcher);

  return 0;
}

/*
*
* 关闭整个异步服务(handle)
*
*/
void uv__async_close(uv_async_t* handle) {
  QUEUE_REMOVE(&handle->queue); // 从队列中移除 this 节点
  uv__handle_stop(handle);
}

/*
*
* 异步事件的依次运行
*
* 遍历 loop 中的 async_handles ,依次调用其回掉函数
*
* ( uv__async_start(...) 中的回掉函数,被设置成为 uv__async 中的回掉函数 )
*
* 从 uv__async_io 中回掉的 
*/
static void uv__async_event(uv_loop_t* loop,
                            struct uv__async* w,
                            unsigned int nevents) {
  QUEUE* q;
  uv_async_t* h;

  QUEUE_FOREACH(q, &loop->async_handles) {     //相当于 for 循环
    h = QUEUE_DATA(q, uv_async_t, queue);      //取出队列头部的元素   

    if (cmpxchgi(&h->pending, 1, 0) == 0)
      continue;

    if (h->async_cb == NULL)
      continue;
    h->async_cb(h);                            //运行 uv_async 中的回掉函数
  }
}

/*
*
* 异步的 IO 事件，由 epoll 来通知 
*
* 相当于一个钩子函数 ( hook ), 实际上调用的是 uv__async 中的回掉函数
*
* 在 uv__io_init 被设置成为 async 中的 io 的回掉函数,这样 epoll
*通知时,会调用这个函数
* 
*/
static void uv__async_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  struct uv__async* wa;
  char buf[1024];
  unsigned n;
  ssize_t r;

  n = 0;
  for (;;) {                             // 为什么要无限循环呢 ? IO缓冲区的数据是累积的，需要一次性将数据读出来，以免影响下一次.
    r = read(w->fd, buf, sizeof(buf));   // 从 uv__io_t 中的文件描述符中读取数据 

    if (r > 0)                           // r 读出的数据的位数 ( byte )
      n += r;                            // r 的作用域仅限于 for 循环

    if (r == sizeof(buf))                //
      continue;

    if (r != -1)                         // 有数据过来
      break;

    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;

    if (errno == EINTR)                  // 中断而产生的错误  
      continue;

    abort();
  }

  wa = container_of(w, struct uv__async, io_watcher); // 从 uv__io_t 中 反推出 uv__async

#if defined(__linux__)       // linux 平台下
  if (wa->wfd == -1) {       // wa->wfd == -1
    uint64_t val;
    assert(n == sizeof(val)); // n == 8 ( byte ) 如果不是 8 byte 则说明数据没有读完整
    memcpy(&val, buf, sizeof(val));  /* Avoid alignment issues. */
    wa->cb(loop, wa, val);           // 调用的是 uv__async 中的回掉函数  
    return;
  }
#endif

  wa->cb(loop, wa, n);
}

/*
*
*  异步发送
*
*/
void uv__async_send(struct uv__async* wa) {
  const void* buf;
  ssize_t len;
  int fd;
  int r;

  buf = "";
  len = 1;
  fd = wa->wfd;  //文件描述符 用于唤醒某个线程 或者说是唤醒 epoll(...)

#if defined(__linux__)                   // linux 平台下
  if (fd == -1) {
    static const uint64_t val = 1;
    buf = &val;
    len = sizeof(val);
    fd = wa->io_watcher.fd;  /* eventfd */ // 往 uv__io_t 中的 fd 发送字节
  }
#endif

  do
    r = write(fd, buf, len);            // 写字节 ( 8 byte )  
  while (r == -1 && errno == EINTR);    // 往 wakeUpFd 中写入 1 byte字节 用于唤醒
                                        // 其对于loop 中的 epoll(...) 

  if (r == len)
    return;

  if (r == -1)
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;

  abort();
}


void uv__async_init(struct uv__async* wa) {
  wa->io_watcher.fd = -1;
  wa->wfd = -1;
}

/*
*
* 异步服务开始运行
* 
* uv_loop_t loop : 异步服务工作的 EventLoop
*
* uv__async wa : 在 EventLoop 中监控的异步服务( 监控异步信号 )
*
* uv__async_cb cb : 异步服务的回掉函数(被设置成本地 uv__async_event )
*
*/
int uv__async_start(uv_loop_t* loop, struct uv__async* wa, uv__async_cb cb) {
  int pipefd[2];
  int err;

  if (wa->io_watcher.fd != -1)  // io_watcher.fd 初始化为-1
    return 0;                   // io_watcher.fd!=-1 说明 uv__async_start(...) 已经被调用过了

  err = uv__async_eventfd();     
  if (err >= 0) {
    pipefd[0] = err;            // 设置 pipefd[0] 为 uv__io_t 中的 fd
    pipefd[1] = -1;             // 设置 pipefd[1] 为 uv__async 中的 wfd
  }
  else if (err == -ENOSYS) {   //系统不支持 eventfd ,则创建管道
    err = uv__make_pipe(pipefd, UV__F_NONBLOCK);
#if defined(__linux__)         // linux 平台 
    /* Save a file descriptor by opening one of the pipe descriptors as
     * read/write through the procfs.( 进程文件系统 )  That file descriptor can then
     * function as both ends of the pipe.
     */
    if (err == 0) {            //管道创建成功  
      char buf[32];
      int fd;

      snprintf(buf, sizeof(buf), "/proc/self/fd/%d", pipefd[0]); // "/proc/self/fd/%d" 通过文件描述符获取文件的绝对路径
      fd = uv__open_cloexec(buf, O_RDWR);  // 读写模式打开文件描述符                   
      if (fd >= 0) {
        uv__close(pipefd[0]);              // 先关闭 再设置
        uv__close(pipefd[1]);
        pipefd[0] = fd;                    // 文件描述符可用
        pipefd[1] = fd;
      }
    }
#endif
  }

  if (err < 0)                             // 管道创建不成功
    return err;
  
  // 初始化 uv__io_t ( io_watcher ) 
  // uv__async_io 是 io_watcher 中的回掉函数 设置 io 的回掉函数为上层 async 的回掉函数
  //当 epoll 有通知过来时,执行的就是 async 中的函数了 
  // pipefd[0] 是 io_watcher 中的 fd
  uv__io_init(&wa->io_watcher, uv__async_io, pipefd[0]);  // uv__io_init 中的回掉函数是
                                                          //static uv__async_io(...) 
  //设置关注的事件 UV_POLLIN
  uv__io_start(loop, &wa->io_watcher, UV__POLLIN);
  
  wa->wfd = pipefd[1];                                    // pipefd[1]
  wa->cb = cb;                                            // 真正的 async 中的回掉函数
                                                          //在 uv__async_io 中被回掉 
  return 0;
}

/*
*
* 停止异步服务( handle )
*
* io_watcher 是一个 uv__io_t 的结构
*
*/
void uv__async_stop(uv_loop_t* loop, struct uv__async* wa) {
  if (wa->io_watcher.fd == -1)
    return;

  if (wa->wfd != -1) {
    if (wa->wfd != wa->io_watcher.fd)
      uv__close(wa->wfd);
    wa->wfd = -1;
  }

  uv__io_stop(loop, &wa->io_watcher, UV__POLLIN);
  uv__close(wa->io_watcher.fd);
  wa->io_watcher.fd = -1;
}

/*

创建异步的 eventfd

*/
static int uv__async_eventfd() {
#if defined(__linux__)
  static int no_eventfd2;
  static int no_eventfd;
  int fd;

  if (no_eventfd2)           // no_eventfd2!=0 时 直接跳转
    goto skip_eventfd2;     // 这段函数只是执行一次

  fd = uv__eventfd2(0, UV__EFD_CLOEXEC | UV__EFD_NONBLOCK);
  if (fd != -1)             // fd == -1 创建文件描述符出错 
    return fd;              //返回 文件描述符( wakeUpFd )

  if (errno != ENOSYS)      //系统不支持 eventfd2
    return -errno;

  no_eventfd2 = 1;

skip_eventfd2:

  if (no_eventfd)
    goto skip_eventfd;

  fd = uv__eventfd(0);       //使用 eventfd
  if (fd != -1) {
    uv__cloexec(fd, 1);      //设置 close-on-process
    uv__nonblock(fd, 1);     //设置 nonblock 
    return fd;               //返回文件描述符(wakeUpFd)
  }

  if (errno != ENOSYS)       //系统不支持该调用
    return -errno;

  no_eventfd = 1;

skip_eventfd:

#endif

  return -ENOSYS;
}

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
* ���ؿɼ�
*
*/
static void uv__async_event(uv_loop_t* loop,
                            struct uv__async* w,
                            unsigned int nevents);
static int uv__async_eventfd(void);

/*
*
* ��ʼ�� uv_async_t handle ( �첽���� ) (���ⲿ�ṩ�ĺ����ӿ�)
*
* uv_async_cb cb : �첽�ص�����
*
*  �ú����� uv_loop_init(...) �б�����,�첽�ص����������ó�
* �̳߳��е� uv_work_done(...)
*
*/

int uv_async_init(uv_loop_t* loop, uv_async_t* handle, uv_async_cb async_cb) {
  int err;
                  // uv_loop_t    uv__async         uv__async_cb
  err = uv__async_start(loop, &loop->async_watcher, uv__async_event);  //uv__async_event : �����첽�ص��ĺ��ĺ��� 
  if (err)
    return err;

  uv__handle_init(loop, (uv_handle_t*)handle, UV_ASYNC);
  handle->async_cb = async_cb;                      // �����ó�Ϊ�̳߳��е� uv_work_done(...) 
  handle->pending = 0;

  QUEUE_INSERT_TAIL(&loop->async_handles, &handle->queue); //�� this un_async_t ���뵽 loop->async_handles ĩβ
  uv__handle_start(handle);

  return 0;
}

/*
*
* ��һ���첽����(handle)�з����ź�
* 
* һ��һ���첽����( uv_async_t ) ��ע�ᵽһ�� EventLoop( uv_loop_t )��
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
* �ر������첽����(handle)
*
*/
void uv__async_close(uv_async_t* handle) {
  QUEUE_REMOVE(&handle->queue); // �Ӷ������Ƴ� this �ڵ�
  uv__handle_stop(handle);
}

/*
*
* �첽�¼�����������
*
* ���� loop �е� async_handles ,���ε�����ص�����
*
* ( uv__async_start(...) �еĻص�����,�����ó�Ϊ uv__async �еĻص����� )
*
* �� uv__async_io �лص��� 
*/
static void uv__async_event(uv_loop_t* loop,
                            struct uv__async* w,
                            unsigned int nevents) {
  QUEUE* q;
  uv_async_t* h;

  QUEUE_FOREACH(q, &loop->async_handles) {     //�൱�� for ѭ��
    h = QUEUE_DATA(q, uv_async_t, queue);      //ȡ������ͷ����Ԫ��   

    if (cmpxchgi(&h->pending, 1, 0) == 0)
      continue;

    if (h->async_cb == NULL)
      continue;
    h->async_cb(h);                            //���� uv_async �еĻص�����
  }
}

/*
*
* �첽�� IO �¼����� epoll ��֪ͨ 
*
* �൱��һ�����Ӻ��� ( hook ), ʵ���ϵ��õ��� uv__async �еĻص�����
*
* �� uv__io_init �����ó�Ϊ async �е� io �Ļص�����,���� epoll
*֪ͨʱ,������������
* 
*/
static void uv__async_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  struct uv__async* wa;
  char buf[1024];
  unsigned n;
  ssize_t r;

  n = 0;
  for (;;) {                             // ΪʲôҪ����ѭ���� ? IO���������������ۻ��ģ���Ҫһ���Խ����ݶ�����������Ӱ����һ��.
    r = read(w->fd, buf, sizeof(buf));   // �� uv__io_t �е��ļ��������ж�ȡ���� 

    if (r > 0)                           // r ���������ݵ�λ�� ( byte )
      n += r;                            // r ������������� for ѭ��

    if (r == sizeof(buf))                //
      continue;

    if (r != -1)                         // �����ݹ���
      break;

    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;

    if (errno == EINTR)                  // �ж϶������Ĵ���  
      continue;

    abort();
  }

  wa = container_of(w, struct uv__async, io_watcher); // �� uv__io_t �� ���Ƴ� uv__async

#if defined(__linux__)       // linux ƽ̨��
  if (wa->wfd == -1) {       // wa->wfd == -1
    uint64_t val;
    assert(n == sizeof(val)); // n == 8 ( byte ) ������� 8 byte ��˵������û�ж�����
    memcpy(&val, buf, sizeof(val));  /* Avoid alignment issues. */
    wa->cb(loop, wa, val);           // ���õ��� uv__async �еĻص�����  
    return;
  }
#endif

  wa->cb(loop, wa, n);
}

/*
*
*  �첽����
*
*/
void uv__async_send(struct uv__async* wa) {
  const void* buf;
  ssize_t len;
  int fd;
  int r;

  buf = "";
  len = 1;
  fd = wa->wfd;  //�ļ������� ���ڻ���ĳ���߳� ����˵�ǻ��� epoll(...)

#if defined(__linux__)                   // linux ƽ̨��
  if (fd == -1) {
    static const uint64_t val = 1;
    buf = &val;
    len = sizeof(val);
    fd = wa->io_watcher.fd;  /* eventfd */ // �� uv__io_t �е� fd �����ֽ�
  }
#endif

  do
    r = write(fd, buf, len);            // д�ֽ� ( 8 byte )  
  while (r == -1 && errno == EINTR);    // �� wakeUpFd ��д�� 1 byte�ֽ� ���ڻ���
                                        // �����loop �е� epoll(...) 

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
* �첽����ʼ����
* 
* uv_loop_t loop : �첽�������� EventLoop
*
* uv__async wa : �� EventLoop �м�ص��첽����( ����첽�ź� )
*
* uv__async_cb cb : �첽����Ļص�����(�����óɱ��� uv__async_event )
*
*/
int uv__async_start(uv_loop_t* loop, struct uv__async* wa, uv__async_cb cb) {
  int pipefd[2];
  int err;

  if (wa->io_watcher.fd != -1)  // io_watcher.fd ��ʼ��Ϊ-1
    return 0;                   // io_watcher.fd!=-1 ˵�� uv__async_start(...) �Ѿ������ù���

  err = uv__async_eventfd();     
  if (err >= 0) {
    pipefd[0] = err;            // ���� pipefd[0] Ϊ uv__io_t �е� fd
    pipefd[1] = -1;             // ���� pipefd[1] Ϊ uv__async �е� wfd
  }
  else if (err == -ENOSYS) {   //ϵͳ��֧�� eventfd ,�򴴽��ܵ�
    err = uv__make_pipe(pipefd, UV__F_NONBLOCK);
#if defined(__linux__)         // linux ƽ̨ 
    /* Save a file descriptor by opening one of the pipe descriptors as
     * read/write through the procfs.( �����ļ�ϵͳ )  That file descriptor can then
     * function as both ends of the pipe.
     */
    if (err == 0) {            //�ܵ������ɹ�  
      char buf[32];
      int fd;

      snprintf(buf, sizeof(buf), "/proc/self/fd/%d", pipefd[0]); // "/proc/self/fd/%d" ͨ���ļ���������ȡ�ļ��ľ���·��
      fd = uv__open_cloexec(buf, O_RDWR);  // ��дģʽ���ļ�������                   
      if (fd >= 0) {
        uv__close(pipefd[0]);              // �ȹر� ������
        uv__close(pipefd[1]);
        pipefd[0] = fd;                    // �ļ�����������
        pipefd[1] = fd;
      }
    }
#endif
  }

  if (err < 0)                             // �ܵ��������ɹ�
    return err;
  
  // ��ʼ�� uv__io_t ( io_watcher ) 
  // uv__async_io �� io_watcher �еĻص����� ���� io �Ļص�����Ϊ�ϲ� async �Ļص�����
  //�� epoll ��֪ͨ����ʱ,ִ�еľ��� async �еĺ����� 
  // pipefd[0] �� io_watcher �е� fd
  uv__io_init(&wa->io_watcher, uv__async_io, pipefd[0]);  // uv__io_init �еĻص�������
                                                          //static uv__async_io(...) 
  //���ù�ע���¼� UV_POLLIN
  uv__io_start(loop, &wa->io_watcher, UV__POLLIN);
  
  wa->wfd = pipefd[1];                                    // pipefd[1]
  wa->cb = cb;                                            // ������ async �еĻص�����
                                                          //�� uv__async_io �б��ص� 
  return 0;
}

/*
*
* ֹͣ�첽����( handle )
*
* io_watcher ��һ�� uv__io_t �Ľṹ
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

�����첽�� eventfd

*/
static int uv__async_eventfd() {
#if defined(__linux__)
  static int no_eventfd2;
  static int no_eventfd;
  int fd;

  if (no_eventfd2)           // no_eventfd2!=0 ʱ ֱ����ת
    goto skip_eventfd2;     // ��κ���ֻ��ִ��һ��

  fd = uv__eventfd2(0, UV__EFD_CLOEXEC | UV__EFD_NONBLOCK);
  if (fd != -1)             // fd == -1 �����ļ����������� 
    return fd;              //���� �ļ�������( wakeUpFd )

  if (errno != ENOSYS)      //ϵͳ��֧�� eventfd2
    return -errno;

  no_eventfd2 = 1;

skip_eventfd2:

  if (no_eventfd)
    goto skip_eventfd;

  fd = uv__eventfd(0);       //ʹ�� eventfd
  if (fd != -1) {
    uv__cloexec(fd, 1);      //���� close-on-process
    uv__nonblock(fd, 1);     //���� nonblock 
    return fd;               //�����ļ�������(wakeUpFd)
  }

  if (errno != ENOSYS)       //ϵͳ��֧�ָõ���
    return -errno;

  no_eventfd = 1;

skip_eventfd:

#endif

  return -ENOSYS;
}

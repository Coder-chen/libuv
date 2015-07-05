/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
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

#include "uv-common.h"

#if !defined(_WIN32)
# include "unix/internal.h"
#else
# include "win/req-inl.h"
/* TODO(saghul): unify internal req functions */
static void uv__req_init(uv_loop_t* loop,
                         uv_req_t* req,
                         uv_req_type type) {
  uv_req_init(loop, req);      // uv_req_init(...) 宏定义 loop-watcher.c 
  req->type = type;
  uv__req_register(loop, req); // 将 req->active_queue 插入到 loop->active_reqe的末尾
}
# define uv__req_init(loop, req, type) \
    uv__req_init((loop), (uv_req_t*)(req), (type))
#endif

#include <stdlib.h>

#define MAX_THREADPOOL_SIZE 128

static uv_once_t once = UV_ONCE_INIT;
static uv_cond_t cond;             //全局条件变量 
static uv_mutex_t mutex;           //全局锁变量
static unsigned int nthreads;
static uv_thread_t* threads;
static uv_thread_t default_threads[4];
static QUEUE exit_message;         // void* [2] 
static QUEUE wq;                   // void* [2]
static volatile int initialized;


static void uv__cancelled(struct uv__work* w) {
  abort();
}


/* To avoid deadlock with uv_cancel() it's crucial that the worker
 * never holds the global mutex and the loop-local mutex at the same time.
 */

/*
*
*  线程的执行入口函数;
*
*  线程池初始化后，所以创建的线程都开始执行 worker 函数；
*
*  每个线程在 worker 中都无限循环，等待条件变量的到来 等待的时候是 sleep 的，不会占用计算资源
* 
*  条件变量由 post 函数发出
*/
static void worker(void* arg) {
  struct uv__work* w;
  QUEUE* q;

  (void) arg;                       //传入的 arg 为 NULL

  for (;;) {                        //无限循环 for(;;)
    uv_mutex_lock(&mutex);          //上锁    

    while (QUEUE_EMPTY(&wq))        //任务队列(wq)为空
      uv_cond_wait(&cond, &mutex);  //等待条件发生 (前面已经上锁，还没有解锁? 系统会自动解锁) 
                                    //条件变量到了后，占用全局锁
                                    
    q = QUEUE_HEAD(&wq);            //查看队列头节点 (并不出队列，退出时，要让所有的线程都知道)

    if (q == &exit_message)
      uv_cond_signal(&cond);        //发送信号给所有的线程
    else {
      QUEUE_REMOVE(q);              //从队列 (wq) 头节点中移除 q
      QUEUE_INIT(q);  /* Signal uv_cancel() that the work req is
                             executing. */
    }

    uv_mutex_unlock(&mutex);       //解锁 给其余线程获得锁的机会 

    if (q == &exit_message)
      break;                       //跳出循环 等待被 join()

    w = QUEUE_DATA(q, struct uv__work, wq);  //q中的数据 取出节点( struct uv_work )
    w->work(w);                   // 执行队列 wq (struct uv_work 队列)中的工作线程函数                            
                                  // 这个一般是文件系统中的函数  
    uv_mutex_lock(&w->loop->wq_mutex);       //一个loop中的 wq 可以被多个线程看到 这里锁住的是 loop 中的锁 ，而不是线程池的锁
                                             //线程池的锁管 static QUEUE wq 
                                             //loop中的锁管 loop 中的 wq
    // 各个分离的(struct uv_work)被按照顺序插入到 static QUEUE wq 中，随着线程池
    // 的运行，各个(struct uv_work)开始被执行，执行完后的节点(struct uv_work)被插入
    // 到对应 loop 中的 wq 中，表示已经执行完了的 (struct uv_work) 节点
    
    w->work = NULL;  /* Signal uv_cancel() that the work req is done
                        executing. */
    QUEUE_INSERT_TAIL(&w->loop->wq, &w->wq); //将已经执行完了节点 (struct uv_work) 插入到队尾
    uv_async_send(&w->loop->wq_async);       //发送异步信号 
    uv_mutex_unlock(&w->loop->wq_mutex);
  }
}

/*

从任务队列队尾插入新的任务

QUEUE* q 是 struct uv_work 的成员变量，通过里面的 q (void* wq[2]== q)
将各个待执行的 工作函数(work) 完成函数(done) 连接成一个队列，供线程调用

*/
static void post(QUEUE* q) {
  uv_mutex_lock(&mutex);        //(另外一个线程在等待条件变量 系统会解锁) 上锁
  QUEUE_INSERT_TAIL(&wq, q);    //从队尾插入任务 
  uv_cond_signal(&cond);        //发送信号  ( 先发送信号 ( 线程工作队列里有新的任务了 )再解锁 )
  uv_mutex_unlock(&mutex);      //解锁 （如果不解锁，等待信号量的线程 将不能重新上锁)
}


#ifndef _WIN32          // linux下
UV_DESTRUCTOR(static void cleanup(void)) {
  unsigned int i;

  if (initialized == 0)
    return;

  post(&exit_message);  //提交 exit_message 到 线程的 wq 队列中

  for (i = 0; i < nthreads; i++)
    if (uv_thread_join(threads + i)) //等待线程池中所有的线程返回
      abort();

  if (threads != default_threads)
    uv__free(threads);

  uv_mutex_destroy(&mutex);
  uv_cond_destroy(&cond);

  threads = NULL;
  nthreads = 0;
  initialized = 0;
}
#endif

/*

线程池初始化；
创建多个线程，执行线程统一的执行函数 worker ( void* worker(void*) )

*/
static void init_once(void) {
  unsigned int i;
  const char* val;

  nthreads = ARRAY_SIZE(default_threads);   //线程池默认的大小
  val = getenv("UV_THREADPOOL_SIZE");            //获取环境中变量的值 (UV_THREADPOOL_SIZE 由 shell 设置)
  if (val != NULL)
    nthreads = atoi(val);                   //shell 中 设置了 UV_THREADPOOL_SIZE         
  if (nthreads == 0)
    nthreads = 1;   
  if (nthreads > MAX_THREADPOOL_SIZE)
    nthreads = MAX_THREADPOOL_SIZE;

  threads = default_threads;                //threads 指向全局的默认的存放线程ID的数组
  if (nthreads > ARRAY_SIZE(default_threads)) {
    threads = uv__malloc(nthreads * sizeof(threads[0]));
    if (threads == NULL) {
      nthreads = ARRAY_SIZE(default_threads);
      threads = default_threads;
    }
  }

  if (uv_cond_init(&cond))                //构造条件变量
    abort();

  if (uv_mutex_init(&mutex))              //构造互斥锁
    abort();

  QUEUE_INIT(&wq);                        //初始化任务队列 

  for (i = 0; i < nthreads; i++)
    if (uv_thread_create(threads + i, worker, NULL))  //创建线程执行 worker
      abort();

  initialized = 1;                       //已经初始化的标志
}

/*
*
*  提交任务:
*  
*  uv_loop_t* loop: loop 是该线程的事件循环
*
*  struvt uv__work 是线程执行任务 (task) 的最小单元,其中包含了工作函数(work),
*和完成函数(done)的函数指针，由线程负责调用。 
*   
*  结构体用里面的 void* wq[2] 成员来组成一个队列，供线程调用各个工作函数，完成函数
*/
void uv__work_submit(uv_loop_t* loop,
                     struct uv__work* w,
                     void (*work)(struct uv__work* w),
                     void (*done)(struct uv__work* w, int status)) {
  uv_once(&once, init_once);  // 执行初始化操作( 保证本进程中 init_once 函数只是执行一次 )
  w->loop = loop;
  w->work = work;             // 设置 uv__work 中的 work 回掉函数
  w->done = done;             // 设置 uv__work 中的 done 回掉函数
  post(&w->wq); 			  // 提交到线程池中，会引起条件变量的变化，执行 work(...) 函数
}


static int uv__work_cancel(uv_loop_t* loop, uv_req_t* req, struct uv__work* w) {
  int cancelled;

  uv_mutex_lock(&mutex);
  uv_mutex_lock(&w->loop->wq_mutex);

  cancelled = !QUEUE_EMPTY(&w->wq) && w->work != NULL;
  if (cancelled)
    QUEUE_REMOVE(&w->wq);

  uv_mutex_unlock(&w->loop->wq_mutex);
  uv_mutex_unlock(&mutex);

  if (!cancelled)
    return UV_EBUSY;

  w->work = uv__cancelled;
  uv_mutex_lock(&loop->wq_mutex);
  QUEUE_INSERT_TAIL(&loop->wq, &w->wq);
  uv_async_send(&loop->wq_async);
  uv_mutex_unlock(&loop->wq_mutex);

  return 0;
}


void uv__work_done(uv_async_t* handle) {
  struct uv__work* w;
  uv_loop_t* loop;
  QUEUE* q;
  QUEUE wq;
  int err;

  loop = container_of(handle, uv_loop_t, wq_async);  // 从 wq_async 中推导出 uv_loop_t
  QUEUE_INIT(&wq);

  uv_mutex_lock(&loop->wq_mutex);                    // 异步队列的锁
  if (!QUEUE_EMPTY(&loop->wq)) {
    q = QUEUE_HEAD(&loop->wq);
    QUEUE_SPLIT(&loop->wq, q, &wq);
  }
  uv_mutex_unlock(&loop->wq_mutex);

  while (!QUEUE_EMPTY(&wq)) {
    q = QUEUE_HEAD(&wq);
    QUEUE_REMOVE(q);

    w = container_of(q, struct uv__work, wq);        
    err = (w->work == uv__cancelled) ? UV_ECANCELED : 0;
    w->done(w, err);                                // 执行回掉函数
  }
}


static void uv__queue_work(struct uv__work* w) {
  uv_work_t* req = container_of(w, uv_work_t, work_req);

  req->work_cb(req);
}


static void uv__queue_done(struct uv__work* w, int err) {
  uv_work_t* req;

  req = container_of(w, uv_work_t, work_req);
  uv__req_unregister(req->loop, req);

  if (req->after_work_cb == NULL)
    return;

  req->after_work_cb(req, err);
}


int uv_queue_work(uv_loop_t* loop,
                  uv_work_t* req,
                  uv_work_cb work_cb,
                  uv_after_work_cb after_work_cb) {
  if (work_cb == NULL)
    return UV_EINVAL;

  uv__req_init(loop, req, UV_WORK);
  req->loop = loop;
  req->work_cb = work_cb;
  req->after_work_cb = after_work_cb;
  uv__work_submit(loop, &req->work_req, uv__queue_work, uv__queue_done);
  return 0;
}


int uv_cancel(uv_req_t* req) {
  struct uv__work* wreq;
  uv_loop_t* loop;

  switch (req->type) {
  case UV_FS:
    loop =  ((uv_fs_t*) req)->loop;
    wreq = &((uv_fs_t*) req)->work_req;
    break;
  case UV_GETADDRINFO:
    loop =  ((uv_getaddrinfo_t*) req)->loop;
    wreq = &((uv_getaddrinfo_t*) req)->work_req;
    break;
  case UV_GETNAMEINFO:
    loop = ((uv_getnameinfo_t*) req)->loop;
    wreq = &((uv_getnameinfo_t*) req)->work_req;
    break;
  case UV_WORK:
    loop =  ((uv_work_t*) req)->loop;
    wreq = &((uv_work_t*) req)->work_req;
    break;
  default:
    return UV_EINVAL;
  }

  return uv__work_cancel(loop, req, wreq);
}

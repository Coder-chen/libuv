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
  uv_req_init(loop, req);      // uv_req_init(...) �궨�� loop-watcher.c 
  req->type = type;
  uv__req_register(loop, req); // �� req->active_queue ���뵽 loop->active_reqe��ĩβ
}
# define uv__req_init(loop, req, type) \
    uv__req_init((loop), (uv_req_t*)(req), (type))
#endif

#include <stdlib.h>

#define MAX_THREADPOOL_SIZE 128

static uv_once_t once = UV_ONCE_INIT;
static uv_cond_t cond;             //ȫ���������� 
static uv_mutex_t mutex;           //ȫ��������
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
*  �̵߳�ִ����ں���;
*
*  �̳߳س�ʼ�������Դ������̶߳���ʼִ�� worker ������
*
*  ÿ���߳��� worker �ж�����ѭ�����ȴ����������ĵ��� �ȴ���ʱ���� sleep �ģ�����ռ�ü�����Դ
* 
*  ���������� post ��������
*/
static void worker(void* arg) {
  struct uv__work* w;
  QUEUE* q;

  (void) arg;                       //����� arg Ϊ NULL

  for (;;) {                        //����ѭ�� for(;;)
    uv_mutex_lock(&mutex);          //����    

    while (QUEUE_EMPTY(&wq))        //�������(wq)Ϊ��
      uv_cond_wait(&cond, &mutex);  //�ȴ��������� (ǰ���Ѿ���������û�н���? ϵͳ���Զ�����) 
                                    //�����������˺�ռ��ȫ����
                                    
    q = QUEUE_HEAD(&wq);            //�鿴����ͷ�ڵ� (���������У��˳�ʱ��Ҫ�����е��̶߳�֪��)

    if (q == &exit_message)
      uv_cond_signal(&cond);        //�����źŸ����е��߳�
    else {
      QUEUE_REMOVE(q);              //�Ӷ��� (wq) ͷ�ڵ����Ƴ� q
      QUEUE_INIT(q);  /* Signal uv_cancel() that the work req is
                             executing. */
    }

    uv_mutex_unlock(&mutex);       //���� �������̻߳�����Ļ��� 

    if (q == &exit_message)
      break;                       //����ѭ�� �ȴ��� join()

    w = QUEUE_DATA(q, struct uv__work, wq);  //q�е����� ȡ���ڵ�( struct uv_work )
    w->work(w);                   // ִ�ж��� wq (struct uv_work ����)�еĹ����̺߳���                            
                                  // ���һ�����ļ�ϵͳ�еĺ���  
    uv_mutex_lock(&w->loop->wq_mutex);       //һ��loop�е� wq ���Ա�����߳̿��� ������ס���� loop �е��� ���������̳߳ص���
                                             //�̳߳ص����� static QUEUE wq 
                                             //loop�е����� loop �е� wq
    // ���������(struct uv_work)������˳����뵽 static QUEUE wq �У������̳߳�
    // �����У�����(struct uv_work)��ʼ��ִ�У�ִ�����Ľڵ�(struct uv_work)������
    // ����Ӧ loop �е� wq �У���ʾ�Ѿ�ִ�����˵� (struct uv_work) �ڵ�
    
    w->work = NULL;  /* Signal uv_cancel() that the work req is done
                        executing. */
    QUEUE_INSERT_TAIL(&w->loop->wq, &w->wq); //���Ѿ�ִ�����˽ڵ� (struct uv_work) ���뵽��β
    uv_async_send(&w->loop->wq_async);       //�����첽�ź� 
    uv_mutex_unlock(&w->loop->wq_mutex);
  }
}

/*

��������ж�β�����µ�����

QUEUE* q �� struct uv_work �ĳ�Ա������ͨ������� q (void* wq[2]== q)
��������ִ�е� ��������(work) ��ɺ���(done) ���ӳ�һ�����У����̵߳���

*/
static void post(QUEUE* q) {
  uv_mutex_lock(&mutex);        //(����һ���߳��ڵȴ��������� ϵͳ�����) ����
  QUEUE_INSERT_TAIL(&wq, q);    //�Ӷ�β�������� 
  uv_cond_signal(&cond);        //�����ź�  ( �ȷ����ź� ( �̹߳������������µ������� )�ٽ��� )
  uv_mutex_unlock(&mutex);      //���� ��������������ȴ��ź������߳� ��������������)
}


#ifndef _WIN32          // linux��
UV_DESTRUCTOR(static void cleanup(void)) {
  unsigned int i;

  if (initialized == 0)
    return;

  post(&exit_message);  //�ύ exit_message �� �̵߳� wq ������

  for (i = 0; i < nthreads; i++)
    if (uv_thread_join(threads + i)) //�ȴ��̳߳������е��̷߳���
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

�̳߳س�ʼ����
��������̣߳�ִ���߳�ͳһ��ִ�к��� worker ( void* worker(void*) )

*/
static void init_once(void) {
  unsigned int i;
  const char* val;

  nthreads = ARRAY_SIZE(default_threads);   //�̳߳�Ĭ�ϵĴ�С
  val = getenv("UV_THREADPOOL_SIZE");            //��ȡ�����б�����ֵ (UV_THREADPOOL_SIZE �� shell ����)
  if (val != NULL)
    nthreads = atoi(val);                   //shell �� ������ UV_THREADPOOL_SIZE         
  if (nthreads == 0)
    nthreads = 1;   
  if (nthreads > MAX_THREADPOOL_SIZE)
    nthreads = MAX_THREADPOOL_SIZE;

  threads = default_threads;                //threads ָ��ȫ�ֵ�Ĭ�ϵĴ���߳�ID������
  if (nthreads > ARRAY_SIZE(default_threads)) {
    threads = uv__malloc(nthreads * sizeof(threads[0]));
    if (threads == NULL) {
      nthreads = ARRAY_SIZE(default_threads);
      threads = default_threads;
    }
  }

  if (uv_cond_init(&cond))                //������������
    abort();

  if (uv_mutex_init(&mutex))              //���컥����
    abort();

  QUEUE_INIT(&wq);                        //��ʼ��������� 

  for (i = 0; i < nthreads; i++)
    if (uv_thread_create(threads + i, worker, NULL))  //�����߳�ִ�� worker
      abort();

  initialized = 1;                       //�Ѿ���ʼ���ı�־
}

/*
*
*  �ύ����:
*  
*  uv_loop_t* loop: loop �Ǹ��̵߳��¼�ѭ��
*
*  struvt uv__work ���߳�ִ������ (task) ����С��Ԫ,���а����˹�������(work),
*����ɺ���(done)�ĺ���ָ�룬���̸߳�����á� 
*   
*  �ṹ��������� void* wq[2] ��Ա�����һ�����У����̵߳��ø���������������ɺ���
*/
void uv__work_submit(uv_loop_t* loop,
                     struct uv__work* w,
                     void (*work)(struct uv__work* w),
                     void (*done)(struct uv__work* w, int status)) {
  uv_once(&once, init_once);  // ִ�г�ʼ������( ��֤�������� init_once ����ֻ��ִ��һ�� )
  w->loop = loop;
  w->work = work;             // ���� uv__work �е� work �ص�����
  w->done = done;             // ���� uv__work �е� done �ص�����
  post(&w->wq); 			  // �ύ���̳߳��У����������������ı仯��ִ�� work(...) ����
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

  loop = container_of(handle, uv_loop_t, wq_async);  // �� wq_async ���Ƶ��� uv_loop_t
  QUEUE_INIT(&wq);

  uv_mutex_lock(&loop->wq_mutex);                    // �첽���е���
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
    w->done(w, err);                                // ִ�лص�����
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

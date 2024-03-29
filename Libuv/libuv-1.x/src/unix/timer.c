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

#include "uv.h"
#include "internal.h"
#include "heap-inl.h"

#include <assert.h>
#include <limits.h>


static int timer_less_than(const struct heap_node* ha,
                           const struct heap_node* hb) {
  const uv_timer_t* a;
  const uv_timer_t* b;

  a = container_of(ha, const uv_timer_t, heap_node);
  b = container_of(hb, const uv_timer_t, heap_node);

  if (a->timeout < b->timeout)
    return 1;
  if (b->timeout < a->timeout)
    return 0;

  /* Compare start_id when both have the same timeout. start_id is
   * allocated with loop->timer_counter in uv_timer_start().
   */
  if (a->start_id < b->start_id)
    return 1;
  if (b->start_id < a->start_id)
    return 0;

  return 0;
}


int uv_timer_init(uv_loop_t* loop, uv_timer_t* handle) {
  uv__handle_init(loop, (uv_handle_t*)handle, UV_TIMER);
  handle->timer_cb = NULL;
  handle->repeat = 0;
  return 0;
}


int uv_timer_start(uv_timer_t* handle,
                   uv_timer_cb cb,
                   uint64_t timeout,
                   uint64_t repeat) {
  uint64_t clamped_timeout;

  if (cb == NULL)
    return -EINVAL;

  if (uv__is_active(handle))
    uv_timer_stop(handle);

  clamped_timeout = handle->loop->time + timeout;
  if (clamped_timeout < timeout)
    clamped_timeout = (uint64_t) -1;

  handle->timer_cb = cb;
  handle->timeout = clamped_timeout;
  handle->repeat = repeat;
  /* start_id is the second index to be compared in uv__timer_cmp() */
  handle->start_id = handle->loop->timer_counter++;

  heap_insert((struct heap*) &handle->loop->timer_heap,
              (struct heap_node*) &handle->heap_node,
              timer_less_than);
  uv__handle_start(handle);

  return 0;
}

/*
*
* 停止定时器
*/
int uv_timer_stop(uv_timer_t* handle) {
  if (!uv__is_active(handle))         // 查看定时器服务 ( handler ) 是否还运行
    return 0;

  heap_remove((struct heap*) &handle->loop->timer_heap,
              (struct heap_node*) &handle->heap_node,
              timer_less_than);      // 移除 timer <= loop time 定时器已经过期 
  uv__handle_stop(handle);           // 取消定时任务  

  return 0;
}


int uv_timer_again(uv_timer_t* handle) {
  if (handle->timer_cb == NULL)
    return -EINVAL;

  if (handle->repeat) {
    uv_timer_stop(handle);
    uv_timer_start(handle, handle->timer_cb, handle->repeat, handle->repeat);
  }

  return 0;
}


void uv_timer_set_repeat(uv_timer_t* handle, uint64_t repeat) {
  handle->repeat = repeat;
}


uint64_t uv_timer_get_repeat(const uv_timer_t* handle) {
  return handle->repeat;
}

/*
*
* 获取 poll 的 timeout
*
*/
int uv__next_timeout(const uv_loop_t* loop) {
  const struct heap_node* heap_node;
  const uv_timer_t* handle;
  uint64_t diff;

  heap_node = heap_min((const struct heap*) &loop->timer_heap); // 定时服务最小的节点信息
  if (heap_node == NULL)                       // 没有定时服务
    return -1; /* block indefinitely */  // epoll 继续等待下去

  handle = container_of(heap_node, const uv_timer_t, heap_node);
  if (handle->timeout <= loop->time)           // 定时服务有一部分已经过期                          
    return 0;                                  // epoll 立即返回

  diff = handle->timeout - loop->time;         // diff 定时服务发生的时间间隔
  if (diff > INT_MAX)
    diff = INT_MAX;

  return diff;                                 // epoll 等待的时间 
}

/*
*
* 启动定时器服务
*
*/
void uv__run_timers(uv_loop_t* loop) {
  struct heap_node* heap_node;
  uv_timer_t* handle;

  for (;;) {
    heap_node = heap_min((struct heap*) &loop->timer_heap);
    if (heap_node == NULL)   // 定时器为空 
      break;                  

    handle = container_of(heap_node, uv_timer_t, heap_node);  // 取出一个定时器服务( handle )
    if (handle->timeout > loop->time)  // -------------------------->
      break;                           //  now  timer(min) timer ...
                                       // 定时器未到时间
    uv_timer_stop(handle);             // 停止当前的定时器服务(handle)   
    uv_timer_again(handle);            // 如果定时器是 repeat 模式，则重新开始定时任务
    handle->timer_cb(handle);          // 调用定时器回掉函数
  }
}


void uv__timer_close(uv_timer_t* handle) {
  uv_timer_stop(handle);
}

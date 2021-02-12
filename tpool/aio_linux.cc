/* Copyright (C) 2019, 2021, MariaDB Corporation.

This program is free software; you can redistribute itand /or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111 - 1301 USA*/

#include "tpool_structs.h"
#include "tpool.h"

#ifdef HAVE_URING
#include <liburing.h>

#include <algorithm>
#include <vector>
#include <thread>
#include <mutex>

namespace
{

class aio_uring final : public tpool::aio
{
  aio_uring() {}

public:
  static aio_uring *create(int max_aio, tpool::thread_pool *tpool)
  {
    auto *result= new aio_uring;
    result->tpool_= tpool;

    if (io_uring_queue_init(max_aio, &result->uring_, 0) != 0)
    {
      fprintf(stderr, "io_uring_queue_init() failed with errno %d\n", errno);
      delete result;
      return nullptr;
    }

    result->thread_= std::thread{thread_routine, result};

    return result;
  }

  ~aio_uring()
  {
    {
      std::lock_guard<std::mutex> _(mutex_);
      io_uring_sqe *sqe= io_uring_get_sqe(&uring_);
      io_uring_prep_nop(sqe);
      io_uring_sqe_set_data(sqe, nullptr);
      auto ret= io_uring_submit(&uring_);
      if (ret != 1)
      {
        fprintf(stderr,
                "io_uring_submit() returned %d during shutdown: this may "
                "cause a hang",
                ret);
        abort();
      }
    }
    thread_.join();
    io_uring_queue_exit(&uring_);
  }

  int submit_io(tpool::aiocb *cb) override
  {
    // The whole operation since io_uring_get_sqe() and till io_uring_submit()
    // must be atomical. This is because liburing provides thread-unsafe calls.
    std::lock_guard<std::mutex> _(mutex_);

    io_uring_sqe *sqe= io_uring_get_sqe(&uring_);
    if (cb->m_opcode == tpool::aio_opcode::AIO_PREAD)
      io_uring_prep_read(sqe, cb->m_fh, cb->m_buffer, cb->m_len, cb->m_offset);
    else
      io_uring_prep_write(sqe, cb->m_fh, cb->m_buffer, cb->m_len,
                          cb->m_offset);
    io_uring_sqe_set_data(sqe, cb);

    return io_uring_submit(&uring_) == 1 ? 0 : -1;
  }

  int bind(native_file_handle &fd) override
  {
    std::lock_guard<std::mutex> _(files_mutex_);
    auto it= std::lower_bound(files_.begin(), files_.end(), fd);
    assert(it == files_.end() || *it != fd);
    files_.insert(it, fd);
    return io_uring_register_files_update(&uring_, 0, files_.data(),
                                          files_.size());
  }

  int unbind(const native_file_handle &fd) override
  {
    std::lock_guard<std::mutex> _(files_mutex_);
    auto it= std::lower_bound(files_.begin(), files_.end(), fd);
    assert(*it == fd);
    files_.erase(it);
    return io_uring_register_files_update(&uring_, 0, files_.data(),
                                          files_.size());
  }

private:
  static void thread_routine(aio_uring *aio)
  {
    for (;;)
    {
      io_uring_cqe *cqe;
      if (int ret= io_uring_wait_cqe(&aio->uring_, &cqe))
      {
        fprintf(stderr, "io_uring_wait_cqe() returned %d\n", ret);
        abort();
      }

      auto *iocb= (tpool::aiocb *) io_uring_cqe_get_data(cqe);
      if (!iocb)
        break;

      int res= cqe->res;
      if (res < 0)
      {
        iocb->m_err= -res;
        iocb->m_ret_len= 0;
      }
      else
      {
        iocb->m_err= 0;
        iocb->m_ret_len= res;
      }

      io_uring_cqe_seen(&aio->uring_, cqe);

      iocb->m_internal_task.m_func= iocb->m_callback;
      iocb->m_internal_task.m_arg= iocb;
      iocb->m_internal_task.m_group= iocb->m_group;
      aio->tpool_->submit_task(&iocb->m_internal_task);
    }
  }

  io_uring uring_;
  std::mutex mutex_;
  tpool::thread_pool *tpool_;
  std::thread thread_;

  std::vector<native_file_handle> files_;
  std::mutex files_mutex_;
};

}

namespace tpool
{

aio *create_linux_aio(thread_pool *pool, int max_aio)
{
  return aio_uring::create(max_aio, pool);
}

}

#elif defined(LINUX_NATIVE_AIO)
# include <thread>
# include <atomic>
# include <libaio.h>
# include <sys/syscall.h>

namespace {

/**
  Invoke the io_getevents() system call, without timeout parameter.

  @param ctx     context from io_setup()
  @param min_nr  minimum number of completion events to wait for
  @param nr      maximum number of completion events to collect
  @param ev      the collected events

  In https://pagure.io/libaio/c/7cede5af5adf01ad26155061cc476aad0804d3fc
  the io_getevents() implementation in libaio was "optimized" so that it
  would elide the system call when there are no outstanding requests
  and a timeout was specified.

  The libaio code for dereferencing ctx would occasionally trigger
  SIGSEGV if io_destroy() was concurrently invoked from another thread.
  Hence, we have to use the raw system call.

  WHY are we doing this at all?
  Because we want io_destroy() from another thread to interrupt io_getevents().

  And, WHY do we want io_destroy() from another thread to interrupt
  io_getevents()?

  Because there is no documented, libaio-friendly and race-condition-free way to
  interrupt io_getevents(). io_destroy() coupled with raw syscall seemed to work
  for us so far.

  Historical note : in the past, we used io_getevents with timeouts. We'd wake
  up periodically, check for shutdown flag, return from the main routine.
  This was admittedly safer, yet it did cost periodic wakeups, which we are not
  willing to do anymore.

  @note we also rely on the undocumented property, that io_destroy(ctx)
  will make this version of io_getevents return EINVAL.
*/
int my_getevents(io_context_t ctx, long min_nr, long nr, io_event *ev)
{
  int saved_errno= errno;
  int ret= syscall(__NR_io_getevents, reinterpret_cast<long>(ctx),
                   min_nr, nr, ev, 0);
  if (ret < 0)
  {
    ret= -errno;
    errno= saved_errno;
  }
  return ret;
}


/*
  Linux AIO implementation, based on native AIO.
  Needs libaio.h and -laio at the compile time.

  io_submit() is used to submit async IO.

  A single thread will collect the completion notification
  with io_getevents() and forward io completion callback to
  the worker threadpool.
*/

class aio_linux final : public tpool::aio
{
  tpool::thread_pool *m_pool;
  io_context_t m_io_ctx;
  std::thread m_getevent_thread;
  static std::atomic<bool> shutdown_in_progress;

  static void getevent_thread_routine(aio_linux *aio)
  {
    /*
      We collect events in small batches to hopefully reduce the
      number of system calls.
    */
    constexpr unsigned MAX_EVENTS= 256;

    io_event events[MAX_EVENTS];
    for (;;)
    {
      switch (int ret= my_getevents(aio->m_io_ctx, 1, MAX_EVENTS, events)) {
      case -EINTR:
        continue;
      case -EINVAL:
        if (shutdown_in_progress)
          return;
        /* fall through */
      default:
        if (ret < 0)
        {
          fprintf(stderr, "io_getevents returned %d\n", ret);
          abort();
          return;
        }
        for (int i= 0; i < ret; i++)
        {
          const io_event &event= events[i];
          auto *iocb= static_cast<tpool::aiocb*>(event.obj);
          if (static_cast<int>(event.res) < 0)
          {
            iocb->m_err= -event.res;
            iocb->m_ret_len= 0;
          }
          else
          {
            iocb->m_ret_len= event.res;
            iocb->m_err= 0;
          }
          iocb->m_internal_task.m_func= iocb->m_callback;
          iocb->m_internal_task.m_arg= iocb;
          iocb->m_internal_task.m_group= iocb->m_group;
          aio->m_pool->submit_task(&iocb->m_internal_task);
        }
      }
    }
  }

public:
  aio_linux(io_context_t ctx, tpool::thread_pool *pool)
    : m_pool(pool), m_io_ctx(ctx),
    m_getevent_thread(getevent_thread_routine, this)
  {
  }

  ~aio_linux()
  {
    shutdown_in_progress= true;
    io_destroy(m_io_ctx);
    m_getevent_thread.join();
    shutdown_in_progress= false;
  }

  int submit_io(tpool::aiocb *cb) override
  {
    io_prep_pread(static_cast<iocb*>(cb), cb->m_fh, cb->m_buffer, cb->m_len,
                  cb->m_offset);
    if (cb->m_opcode != tpool::aio_opcode::AIO_PREAD)
      cb->aio_lio_opcode= IO_CMD_PWRITE;
    iocb *icb= static_cast<iocb*>(cb);
    int ret= io_submit(m_io_ctx, 1, &icb);
    if (ret == 1)
      return 0;
    errno= -ret;
    return -1;
  }

  int bind(native_file_handle&) override { return 0; }
  int unbind(const native_file_handle&) override { return 0; }
};

std::atomic<bool> aio_linux::shutdown_in_progress;

}

namespace tpool
{

aio *create_linux_aio(thread_pool *pool, int max_io)
{
  io_context_t ctx;
  memset(&ctx, 0, sizeof ctx);
  if (int ret= io_setup(max_io, &ctx))
  {
    fprintf(stderr, "io_setup(%d) returned %d\n", max_io, ret);
    return nullptr;
  }
  return new aio_linux(ctx, pool);
}

}
#else
namespace tpool
{
aio *create_linux_aio(thread_pool *, int) { return nullptr; }
}
#endif

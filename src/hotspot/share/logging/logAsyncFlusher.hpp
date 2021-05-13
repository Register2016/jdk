/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */
#ifndef SHARE_LOGGING_ASYNC_FLUSHER_HPP
#define SHARE_LOGGING_ASYNC_FLUSHER_HPP
#include "logging/log.hpp"
#include "logging/logDecorations.hpp"
#include "logging/logFileOutput.hpp"
#include "logging/logMessageBuffer.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/nonJavaThread.hpp"
#include "utilities/hashtable.hpp"
#include "utilities/linkedlist.hpp"

template <typename E, MEMFLAGS F>
class LinkedListDeque : private LinkedListImpl<E, ResourceObj::C_HEAP, F> {
 private:
  LinkedListNode<E>* _tail;
  size_t _size;

 public:
  LinkedListDeque() : _tail(NULL), _size(0) {}
  void push_back(const E& e) {
    if (!_tail) {
      _tail = this->add(e);
    } else {
      _tail = this->insert_after(e, _tail);
    }

    ++_size;
  }

  void pop_all(LinkedList<E>* logs) {
    logs->move(static_cast<LinkedList<E>* >(this));
    _tail = NULL;
    _size = 0;
  }

  void pop_front() {
    LinkedListNode<E>* h = this->unlink_head();
    if (h == _tail) {
      _tail = NULL;
    }

    if (h != NULL) {
      --_size;
      this->delete_node(h);
    }
  }

  size_t size() const { return _size; }

  const E* front() const {
    return this->_head == NULL ? NULL : this->_head->peek();
  }

  const E* back() const {
    return _tail == NULL ? NULL : _tail->peek();
  }
};

class AsyncLogMessage {
  LogFileOutput&   _output;
  const LogDecorations _decorations;
  char*            _message;

public:
  AsyncLogMessage(LogFileOutput& output, const LogDecorations& decorations, char* msg)
    : _output(output), _decorations(decorations), _message(msg) {}

  void writeback();

  // two AsyncLogMessage are equal if both _output and _message are same.
  bool equals(const AsyncLogMessage& o) const {
    if (_message == o._message) {
      return &_output == &o._output;
    } else if (_message == NULL || o._message == NULL) {
      return false;
    } else {
      return &_output == &o._output && !strcmp(_message, o._message);
    }
  }

  const char* message() const { return _message; }
  LogFileOutput* output() const { return &_output; }
};

typedef LinkedListDeque<AsyncLogMessage, mtLogging> AsyncLogBuffer;
typedef KVHashtable<LogFileOutput*, uint32_t, mtLogging> AsyncLogMap;
struct AsyncLogMapIterator {
  bool do_entry(LogFileOutput* output, uint32_t* counter);
};

class LogAsyncFlusher : public NonJavaThread {
 private:
  static LogAsyncFlusher* _instance;

  enum class ThreadState {
    Running,
    Terminating,
    Terminated
  };

  volatile ThreadState _state;
  // The semantics of _lock is more like a Java monitor.
  // AssyncLog thread sleeps on _lock until the occupancy of the buffer is over 3/4, or timeout
  // It also acts as a mutex to consolidate buffer's MT-safety.
  Monitor _lock;
  AsyncLogMap _stats; // statistics of dropping messages.
  AsyncLogBuffer _buffer;

  // The memory use of each AsyncLogMessage(payload) consist of itself, a logDecoration object
  // and a variable-length c-string message.
  // A normal logging  message is smaller than vwrite_buffer_size, which is defined in logtagset.cpp
  const size_t _buffer_max_size = {AsyncLogBufferSize / (sizeof(AsyncLogMessage) + sizeof(LogDecorations) + vwrite_buffer_size)};
  static const int64_t ASYNCLOG_WAIT_TIMEOUT = 500; // timeout in millisecond.

  LogAsyncFlusher();
  void enqueue_impl(const AsyncLogMessage& msg);
  static void writeback(const LinkedList<AsyncLogMessage>& logs);
  void run() override;
  void pre_run() override {
    NonJavaThread::pre_run();
    log_debug(logging, thread)("starting AsyncLog Thread tid = " INTX_FORMAT, os::current_thread_id());
  }
  char* name() const override { return (char*)"AsyncLog Thread"; }

 public:
  void enqueue(LogFileOutput& output, const LogDecorations& decorations, const char* msg);
  void enqueue(LogFileOutput& output, LogMessageBuffer::Iterator msg_iterator);
  // Use with_lock = false at your own risk. It is only safe without any active reader.
  void flush(bool with_lock = true);

  // None of following functions are thread-safe.
  static LogAsyncFlusher* instance();
  // |JVM start | initialize() | ...java application... | terminate() |JVM exit|
  //                           p1                       p2
  // Logging sites spread across the entire JVM lifecycle. There're 2 synchronizaton points(p1 and p2).
  // Async logging EXCLUSIVELY takes over from synchronous logging from p1 to p2.
  // It's because current implementation is using some HotSpot runtime supports such mutex, threading etc.
  // They are not available in the very begining or threads are destroyed.
  static void initialize();
  static void terminate();
  static void abort();
};

#endif // SHARE_LOGGING_ASYNC_FLUSHER_HPP

// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

// What and why
// ============
//
// For general code making use of mutexes, use these ceph:: types.
// The key requirement is that you make use of the ceph::make_mutex()
// and make_recursive_mutex() factory methods, which take a string
// naming the mutex for the purposes of the lockdep debug variant.
//
// For legacy Mutex users that passed recursive=true, use
// ceph::make_recursive_mutex.  For legacy Mutex users that passed
// lockdep=false, use std::mutex directly.

#ifdef CEPH_DEBUG_MUTEX

// ============================================================================
// debug (lockdep-capable, various sanity checks and asserts)
// ============================================================================

#include "common/condition_variable_debug.h"
#include "common/mutex_debug.h"
#include "common/shared_mutex_debug.h"

namespace ceph {
  typedef ceph::mutex_debug mutex;
  typedef ceph::mutex_recursive_debug recursive_mutex;
  typedef ceph::condition_variable_debug condition_variable;
  typedef ceph::shared_mutex_debug shared_mutex;

  // pass arguments to mutex_debug ctor
  template <typename ...Args>
  mutex make_mutex(Args&& ...args) {
    return {std::forward<Args>(args)...};
  }

  // pass arguments to recursive_mutex_debug ctor
  template <typename ...Args>
  recursive_mutex make_recursive_mutex(Args&& ...args) {
    return {std::forward<Args>(args)...};
  }

  // pass arguments to shared_mutex_debug ctor
  template <typename ...Args>
  shared_mutex make_shared_mutex(Args&& ...args) {
    return {std::forward<Args>(args)...};
  }

  // debug methods
  #define ceph_mutex_is_locked(m) ((m).is_locked())
  #define ceph_mutex_is_not_locked(m) (!(m).is_locked())
  #define ceph_mutex_is_rlocked(m) ((m).is_rlocked())
  #define ceph_mutex_is_wlocked(m) ((m).is_wlocked())
  #define ceph_mutex_is_locked_by_me(m) ((m).is_locked_by_me())
  #define ceph_mutex_is_not_locked_by_me(m) (!(m).is_locked_by_me())
}

#else

// ============================================================================
// release (fast and minimal)
// ============================================================================

#include <condition_variable>
#include <mutex>
#include <shared_mutex>


namespace ceph {

  typedef std::mutex mutex;
  typedef std::recursive_mutex recursive_mutex;
  typedef std::condition_variable condition_variable;
  typedef std::shared_mutex shared_mutex;

  // discard arguments to make_mutex (they are for debugging only)
  template <typename ...Args>
  std::mutex make_mutex(Args&& ...args) {
    return {};
  }
  template <typename ...Args>
  std::recursive_mutex make_recursive_mutex(Args&& ...args) {
    return {};
  }
  template <typename ...Args>
  std::shared_mutex make_shared_mutex(Args&& ...args) {
    return {};
  }

  // debug methods.  Note that these can blindly return true
  // because any code that does anything other than assert these
  // are true is broken.
  #define ceph_mutex_is_locked(m) true
  #define ceph_mutex_is_not_locked(m) true
  #define ceph_mutex_is_rlocked(m) true
  #define ceph_mutex_is_wlocked(m) true
  #define ceph_mutex_is_locked_by_me(m) true
  #define ceph_mutex_is_not_locked_by_me(m) true

}

#endif	// CEPH_DEBUG_MUTEX

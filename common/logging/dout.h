// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2010 Sage Weil <sage@newdream.net>
 * Copyright (C) 2010 Dreamhost
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_DOUT_H
#define CEPH_DOUT_H

#include <type_traits>

#include "include/ceph_assert.h"
#include "common/global/global_context.h"
#include "common/ceph_context.h"
#include "common/config.h"
#include "common/likely.h"
#include "common/Clock.h"
#include "log/Log.h"

extern void dout_emergency(const char * const str);
extern void dout_emergency(const std::string &str);

// intentionally conflict with endl
class _bad_endl_use_dendl_t { public: _bad_endl_use_dendl_t(int) {} };
static const _bad_endl_use_dendl_t endl = 0;
inline std::ostream& operator<<(std::ostream& out, _bad_endl_use_dendl_t) {
  ceph_abort_msg("you are using the wrong endl.. use std::endl or dendl");
  return out;
}

class DoutPrefixProvider {
public:
  virtual std::ostream& gen_prefix(std::ostream& out) const = 0;
  virtual CephContext *get_cct() const = 0;
  virtual unsigned get_subsys() const = 0;
  virtual ~DoutPrefixProvider() {}
};

// a prefix provider with empty prefix
class NoDoutPrefix : public DoutPrefixProvider {
  CephContext *const cct;
  const unsigned subsys;
 public:
  NoDoutPrefix(CephContext *cct, unsigned subsys) : cct(cct), subsys(subsys) {}

  std::ostream& gen_prefix(std::ostream& out) const override { return out; }
  CephContext *get_cct() const override { return cct; }
  unsigned get_subsys() const override { return subsys; }
};

// a prefix provider with static (const char*) prefix
class DoutPrefix : public NoDoutPrefix {
  const char *const prefix;
 public:
  DoutPrefix(CephContext *cct, unsigned subsys, const char *prefix)
    : NoDoutPrefix(cct, subsys), prefix(prefix) {}

  std::ostream& gen_prefix(std::ostream& out) const override {
    return out << prefix;
  }
};

// a prefix provider that composes itself on top of another
class DoutPrefixPipe : public DoutPrefixProvider {
  const DoutPrefixProvider& dpp;
 public:
  DoutPrefixPipe(const DoutPrefixProvider& dpp) : dpp(dpp) {}

  std::ostream& gen_prefix(std::ostream& out) const override final {
    dpp.gen_prefix(out);
    add_prefix(out);
    return out;
  }
  CephContext *get_cct() const override { return dpp.get_cct(); }
  unsigned get_subsys() const override { return dpp.get_subsys(); }

  virtual void add_prefix(std::ostream& out) const = 0;
};

// helpers
namespace ceph::dout {

template<typename T>
struct dynamic_marker_t {
  T value;
  operator T() const { return value; }
};

template<typename T>
dynamic_marker_t<T> need_dynamic(T&& t) {
  return dynamic_marker_t<T>{ std::forward<T>(t) };
}

template<typename T>
struct is_dynamic : public std::false_type {};

template<typename T>
struct is_dynamic<dynamic_marker_t<T>> : public std::true_type {};

} // ceph::dout

// generic macros
#define dout_prefix *_dout

#define dout_impl(cct, sub, v)						\
  do {									\
  const bool should_gather = [&](const auto cctX) {			\
    if constexpr (ceph::dout::is_dynamic<decltype(sub)>::value ||	\
		  ceph::dout::is_dynamic<decltype(v)>::value) {		\
      return cctX->_conf->subsys.should_gather(sub, v);			\
    } else {								\
      /* The parentheses are **essential** because commas in angle	\
       * brackets are NOT ignored on macro expansion! A language's	\
       * limitation, sorry. */						\
      return (cctX->_conf->subsys.template should_gather<sub, v>());	\
    }									\
  }(cct);								\
									\
  if (should_gather) {							\
    ceph::logging::MutableEntry _dout_e(v, sub);                        \
    static_assert(std::is_convertible<decltype(&*cct), CephContext* >::value,		\
		  "provided cct must be compatible with CephContext*"); \
    auto _dout_cct = cct;						\
    std::ostream* _dout = &_dout_e.get_ostream();

#define dendl_impl std::flush;                                          \
    _dout_cct->_log->submit_entry(std::move(_dout_e));                  \
  }                                                                     \
  } while (0)

#define lsubdout(cct, sub, v)  dout_impl(cct, ceph_subsys_##sub, v) dout_prefix
#define ldout(cct, v)  dout_impl(cct, dout_subsys, v) dout_prefix
#define lderr(cct) dout_impl(cct, ceph_subsys_, -1) dout_prefix

#define ldpp_dout(dpp, v) 						\
  if (decltype(auto) pdpp = (dpp); pdpp) /* workaround -Wnonnull-compare for 'this' */ \
    dout_impl(pdpp->get_cct(), ceph::dout::need_dynamic(pdpp->get_subsys()), v) \
      pdpp->gen_prefix(*_dout)

#define lgeneric_subdout(cct, sub, v) dout_impl(cct, ceph_subsys_##sub, v) *_dout
#define lgeneric_dout(cct, v) dout_impl(cct, ceph_subsys_, v) *_dout
#define lgeneric_derr(cct) dout_impl(cct, ceph_subsys_, -1) *_dout

#define ldlog_p1(cct, sub, lvl) (cct->_conf->subsys.should_gather((sub), (lvl)))

#define dendl dendl_impl

#endif

// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2010-2011 Dreamhost
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "comon/utils/compat.h"
#include "common_init.h"
#include "common/utils/admin_socket.h"
#include "common/utils/ceph_argparse.h"
#include "ceph_context.h"
#include "config.h"
#include "common/logging/dout.h"
#include "common/strtol.h"
#include "common/zipkin_trace.h"

#define dout_subsys ceph_subsys_

#define _STR(x) #x
#define STRINGIFY(x) _STR(x)

CephContext *common_preinit(const CephInitParameters &iparams, enum code_environment_t code_env, int flags)
{
  g_code_env = code_env;

  // Create a configuration object
  CephContext *cct = new CephContext(iparams.module_type, code_env, flags);

  auto& conf = cct->_conf;
  // add config observers here

  // Set up our entity name.
  conf->name = iparams.name;


  if ((flags & CINIT_FLAG_UNPRIVILEGED_DAEMON_DEFAULTS)) {
    // make this unique despite multiple instances by the same name.
    conf.set_val_default("admin_socket", "$run_dir/$cluster-$name.$pid.$cctid.asok");
  }

  if (code_env == CODE_ENVIRONMENT_LIBRARY || code_env == CODE_ENVIRONMENT_UTILITY_NODOUT) {
    conf.set_val_default("log_to_stderr", "false");
    conf.set_val_default("err_to_stderr", "false");
    conf.set_val_default("log_flush_on_exit", "false");
  }

  return cct;
}

void complain_about_parse_errors(CephContext *cct, std::deque<std::string> *parse_errors)
{
  if (parse_errors->empty())
    return;
  lderr(cct) << "Errors while parsing config file!" << dendl;
  int cur_err = 0;
  static const int MAX_PARSE_ERRORS = 20;
  for (std::deque<std::string>::const_iterator p = parse_errors->begin(); p != parse_errors->end(); ++p)
  {
    lderr(cct) << *p << dendl;
    if (cur_err == MAX_PARSE_ERRORS) {
      lderr(cct) << "Suppressed " << (parse_errors->size() - MAX_PARSE_ERRORS) << " more errors." << dendl;
      break;
    }
    ++cur_err;
  }
}


/* Please be sure that this can safely be called multiple times by the
 * same application. */
void common_init_finish(CephContext *cct)
{
  // only do this once per cct
  if (cct->_finished) {
    return;
  }
  cct->_finished = true;
  cct->init_crypto();
  ZTracer::ztrace_init();

  if (!cct->_log->is_started()) {
    cct->_log->start();
  }

  int flags = cct->get_init_flags();
  if (!(flags & CINIT_FLAG_NO_DAEMON_ACTIONS))
    cct->start_service_thread();

  if ((flags & CINIT_FLAG_DEFER_DROP_PRIVILEGES) && (cct->get_set_uid() || cct->get_set_gid())) {
    cct->get_admin_socket()->chown(cct->get_set_uid(), cct->get_set_gid());
  }

  const auto& conf = cct->_conf;

  if (!conf->admin_socket.empty() && !conf->admin_socket_mode.empty()) {
    int ret = 0;
    std::string err;

    ret = strict_strtol(conf->admin_socket_mode.c_str(), 8, &err);
    if (err.empty()) {
      if (!(ret & (~ACCESSPERMS))) {
        cct->get_admin_socket()->chmod(static_cast<mode_t>(ret));
      } else {
        lderr(cct) << "Invalid octal permissions string: "
            << conf->admin_socket_mode << dendl;
      }
    } else {
      lderr(cct) << "Invalid octal string: " << err << dendl;
    }
  }
}

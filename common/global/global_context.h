// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_GLOBAL_CONTEXT_H
#define CEPH_GLOBAL_CONTEXT_H

#include <limits.h>
#include "config_fwd.h"

#include "ceph_context.h"
#include "global_context.h"
#include <string.h>

/*
 * Global variables for use from process context.
 */
CephContext *g_ceph_context = NULL;
ConfigProxy& g_conf() {
  return g_ceph_context->_conf;
}

#endif

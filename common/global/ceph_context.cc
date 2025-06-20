// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 * Copyright (C) 2017 OVH
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "ceph_context.h"
#include <mutex>
#include <iostream>
#include <pthread.h>
#include <boost/algorithm/string.hpp>
#include "include/mempool.h"
#include "common/admin_socket.h"
#include "common/code_environment.h"
#include "common/ceph_mutex.h"
#include "common/debug.h"
#include "config.h"
#include "common/HeartbeatMap.h"
#include "common/errno.h"
#include "common/Graylog.h"
#include "log/Log.h"
#include "auth/Crypto.h"
#include "include/str_list.h"
#include "common/config.h"
#include "common/config_obs.h"
#include "common/PluginRegistry.h"
#include "include/spinlock.h"
#include "mon/MonMap.h"

using ceph::bufferlist;
using ceph::HeartbeatMap;

// for CINIT_FLAGS
#include "common_init.h"
#include <iostream>
#include <pthread.h>

namespace {

class LockdepObs : public md_config_obs_t {
public:
  explicit LockdepObs(CephContext *cct)
    : m_cct(cct), m_registered(false), lock(ceph::make_mutex("lock_dep_obs")) {
  }
  ~LockdepObs() override {
    if (m_registered) {
      lockdep_unregister_ceph_context(m_cct);
    }
  }

  const char** get_tracked_conf_keys() const override {
    static const char *KEYS[] = {"lockdep", NULL};
    return KEYS;
  }

  void handle_conf_change(const ConfigProxy& conf,
                          const std::set <std::string> &changed) override {
    std::unique_lock locker(lock);
    if (conf->lockdep && !m_registered) {
      lockdep_register_ceph_context(m_cct);
      m_registered = true;
    } else if (!conf->lockdep && m_registered) {
      lockdep_unregister_ceph_context(m_cct);
      m_registered = false;
    }
  }
private:
  CephContext *m_cct;
  bool m_registered;
  ceph::mutex lock;
};

class MempoolObs : public md_config_obs_t, public AdminSocketHook {
  CephContext *cct;
  ceph::mutex lock;

public:
  explicit MempoolObs(CephContext *cct)
    : cct(cct), lock(ceph::make_mutex("mem_pool_obs")) {
    cct->_conf.add_observer(this);
    int r = cct->get_admin_socket()->register_command(
      "dump_mempools",
      "dump_mempools",
      this,
      "get mempool stats");
    ceph_assert(r == 0);
  }
  ~MempoolObs() override {
    cct->_conf.remove_observer(this);
    cct->get_admin_socket()->unregister_command("dump_mempools");
  }

  // md_config_obs_t
  const char** get_tracked_conf_keys() const override {
    static const char *KEYS[] = {
      "mempool_debug",
      NULL
    };
    return KEYS;
  }

  void handle_conf_change(const ConfigProxy& conf,
                          const std::set <std::string> &changed) override {
    std::unique_lock locker(lock);
    if (changed.count("mempool_debug")) {
      mempool::set_debug_mode(cct->_conf->mempool_debug);
    }
  }

  // AdminSocketHook
  bool call(std::string_view command, const cmdmap_t& cmdmap,
	    std::string_view format, bufferlist& out) override {
    if (command == "dump_mempools") {
      std::unique_ptr<Formatter> f(Formatter::create(format));
      f->open_object_section("mempools");
      mempool::dump(f.get());
      f->close_section();
      f->flush(out);
      return true;
    }
    return false;
  }
};

} // anonymous namespace

class CephContextServiceThread : public Thread
{
public:
  explicit CephContextServiceThread(CephContext *cct)
    : _reopen_logs(false), _exit_thread(false), _cct(cct) {}

  ~CephContextServiceThread() override {}

  void *entry() override
  {
    while (1) {
      std::unique_lock l(_lock);
      if (_exit_thread) {
        break;
      }

      if (_cct->_conf->heartbeat_interval) {
        auto interval = ceph::make_timespan(_cct->_conf->heartbeat_interval);
        _cond.wait_for(l, interval);
      } else
        _cond.wait(l);

      if (_exit_thread) {
        break;
      }

      if (_reopen_logs) {
        _cct->_log->reopen_log_file();
        _reopen_logs = false;
      }
      _cct->_heartbeat_map->check_touch_file();

      // refresh the perf coutners
      _cct->_refresh_perf_values();
    }
    return NULL;
  }

  void reopen_logs()
  {
    std::lock_guard l(_lock);
    _reopen_logs = true;
    _cond.notify_all();
  }

  void exit_thread()
  {
    std::lock_guard l(_lock);
    _exit_thread = true;
    _cond.notify_all();
  }

private:
  ceph::mutex _lock = ceph::make_mutex("CephContextServiceThread::_lock");
  ceph::condition_variable _cond;
  bool _reopen_logs;
  bool _exit_thread;
  CephContext *_cct;
};


/**
 * observe logging config changes
 *
 * The logging subsystem sits below most of the ceph code, including
 * the config subsystem, to keep it simple and self-contained.  Feed
 * logging-related config changes to the log.
 */
class LogObs : public md_config_obs_t {
  ceph::logging::Log *log;
  ceph::mutex lock;

public:
  explicit LogObs(ceph::logging::Log *l)
    : log(l), lock(ceph::make_mutex("log_obs")) {
  }

  const char** get_tracked_conf_keys() const override {
    static const char *KEYS[] = {
      "log_file",
      "log_max_new",
      "log_max_recent",
      "log_to_file",
      "log_to_syslog",
      "err_to_syslog",
      "log_stderr_prefix",
      "log_to_stderr",
      "err_to_stderr",
      "log_to_graylog",
      "err_to_graylog",
      "log_graylog_host",
      "log_graylog_port",
      "log_coarse_timestamps",
      "fsid",
      "host",
      NULL
    };
    return KEYS;
  }

  void handle_conf_change(const ConfigProxy& conf,
                          const std::set <std::string> &changed) override {
    std::unique_lock locker(lock);
    // stderr
    if (changed.count("log_to_stderr") || changed.count("err_to_stderr")) {
      int l = conf->log_to_stderr ? 99 : (conf->err_to_stderr ? -1 : -2);
      log->set_stderr_level(l, l);
    }

    // syslog
    if (changed.count("log_to_syslog")) {
      int l = conf->log_to_syslog ? 99 : (conf->err_to_syslog ? -1 : -2);
      log->set_syslog_level(l, l);
    }

    // file
    if (changed.count("log_file") || changed.count("log_to_file")) {
      if (conf->log_to_file) {
	log->set_log_file(conf->log_file);
      } else {
	log->set_log_file({});
      }
      log->reopen_log_file();
    }

    if (changed.count("log_stderr_prefix")) {
      log->set_log_stderr_prefix(conf.get_val<string>("log_stderr_prefix"));
    }

    if (changed.count("log_max_new")) {

      log->set_max_new(conf->log_max_new);
    }

    if (changed.count("log_max_recent")) {
      log->set_max_recent(conf->log_max_recent);
    }

    // graylog
    if (changed.count("log_to_graylog") || changed.count("err_to_graylog")) {
      int l = conf->log_to_graylog ? 99 : (conf->err_to_graylog ? -1 : -2);
      log->set_graylog_level(l, l);

      if (conf->log_to_graylog || conf->err_to_graylog) {
	      log->start_graylog();
      } else if (! (conf->log_to_graylog && conf->err_to_graylog)) {
	      log->stop_graylog();
      }
    }

    if (log->graylog() && (changed.count("log_graylog_host") || changed.count("log_graylog_port"))) {
      log->graylog()->set_destination(conf->log_graylog_host, conf->log_graylog_port);
    }

    if (changed.find("log_coarse_timestamps") != changed.end()) {
      log->set_coarse_timestamps(conf.get_val<bool>("log_coarse_timestamps"));
    }

    // metadata
    if (log->graylog() && changed.count("host")) {
      log->graylog()->set_hostname(conf->host);
    }

    if (log->graylog() && changed.count("fsid")) {
      log->graylog()->set_fsid(conf.get_val<uuid_d>("fsid"));
    }
  }
};

class CephContextHook : public AdminSocketHook {
  CephContext *m_cct;

public:
  explicit CephContextHook(CephContext *cct) : m_cct(cct) {}

  bool call(std::string_view command, const cmdmap_t& cmdmap,
	    std::string_view format, bufferlist& out) override {
    try {
      m_cct->do_command(command, cmdmap, format, &out);
    } catch (const bad_cmd_get& e) {
      return false;
    }
    return true;
  }
};

void CephContext::do_command(std::string_view command, const cmdmap_t& cmdmap,
			     std::string_view format, bufferlist *out)
{
  Formatter *f = Formatter::create(format, "json-pretty", "json-pretty");
  stringstream ss;
  for (auto it = cmdmap.begin(); it != cmdmap.end(); ++it) {
    if (it->first != "prefix") {
      ss << it->first  << ":" << cmd_vartype_stringify(it->second) << " ";
    }
  }
  lgeneric_dout(this, 1) << "do_command '" << command << "' '"
			 << ss.str() << dendl;
  ceph_assert_always(!(command == "assert" && _conf->debug_asok_assert_abort));
  if (command == "abort" && _conf->debug_asok_assert_abort) {
   ceph_abort();
  }
  if (command == "perfcounters_dump" || command == "1" ||
      command == "perf dump") {
    std::string logger;
    std::string counter;
    cmd_getval(this, cmdmap, "logger", logger);
    cmd_getval(this, cmdmap, "counter", counter);
    _perf_counters_collection->dump_formatted(f, false, logger, counter);
  }
  else if (command == "perfcounters_schema" || command == "2" ||
    command == "perf schema") {
    _perf_counters_collection->dump_formatted(f, true);
  }
  else if (command == "perf histogram dump") {
    std::string logger;
    std::string counter;
    cmd_getval(this, cmdmap, "logger", logger);
    cmd_getval(this, cmdmap, "counter", counter);
    _perf_counters_collection->dump_formatted_histograms(f, false, logger,
                                                         counter);
  }
  else if (command == "perf histogram schema") {
    _perf_counters_collection->dump_formatted_histograms(f, true);
  }
  else if (command == "perf reset") {
    std::string var;
    std::string section(command);
    f->open_object_section(section.c_str());
    if (!cmd_getval(this, cmdmap, "var", var)) {
      f->dump_string("error", "syntax error: 'perf reset <var>'");
    } else {
     if(!_perf_counters_collection->reset(var))
        f->dump_stream("error") << "Not find: " << var;
     else
       f->dump_string("success", std::string(command) + ' ' + var);
    }
    f->close_section();
  }
  else {
    std::string section(command);
    boost::replace_all(section, " ", "_");
    f->open_object_section(section.c_str());
    if (command == "config show") {
      _conf.show_config(f);
    }
    else if (command == "config unset") {
      std::string var;
      if (!(cmd_getval(this, cmdmap, "var", var))) {
        f->dump_string("error", "syntax error: 'config unset <var>'");
      } else {
        int r = _conf.rm_val(var.c_str());
        if (r < 0 && r != -ENOENT) {
          f->dump_stream("error") << "error unsetting '" << var << "': "
				  << cpp_strerror(r);
        } else {
          ostringstream ss;
          _conf.apply_changes(&ss);
          f->dump_string("success", ss.str());
        }
      }

    }
    else if (command == "config set") {
      std::string var;
      std::vector<std::string> val;

      if (!(cmd_getval(this, cmdmap, "var", var)) ||
          !(cmd_getval(this, cmdmap, "val", val))) {
        f->dump_string("error", "syntax error: 'config set <var> <value>'");
      } else {
	// val may be multiple words
	string valstr = str_join(val, " ");
        int r = _conf.set_val(var.c_str(), valstr.c_str());
        if (r < 0) {
          f->dump_stream("error") << "error setting '" << var << "' to '" << valstr << "': " << cpp_strerror(r);
        } else {
          ostringstream ss;
          _conf.apply_changes(&ss);
          f->dump_string("success", ss.str());
        }
      }
    } else if (command == "config get") {
      std::string var;
      if (!cmd_getval(this, cmdmap, "var", var)) {
	f->dump_string("error", "syntax error: 'config get <var>'");
      } else {
	char buf[4096];
	// FIPS zeroization audit 20191115: this memset is not security related.
	memset(buf, 0, sizeof(buf));
	char *tmp = buf;
	int r = _conf.get_val(var.c_str(), &tmp, sizeof(buf));
	if (r < 0) {
	    f->dump_stream("error") << "error getting '" << var << "': " << cpp_strerror(r);
	} else {
	    f->dump_string(var.c_str(), buf);
	}
      }
    } else if (command == "config help") {
      std::string var;
      if (cmd_getval(this, cmdmap, "var", var)) {
        // Output a single one
        std::string key = ConfFile::normalize_key_name(var);
	auto schema = _conf.get_schema(key);
        if (!schema) {
          std::ostringstream msg;
          msg << "Setting not found: '" << key << "'";
          f->dump_string("error", msg.str());
        } else {
          f->dump_object("option", *schema);
        }
      } else {
        // Output all
        f->open_array_section("options");
        for (const auto &option : ceph_options) {
          f->dump_object("option", option);
        }
        f->close_section();
      }
    } else if (command == "config diff") {
      f->open_object_section("diff");
      _conf.diff(f);
      f->close_section(); // unknown
    } else if (command == "config diff get") {
      std::string setting;
      f->open_object_section("diff");
      _conf.diff(f, setting);
      f->close_section(); // unknown
    } else if (command == "log flush") {
      _log->flush();
    }
    else if (command == "log dump") {
      _log->dump_recent();
    }
    else if (command == "log reopen") {
      _log->reopen_log_file();
    }
    else {
      ceph_abort_msg("registered under wrong command?");    
    }
    f->close_section();
  }
  f->flush(*out);
  delete f;
  lgeneric_dout(this, 1) << "do_command '" << command << "' '" << ss.str()
		         << "result is " << out->length() << " bytes" << dendl;
}

CephContext::CephContext(uint32_t module_type_,
                         enum code_environment_t code_env,
                         int init_flags_)
  : nref(1),
    _conf{code_env == CODE_ENVIRONMENT_DAEMON},
    _log(NULL),
    _module_type(module_type_),
    _init_flags(init_flags_),
    _set_uid(0),
    _set_gid(0),
    _set_uid_string(),
    _set_gid_string(),
    _service_thread(NULL),
    _log_obs(NULL),
    _admin_socket(NULL),
    _perf_counters_conf_obs(NULL),
    _lockdep_obs(NULL)
{
  _log = new ceph::logging::Log(&_conf->subsys);

  _log_obs = new LogObs(_log);
  _conf.add_observer(_log_obs);

  _cct_obs = new CephContextObs(this);
  _conf.add_observer(_cct_obs);

  _lockdep_obs = new LockdepObs(this);
  _conf.add_observer(_lockdep_obs);

  _admin_socket = new AdminSocket(this);
  _heartbeat_map = new HeartbeatMap(this);

  _plugin_registry = new PluginRegistry(this);

  _admin_hook = new CephContextHook(this);
  _admin_socket->register_command("assert", "assert", _admin_hook, "");
  _admin_socket->register_command("abort", "abort", _admin_hook, "");
  _admin_socket->register_command("perfcounters_dump", "perfcounters_dump", _admin_hook, "");
  _admin_socket->register_command("1", "1", _admin_hook, "");
  _admin_socket->register_command("perf dump", "perf dump name=logger,type=CephString,req=false name=counter,type=CephString,req=false", _admin_hook, "dump perfcounters value");
  _admin_socket->register_command("perfcounters_schema", "perfcounters_schema", _admin_hook, "");
  _admin_socket->register_command("perf histogram dump", "perf histogram dump name=logger,type=CephString,req=false name=counter,type=CephString,req=false", _admin_hook, "dump perf histogram values");
  _admin_socket->register_command("2", "2", _admin_hook, "");
  _admin_socket->register_command("perf schema", "perf schema", _admin_hook, "dump perfcounters schema");
  _admin_socket->register_command("perf histogram schema", "perf histogram schema", _admin_hook, "dump perf histogram schema");
  _admin_socket->register_command("perf reset", "perf reset name=var,type=CephString", _admin_hook, "perf reset <name>: perf reset all or one perfcounter name");
  _admin_socket->register_command("config show", "config show", _admin_hook, "dump current config settings");
  _admin_socket->register_command("config help", "config help name=var,type=CephString,req=false", _admin_hook, "get config setting schema and descriptions");
  _admin_socket->register_command("config set", "config set name=var,type=CephString name=val,type=CephString,n=N",  _admin_hook, "config set <field> <val> [<val> ...]: set a config variable");
  _admin_socket->register_command("config unset", "config unset name=var,type=CephString",  _admin_hook, "config unset <field>: unset a config variable");
  _admin_socket->register_command("config get", "config get name=var,type=CephString", _admin_hook, "config get <field>: get the config value");
  _admin_socket->register_command("config diff", "config diff", _admin_hook, "dump diff of current config and default config");
  _admin_socket->register_command("config diff get", "config diff get name=var,type=CephString", _admin_hook, "dump diff get <field>: dump diff of current and default config setting <field>");
  _admin_socket->register_command("log flush", "log flush", _admin_hook, "flush log entries to log file");
  _admin_socket->register_command("log dump", "log dump", _admin_hook, "dump recent log entries to log file");
  _admin_socket->register_command("log reopen", "log reopen", _admin_hook, "reopen log file");

  lookup_or_create_singleton_object<MempoolObs>("mempool_obs", false, this);
}

CephContext::~CephContext()
{
  associated_objs.clear();
  join_service_thread();

  if (_cct_perf) {
    _perf_counters_collection->remove(_cct_perf);
    delete _cct_perf;
    _cct_perf = NULL;
  }

  delete _plugin_registry;

  _admin_socket->unregister_commands(_admin_hook);
  delete _admin_hook;
  delete _admin_socket;

  delete _heartbeat_map;

  delete _perf_counters_collection;
  _perf_counters_collection = NULL;

  delete _perf_counters_conf_obs;
  _perf_counters_conf_obs = NULL;

  _conf.remove_observer(_log_obs);
  delete _log_obs;
  _log_obs = NULL;

  _conf.remove_observer(_cct_obs);
  delete _cct_obs;
  _cct_obs = NULL;

  _conf.remove_observer(_lockdep_obs);
  delete _lockdep_obs;
  _lockdep_obs = NULL;

  _log->stop();
  delete _log;
  _log = NULL;

  delete _crypto_none;
  delete _crypto_aes;
  if (_crypto_inited > 0) {
    ceph_assert(_crypto_inited == 1);  // or else someone explicitly did
				  // init but not shutdown
    shutdown_crypto();
  }
}

void CephContext::put() {
  if (--nref == 0) {
    ANNOTATE_HAPPENS_AFTER(&nref);
    ANNOTATE_HAPPENS_BEFORE_FORGET_ALL(&nref);
    delete this;
  } else {
    ANNOTATE_HAPPENS_BEFORE(&nref);
  }
}

void CephContext::start_service_thread()
{
  {
    std::lock_guard lg(_service_thread_lock);
    if (_service_thread) {
      return;
    }
    _service_thread = new CephContextServiceThread(this);
    _service_thread->create("service");
  }

  if (!(get_init_flags() & CINIT_FLAG_NO_CCT_PERF_COUNTERS))
    _enable_perf_counter();

  // make logs flush on_exit()
  if (_conf->log_flush_on_exit)
    _log->set_flush_on_exit();

  // Trigger callbacks on any config observers that were waiting for
  // it to become safe to start threads.
  _conf.set_safe_to_start_threads();
  _conf.call_all_observers();

  // start admin socket
  if (_conf->admin_socket.length())
    _admin_socket->init(_conf->admin_socket);
}

void CephContext::reopen_logs()
{
  std::lock_guard lg(_service_thread_lock);
  if (_service_thread)
    _service_thread->reopen_logs();
}

void CephContext::join_service_thread()
{
  std::unique_lock<ceph::spinlock> lg(_service_thread_lock);

  CephContextServiceThread *thread = _service_thread;
  if (!thread) {
    return;
  }
  _service_thread = NULL;

  lg.unlock();

  thread->exit_thread();
  thread->join();
  delete thread;

  if (!(get_init_flags() & CINIT_FLAG_NO_CCT_PERF_COUNTERS))
    _disable_perf_counter();
}

uint32_t CephContext::get_module_type() const
{
  return _module_type;
}

void CephContext::set_init_flags(int flags)
{
  _init_flags = flags;
}

int CephContext::get_init_flags() const
{
  return _init_flags;
}

void CephContext::_refresh_perf_values()
{
  if (_cct_perf) {
    _cct_perf->set(l_cct_total_workers, _heartbeat_map->get_total_workers());
    _cct_perf->set(l_cct_unhealthy_workers, _heartbeat_map->get_unhealthy_workers());
  }
  unsigned l = l_mempool_first + 1;
  for (unsigned i = 0; i < mempool::num_pools; ++i) {
    mempool::pool_t& p = mempool::get_pool(mempool::pool_index_t(i));
    _mempool_perf->set(l++, p.allocated_bytes());
    _mempool_perf->set(l++, p.allocated_items());
  }
}

AdminSocket *CephContext::get_admin_socket()
{
  return _admin_socket;
}


void CephContext::notify_pre_fork()
{
  {
    std::lock_guard lg(_fork_watchers_lock);
    for (auto &&t : _fork_watchers) {
      t->handle_pre_fork();
    }
  }
  {
    // note: we don't hold a lock here, but we assume we are idle at
    // fork time, which happens during process init and startup.
    auto i = associated_objs.begin();
    while (i != associated_objs.end()) {
      if (associated_objs_drop_on_fork.count(i->first.first)) {
	i = associated_objs.erase(i);
      } else {
	++i;
      }
    }
    associated_objs_drop_on_fork.clear();
  }
}

void CephContext::notify_post_fork()
{
  ceph::spin_unlock(&_fork_watchers_lock);
  for (auto &&t : _fork_watchers)
    t->handle_post_fork();
}

void CephContext::set_mon_addrs(const MonMap& mm) {
  std::vector<entity_addrvec_t> mon_addrs;
  for (auto& i : mm.mon_info) {
    mon_addrs.push_back(i.second.public_addrs);
  }

  set_mon_addrs(mon_addrs);
}

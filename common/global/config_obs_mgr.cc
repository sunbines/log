// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-

#pragma once

#include "config_obs_mgr.h"

// we could put the implementations in a .cc file, and only instantiate the
// used template specializations explicitly, but that forces us to involve
// unused headers and libraries at compile-time. for instance, for instantiate,
// to instantiate ObserverMgr for seastar, we will need to include seastar
// headers to get the necessary types in place, but that would force us to link
// the non-seastar binaries against seastar libraries. so, to avoid pulling
// in unused dependencies at the expense of increasing compiling time, we put
// the implementation in the header file.
template<class ConfigObs>
void ObserverMgr<ConfigObs>::add_observer(ConfigObs* observer)
{
  const char **keys = observer->get_tracked_conf_keys();
  for (const char ** k = keys; *k; ++k) {
    observers.emplace(*k, observer);
  }
}

template<class ConfigObs>
void ObserverMgr<ConfigObs>::remove_observer(ConfigObs* observer)
{
  [[maybe_unused]] bool found_obs = false;
  for (auto o = observers.begin(); o != observers.end(); ) {
    if (o->second == observer) {
      observers.erase(o++);
      found_obs = true;
    } else {
      ++o;
    }
  }
  ceph_assert(found_obs);
}

template<class ConfigObs>
void ObserverMgr<ConfigObs>::for_each_observer(config_gather_cb callback)
{
  for (const auto& [key, obs] : observers) {
    callback(obs, key);
  }
}

template<class ConfigObs>
template<class ConfigProxyT>
void ObserverMgr<ConfigObs>::for_each_change(const std::set<std::string>& changes,
               ConfigProxyT& proxy, config_gather_cb callback, std::ostream *oss)
{
  // create the reverse observer mapping, mapping observers to the set of
  // changed keys that they'll get.
  string val;
  for (auto& key : changes) {
    auto [first, last] = observers.equal_range(key);
    if ((oss) && !proxy.get_val(key, &val)) {
      (*oss) << key << " = '" << val << "' ";
      if (first == last) {
        (*oss) << "(not observed, change may require restart) ";
      }
    }
    for (auto r = first; r != last; ++r) {
      callback(r->second, key);
    }
  }
}

template<class ConfigObs>
bool ObserverMgr<ConfigObs>::is_tracking(const std::string& name) const
{
  return observers.count(name) > 0;
}

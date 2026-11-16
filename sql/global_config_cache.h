#ifndef GLOBAL_CONFIG_CACHE_INCLUDED
#define GLOBAL_CONFIG_CACHE_INCLUDED

/* Copyright (c) 2026, Multi-MySQL contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "mysql/psi/mysql_rwlock.h"

class THD;
class Json_wrapper;
struct TABLE;
struct TABLE_LIST;
class Table_ref;

/**
  In-memory cache of mysql.global_config: shard_id -> owning region_id.
*/
class Global_config_cache {
 public:
  static Global_config_cache &instance();

  /** Load (or reload) mapping from mysql.global_config. */
  bool reload(THD *thd);

  /** Return owning region for shard, or nullopt if unmapped. */
  std::optional<int64_t> owner_region(int64_t shard) const;

  /**
    True if shard is writable on this node (owner region == opt_region).
    Unmapped shards are not writable when region checks are enabled.
  */
  bool is_shard_writable(int64_t shard) const;

  /**
    Validate shards JSON is an array of integers.
    On success, fills @p out_shards.
  */
  static bool parse_shards_json(const Json_wrapper &wr,
                                std::vector<int64_t> *out_shards);

  /**
    Check that none of @p shards conflict with existing mapping for a
    different region than @p region. If @p replacing_region is set, that
    region's current shards are ignored (UPDATE replace semantics).
  */
  bool check_no_conflict(int64_t region, const std::vector<int64_t> &shards,
                         bool replacing_region) const;

  bool empty() const;

 private:
  Global_config_cache() = default;
  mutable mysql_rwlock_t m_lock;
  std::unordered_map<int64_t, int64_t> m_shard_to_region;
  bool m_initialized{false};

  void init_lock();
};

bool global_config_cache_init(THD *thd);
bool global_config_cache_reload(THD *thd);
void global_config_cache_free();

/** True if table is mysql.global_config. */
bool is_global_config_table(const char *db, const char *table_name);
bool is_global_config_table(const TABLE *table);
bool is_global_config_table(const Table_ref *tl);

/**
  Validate a pending write to mysql.global_config.
  @param region  region column value
  @param shards_field JSON Field for shards (may be nullptr on DELETE)
  @param is_delete true for DELETE
  @param is_update true for UPDATE (replace that region's shards)
*/
bool validate_global_config_write(THD *thd, int64_t region, Field *shards_field,
                                  bool is_delete, bool is_update);

/** Mark THD so cache reloads after successful commit. */
void mark_global_config_changed(THD *thd);

/** Called after successful transaction commit. */
void global_config_cache_on_commit(THD *thd);

#endif /* GLOBAL_CONFIG_CACHE_INCLUDED */

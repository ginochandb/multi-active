/* Copyright (c) 2026, Multi-MySQL contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "sql/global_config_cache.h"

#include <string.h>

#include "my_dbug.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "sql-common/json_dom.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/iterators/row_iterator.h"
#include "sql/mysqld.h"
#include "sql/sql_base.h"  // open_trans_system_tables_for_read
#include "sql/sql_class.h"
#include "sql/sql_executor.h"  // init_table_iterator
#include "sql/table.h"
#include "sql/thr_malloc.h"

namespace {
constexpr const char *k_db = "mysql";
constexpr const char *k_table = "global_config";
constexpr uint k_field_region = 0;
constexpr uint k_field_shards = 1;
}  // namespace

Global_config_cache &Global_config_cache::instance() {
  static Global_config_cache cache;
  return cache;
}

void Global_config_cache::init_lock() {
  if (!m_initialized) {
    mysql_rwlock_init(0, &m_lock);
    m_initialized = true;
  }
}

bool Global_config_cache::empty() const {
  mysql_rwlock_rdlock(&m_lock);
  bool e = m_shard_to_region.empty();
  mysql_rwlock_unlock(&m_lock);
  return e;
}

std::optional<int64_t> Global_config_cache::owner_region(int64_t shard) const {
  mysql_rwlock_rdlock(&m_lock);
  auto it = m_shard_to_region.find(shard);
  std::optional<int64_t> res;
  if (it != m_shard_to_region.end()) res = it->second;
  mysql_rwlock_unlock(&m_lock);
  return res;
}

bool Global_config_cache::is_shard_writable(int64_t shard) const {
  if (opt_region == 0) return true;
  auto owner = owner_region(shard);
  return owner.has_value() &&
         static_cast<ulong>(owner.value()) == opt_region;
}

bool Global_config_cache::parse_shards_json(const Json_wrapper &wr,
                                           std::vector<int64_t> *out_shards) {
  out_shards->clear();
  if (wr.type() != enum_json_type::J_ARRAY) return true;
  const size_t n = wr.length();
  for (size_t i = 0; i < n; ++i) {
    Json_wrapper elem = wr[i];
    if (elem.type() == enum_json_type::J_INT) {
      out_shards->push_back(elem.get_int());
    } else if (elem.type() == enum_json_type::J_UINT) {
      ulonglong v = elem.get_uint();
      if (v > static_cast<ulonglong>(INT64_MAX)) return true;
      out_shards->push_back(static_cast<int64_t>(v));
    } else {
      return true;
    }
  }
  return false;
}

bool Global_config_cache::check_no_conflict(
    int64_t region, const std::vector<int64_t> &shards,
    bool replacing_region) const {
  mysql_rwlock_rdlock(&m_lock);
  for (int64_t shard : shards) {
    auto it = m_shard_to_region.find(shard);
    if (it == m_shard_to_region.end()) continue;
    if (replacing_region && it->second == region) continue;
    if (it->second != region) {
      mysql_rwlock_unlock(&m_lock);
      my_error(ER_GLOBAL_CONFIG_SHARD_CONFLICT, MYF(0),
               static_cast<longlong>(shard),
               static_cast<longlong>(it->second));
      return true;
    }
  }
  mysql_rwlock_unlock(&m_lock);
  return false;
}

bool Global_config_cache::reload(THD *thd) {
  DBUG_TRACE;
  init_lock();

  bool own_thd = false;
  if (thd == nullptr) {
    thd = new (std::nothrow) THD;
    if (thd == nullptr) return true;
    thd->thread_stack = reinterpret_cast<char *>(&thd);
    thd->store_globals();
    own_thd = true;
  }

  mysql_rwlock_wrlock(&m_lock);

  std::unordered_map<int64_t, int64_t> new_map;
  bool error = false;

  Table_ref tables(k_db, k_table, TL_READ);
  if (open_trans_system_tables_for_read(thd, &tables)) {
    // During bootstrap the table may not exist yet; treat as empty.
    if (!thd->get_stmt_da()->is_error() || opt_initialize) {
      m_shard_to_region.clear();
      error = false;
    } else {
      error = true;
    }
    goto end;
  }

  {
    unique_ptr_destroy_only<RowIterator> iterator =
        init_table_iterator(thd, tables.table, /*ignore_not_found_rows=*/false,
                            /*count_examined_rows=*/false);
    if (iterator == nullptr) {
      error = true;
      close_trans_system_tables(thd);
      goto end;
    }

    while (!(iterator->Read())) {
      TABLE *table = tables.table;
      Field *region_field = table->field[k_field_region];
      Field *shards_field = table->field[k_field_shards];
      if (region_field->is_null() || shards_field->is_null()) continue;

      int64_t region = region_field->val_int();
      Json_wrapper wr;
      if (down_cast<Field_json *>(shards_field)->val_json(&wr)) {
        error = true;
        break;
      }
      std::vector<int64_t> shards;
      if (parse_shards_json(wr, &shards)) {
        // Skip malformed rows rather than failing startup hard.
        continue;
      }
      for (int64_t shard : shards) {
        new_map[shard] = region;
      }
    }
  }

  close_trans_system_tables(thd);

  if (!error) m_shard_to_region.swap(new_map);

end:
  mysql_rwlock_unlock(&m_lock);
  if (own_thd) delete thd;
  return error;
}

bool global_config_cache_init(THD *thd) {
  return Global_config_cache::instance().reload(thd);
}

bool global_config_cache_reload(THD *thd) {
  return Global_config_cache::instance().reload(thd);
}

void global_config_cache_free() {
  /* Lock and map are process-lifetime; no teardown required. */
}

bool is_global_config_table(const char *db, const char *table_name) {
  return db != nullptr && table_name != nullptr &&
         my_strcasecmp(system_charset_info, db, k_db) == 0 &&
         my_strcasecmp(system_charset_info, table_name, k_table) == 0;
}

bool is_global_config_table(const TABLE *table) {
  if (table == nullptr || table->s == nullptr) return false;
  return is_global_config_table(table->s->db.str, table->s->table_name.str);
}

bool is_global_config_table(const Table_ref *tl) {
  if (tl == nullptr) return false;
  return is_global_config_table(tl->db, tl->table_name);
}

bool validate_global_config_write(THD *thd, int64_t region, Field *shards_field,
                                  bool is_delete, bool is_update) {
  if (is_delete) return false;
  if (shards_field == nullptr || shards_field->is_null()) {
    my_error(ER_GLOBAL_CONFIG_INVALID_SHARDS, MYF(0));
    return true;
  }
  Json_wrapper wr;
  if (down_cast<Field_json *>(shards_field)->val_json(&wr)) return true;
  std::vector<int64_t> shards;
  if (Global_config_cache::parse_shards_json(wr, &shards)) {
    my_error(ER_GLOBAL_CONFIG_INVALID_SHARDS, MYF(0));
    return true;
  }
  return Global_config_cache::instance().check_no_conflict(region, shards,
                                                           is_update);
}

void mark_global_config_changed(THD *thd) {
  if (thd != nullptr) thd->m_global_config_changed = true;
}

void global_config_cache_on_commit(THD *thd) {
  if (thd == nullptr || !thd->m_global_config_changed) return;
  thd->m_global_config_changed = false;
  (void)global_config_cache_reload(thd);
}

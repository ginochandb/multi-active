#ifndef SQL_REGION_SHARD_INCLUDED
#define SQL_REGION_SHARD_INCLUDED

/* Copyright (c) 2026, Multi-MySQL contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include <cstdint>

#include "mem_root_deque.h"
#include "sql/sql_list.h"

class THD;
class Item;
class Field;
struct TABLE;
class Table_ref;
struct HA_CREATE_INFO;
class Create_field;

/** True when region/shard DML checks are active for this thread. */
bool region_shard_checks_enabled(const THD *thd);

/** Resolve SHARD KEY column index on TABLE_SHARE / TABLE. */
bool resolve_shard_key_field(TABLE *table);

/**
  Validate CREATE/ALTER TABLE shard key option when region != 0.
  @retval true error
*/
bool check_shard_key_on_create(THD *thd, const char *db, const char *table_name,
                               HA_CREATE_INFO *create_info,
                               List<Create_field> *fields, bool is_tmp_table);

/**
  INSERT: require explicit shard column and writable shard value.
*/
bool check_insert_shard(THD *thd, TABLE *table,
                        const mem_root_deque<Item *> *fields,
                        const mem_root_deque<Item *> *values);

/**
  Check a concrete shard value is writable on this node.
*/
bool check_shard_value_writable(THD *thd, TABLE *table, int64_t shard_val);

/**
  UPDATE: forbid updating shard column; require PK or shard equality in WHERE.
*/
bool check_update_shard_prepare(THD *thd, TABLE *table,
                                const mem_root_deque<Item *> *fields,
                                Item *conds);

/**
  DELETE: require PK or shard equality in WHERE.
*/
bool check_delete_shard_prepare(THD *thd, TABLE *table, Item *conds);

/**
  Per-row check before update/delete/insert write using current record's shard.
*/
bool check_row_shard_writable(THD *thd, TABLE *table);

/**
  Validate and mark mysql.global_config write from current table->record[0].
*/
bool check_global_config_row_write(THD *thd, TABLE *table, bool is_update,
                                   bool is_delete);

#endif /* SQL_REGION_SHARD_INCLUDED */

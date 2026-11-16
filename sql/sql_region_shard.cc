/* Copyright (c) 2026, Multi-MySQL contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "sql/sql_region_shard.h"

#include "mem_root_deque.h"
#include "my_dbug.h"
#include "mysqld_error.h"
#include "sql/create_field.h"
#include "sql/field.h"
#include "sql/global_config_cache.h"
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/mysqld.h"
#include "sql/sql_class.h"
#include "sql/sql_list.h"
#include "sql/sql_select.h"
#include "sql/table.h"

namespace {

bool is_integer_field_type(enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
      return true;
    default:
      return false;
  }
}

Field *shard_field(TABLE *table) {
  if (table == nullptr || table->s == nullptr) return nullptr;
  if (table->s->shard_key.str == nullptr) return nullptr;
  if (table->s->shard_field_index == UINT_MAX) return nullptr;
  return table->field[table->s->shard_field_index];
}

bool has_full_pk_equality(TABLE *table, Item *conds) {
  if (table->s->primary_key == MAX_KEY) return false;
  KEY *key = table->key_info + table->s->primary_key;
  for (uint i = 0; i < key->user_defined_key_parts; ++i) {
    Field *f = key->key_part[i].field;
    if (!check_field_is_const(conds, nullptr, f)) return false;
  }
  return true;
}

bool has_shard_equality(TABLE *table, Item *conds, Item **const_item) {
  Field *sf = shard_field(table);
  if (sf == nullptr || conds == nullptr) return false;
  return check_field_is_const(conds, nullptr, sf, const_item);
}

bool table_needs_shard_key(const THD *thd, const TABLE *table) {
  if (!region_shard_checks_enabled(thd)) return false;
  if (is_shard_key_exempt_table(table)) return false;
  return true;
}

}  // namespace

bool region_shard_checks_enabled(const THD *thd) {
  return opt_region != 0 && thd != nullptr && !thd->slave_thread;
}

bool is_shard_key_exempt_db(const char *db) {
  if (db == nullptr) return false;
  if (my_strcasecmp(system_charset_info, db, MYSQL_SCHEMA_NAME.str) == 0)
    return true;
  if (my_strcasecmp(system_charset_info, db, "sys") == 0) return true;
  if (is_perfschema_db(db)) return true;
  if (is_infoschema_db(db)) return true;
  return false;
}

bool is_shard_key_exempt_table(const TABLE *table) {
  if (table == nullptr || table->s == nullptr) return true;
  if (table->s->tmp_table != NO_TMP_TABLE) return true;
  if (table->s->table_category == TABLE_CATEGORY_TEMPORARY) return true;
  // Non-USER categories: SYSTEM/ACL/LOG/RPL/GTID/DICTIONARY/PERFORMANCE/INFORMATION
  if (table->s->table_category != TABLE_CATEGORY_USER) return true;
  if (is_shard_key_exempt_db(table->s->db.str)) return true;
  return false;
}

bool resolve_shard_key_field(TABLE *table) {
  if (table == nullptr || table->s == nullptr) return false;
  table->s->shard_field_index = UINT_MAX;
  if (table->s->shard_key.str == nullptr || table->s->shard_key.length == 0)
    return false;
  for (Field **f = table->field; *f != nullptr; ++f) {
    if (my_strcasecmp(system_charset_info, (*f)->field_name,
                      table->s->shard_key.str) == 0) {
      table->s->shard_field_index = (*f)->field_index();
      return false;
    }
  }
  return true;  // column missing
}

bool check_shard_key_on_create(THD *thd [[maybe_unused]], const char *db,
                               const char *table_name,
                               HA_CREATE_INFO *create_info,
                               List<Create_field> *fields, bool is_tmp_table) {
  if (is_tmp_table) return false;

  // Explicit SHARD KEY in this statement: validate column name/type
  // (including on system schemas).
  if (create_info->used_fields & HA_CREATE_USED_SHARD_KEY) {
    if (create_info->shard_key.str == nullptr) {
      my_error(ER_SHARD_KEY_INVALID, MYF(0), "");
      return true;
    }
    Create_field *found = nullptr;
    List_iterator_fast<Create_field> it(*fields);
    Create_field *cf;
    while ((cf = it++)) {
      if (my_strcasecmp(system_charset_info, cf->field_name,
                        create_info->shard_key.str) == 0) {
        found = cf;
        break;
      }
    }
    if (found == nullptr || !is_integer_field_type(found->sql_type)) {
      my_error(ER_SHARD_KEY_INVALID, MYF(0), create_info->shard_key.str);
      return true;
    }
    return false;
  }

  // System schemas skip the mandatory SHARD KEY requirement.
  if (is_shard_key_exempt_db(db)) return false;

  // No SHARD KEY on CREATE when region enabled.
  if (opt_region != 0 && create_info->shard_key.str == nullptr) {
    my_error(ER_SHARD_KEY_REQUIRED, MYF(0), db ? db : "",
             table_name ? table_name : "");
    return true;
  }
  return false;
}

bool check_shard_value_writable(THD *thd, TABLE *table, int64_t shard_val) {
  if (!region_shard_checks_enabled(thd)) return false;
  if (!table_needs_shard_key(thd, table)) return false;
  if (table->s->shard_key.str == nullptr) {
    my_error(ER_SHARD_KEY_REQUIRED, MYF(0), table->s->db.str,
             table->s->table_name.str);
    return true;
  }
  auto owner = Global_config_cache::instance().owner_region(shard_val);
  if (!owner.has_value() ||
      static_cast<ulong>(owner.value()) != opt_region) {
    my_error(ER_REGION_SHARD_READ_ONLY, MYF(0),
             static_cast<longlong>(shard_val),
             static_cast<ulonglong>(opt_region),
             static_cast<longlong>(owner.value_or(-1)));
    return true;
  }
  return false;
}

bool check_insert_shard(THD *thd, TABLE *table,
                        const mem_root_deque<Item *> *fields,
                        const mem_root_deque<Item *> *values) {
  if (!region_shard_checks_enabled(thd)) return false;
  if (is_global_config_table(table)) return false;
  if (!table_needs_shard_key(thd, table)) return false;

  if (table->s->shard_key.str == nullptr) {
    my_error(ER_SHARD_KEY_REQUIRED, MYF(0), table->s->db.str,
             table->s->table_name.str);
    return true;
  }

  Field *sf = shard_field(table);
  if (sf == nullptr) {
    my_error(ER_SHARD_KEY_INVALID, MYF(0), table->s->shard_key.str);
    return true;
  }

  // Require shard column present in INSERT column list (or implicit all cols).
  bool found = false;
  int field_pos = -1;
  if (fields == nullptr || fields->empty()) {
    found = true;
    field_pos = static_cast<int>(sf->field_index());
  } else {
    int idx = 0;
    for (Item *item : *fields) {
      Item_field *fitem = item->field_for_view_update();
      if (fitem != nullptr && fitem->field == sf) {
        found = true;
        field_pos = idx;
        break;
      }
      ++idx;
    }
  }
  if (!found) {
    my_error(ER_INSERT_SHARD_REQUIRED, MYF(0), table->s->shard_key.str);
    return true;
  }

  if (values != nullptr && field_pos >= 0 &&
      static_cast<size_t>(field_pos) < values->size()) {
    Item *v = (*values)[field_pos];
    if (v == nullptr || !v->const_for_execution()) {
      my_error(ER_INSERT_SHARD_REQUIRED, MYF(0), table->s->shard_key.str);
      return true;
    }
    if (v->is_null()) {
      my_error(ER_INSERT_SHARD_REQUIRED, MYF(0), table->s->shard_key.str);
      return true;
    }
    int64_t shard_val = v->val_int();
    if (thd->is_error()) return true;
    return check_shard_value_writable(thd, table, shard_val);
  }
  return false;
}

bool check_update_shard_prepare(THD *thd, TABLE *table,
                                const mem_root_deque<Item *> *fields,
                                Item *conds) {
  if (!region_shard_checks_enabled(thd)) return false;
  if (is_global_config_table(table)) return false;
  if (!table_needs_shard_key(thd, table)) return false;

  if (table->s->shard_key.str == nullptr) {
    my_error(ER_SHARD_KEY_REQUIRED, MYF(0), table->s->db.str,
             table->s->table_name.str);
    return true;
  }

  Field *sf = shard_field(table);
  if (sf == nullptr) {
    my_error(ER_SHARD_KEY_INVALID, MYF(0), table->s->shard_key.str);
    return true;
  }

  if (fields != nullptr) {
    for (Item *item : *fields) {
      Item_field *fitem = item->field_for_view_update();
      if (fitem != nullptr && fitem->field == sf) {
        my_error(ER_CANNOT_UPDATE_SHARD_KEY, MYF(0), table->s->shard_key.str);
        return true;
      }
    }
  }

  if (conds == nullptr) {
    my_error(ER_DML_NEEDS_PK_OR_SHARD, MYF(0), table->s->shard_key.str);
    return true;
  }

  Item *shard_const = nullptr;
  bool shard_eq = has_shard_equality(table, conds, &shard_const);
  bool pk_eq = has_full_pk_equality(table, conds);
  if (!shard_eq && !pk_eq) {
    my_error(ER_DML_NEEDS_PK_OR_SHARD, MYF(0), table->s->shard_key.str);
    return true;
  }

  if (shard_eq && shard_const != nullptr && shard_const->const_for_execution()) {
    int64_t shard_val = shard_const->val_int();
    if (thd->is_error()) return true;
    return check_shard_value_writable(thd, table, shard_val);
  }
  return false;
}

bool check_delete_shard_prepare(THD *thd, TABLE *table, Item *conds) {
  return check_update_shard_prepare(thd, table, nullptr, conds);
}

bool check_row_shard_writable(THD *thd, TABLE *table) {
  if (!region_shard_checks_enabled(thd)) return false;
  if (is_global_config_table(table)) return false;
  if (!table_needs_shard_key(thd, table)) return false;
  if (table->s->shard_key.str == nullptr) {
    my_error(ER_SHARD_KEY_REQUIRED, MYF(0), table->s->db.str,
             table->s->table_name.str);
    return true;
  }
  Field *sf = shard_field(table);
  if (sf == nullptr || sf->is_null()) {
    my_error(ER_SHARD_KEY_INVALID, MYF(0), table->s->shard_key.str);
    return true;
  }
  return check_shard_value_writable(thd, table, sf->val_int());
}

bool check_global_config_row_write(THD *thd, TABLE *table, bool is_update,
                                   bool is_delete) {
  if (!is_global_config_table(table)) return false;
  // Always reload cache after commit when this table is modified (incl. applier).
  mark_global_config_changed(thd);
  // Skip content validation on replication applier; source already validated.
  if (thd != nullptr && thd->slave_thread) return false;
  Field *region_f = table->field[0];
  Field *shards_f = table->field[1];
  int64_t region = region_f->is_null() ? 0 : region_f->val_int();
  if (validate_global_config_write(thd, region, shards_f, is_delete,
                                   is_update))
    return true;
  return false;
}

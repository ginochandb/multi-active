# Multi-MySQL 多活

本仓库是 MySQL 8.4.8 LTS 的多活分支。多个地域的实例可以同时接受写入：每个实例只写入自己拥有的 shard，写入经 binlog 复制到其他地域。读语句在各地域本地执行，不受 shard 归属限制。

上游 MySQL 的许可说明见 [README](README)。通用编译步骤见 [Docs/README.build](Docs/README.build)。本文只描述相对上游的多活增量。

## 模型

每个实例用系统变量 `region` 标识自己所在的地域。业务表用 `SHARD KEY` 标出 shard 列。`mysql.global_config` 记录「哪个 region 拥有哪些 shard」。实例启动后把这张表载入内存；用户表写入时，用当前行的 shard 值查属主，属主等于本机 `region` 才允许写入。

`region = 0`（默认）时不做写归属校验。此时 `SHARD KEY` 可以省略；一旦写上，列名和整数类型仍会检查。`region` 非 0 时，用户表必须声明 `SHARD KEY`，并且只能写入归本 region 所有的 shard。

复制 SQL 线程不重复做归属校验。源端已经校验过的行事件，可以在其他 region 上应用，从而把本地域的写入同步到其余地域。

```mermaid
flowchart LR
  regionA["region 1 本地写入"] -->|"仅 shard 1,2"| binlogA[binlog]
  binlogA --> applierB["region 2 复制线程"]
  applierB --> dataB[本地数据]
  config["mysql.global_config"] --> cache["内存映射 shard 到 region"]
  cache --> regionA
```

## 配置

`region` 是只读全局变量，取值 `0` 到 `4294967295`，默认 `0`。它不写入 binlog，也不能在线修改：

```sql
SELECT @@global.region;
SHOW GLOBAL VARIABLES LIKE 'region';
SET GLOBAL region = 1;   -- ER_VARIABLE_IS_READONLY
```

启动时指定：

```text
mysqld --region=1
```

## 归属表

`mysql.global_config` 在初始化脚本 [scripts/mysql_system_tables.sql](scripts/mysql_system_tables.sql) 中创建。已有数据目录升级时，由 [scripts/mysql_system_tables_fix.sql](scripts/mysql_system_tables_fix.sql) 在表不存在时补建。

| 列 | 类型 | 含义 |
| --- | --- | --- |
| `region` | `BIGINT NOT NULL`，主键 | 拥有下列 shard 的地域编号 |
| `shards` | `JSON NOT NULL` | shard 编号的 JSON 整数数组，例如 `[1,2,3]` |

写入规则：

- `shards` 必须是 JSON 整数数组。对象、字符串等其他 JSON 类型会被拒绝。
- 同一个 shard 只能属于一个 region。把已属于其他 region 的 shard 再写入时失败。
- `UPDATE` 按「用新数组替换该 region 当前的 shard 列表」检查冲突：仍留在本 region 的 shard 不算冲突。
- 服务器启动时把全表载入内存映射（shard → region）。对本表的事务提交成功后，缓存整表重载。
- 复制线程应用本表的变更时跳过内容校验，提交后同样刷新缓存。源端已经做过校验。

```sql
INSERT INTO mysql.global_config (region, shards)
  VALUES (1, CAST('[1, 2]' AS JSON));
INSERT INTO mysql.global_config (region, shards)
  VALUES (2, CAST('[3]' AS JSON));

-- shard 2 已属于 region 1
INSERT INTO mysql.global_config (region, shards)
  VALUES (3, CAST('[2]' AS JSON));

UPDATE mysql.global_config
  SET shards = CAST('[1, 2, 5]' AS JSON)
  WHERE region = 1;

DELETE FROM mysql.global_config WHERE region = 2;
```

## 表定义

`SHARD KEY` 是建表选项，语法在 [sql/sql_yacc.yy](sql/sql_yacc.yy)：

```sql
CREATE TABLE t1 (
  id INT PRIMARY KEY,
  shard_id BIGINT NOT NULL,
  val INT
) ENGINE=InnoDB SHARD KEY(shard_id);
```

列必须已经出现在表定义里，并且是整数类型（`TINYINT`、`SMALLINT`、`MEDIUMINT`、`INT`、`BIGINT`、`YEAR`）。名字不存在或类型不符时报 `ER_SHARD_KEY_INVALID`。选项写入数据字典，`SHOW CREATE TABLE` 会带出 `SHARD KEY(...)`。

`region` 非 0 时，用户表建表必须带 `SHARD KEY`，否则报 `ER_SHARD_KEY_REQUIRED`。以下对象不做这项强制要求：

- 库 `mysql`、`sys`、`performance_schema`、`information_schema`
- 临时表
- 表类别不是用户表的系统表（权限、日志、复制、数据字典等）

`mysql.global_config` 本身没有 `SHARD KEY`，对它的增删改走单独的归属校验。

## 写入规则

以下检查在 `region != 0` 且当前线程不是复制线程时生效，实现见 [sql/sql_region_shard.cc](sql/sql_region_shard.cc)。行写入入口 [sql/handler.cc](sql/handler.cc) 的 `ha_write_row`、`ha_update_row`、`ha_delete_row` 会再按当前行的 shard 值核对一次属主。

**INSERT。** 列清单里必须包含 `SHARD KEY` 列（未写列清单、按全部列插入时视为已包含）。对应的值必须是执行期常量，且不能为 `NULL`。该 shard 在 `mysql.global_config` 中的属主必须等于本机 `region`。尚未登记的 shard 不可写。

**UPDATE。** 不能修改 `SHARD KEY` 列。`WHERE` 必须包含完整主键的等值条件，或者 `SHARD KEY` 列的等值条件。`WHERE` 里带有 shard 等值时，会先检查该 shard 是否归本 region。

**DELETE。** 与 `UPDATE` 相同：`WHERE` 必须是完整主键等值，或 `SHARD KEY` 列等值。删除行之前仍按该行实际 shard 检查属主。

下面的语句假定本机 `--region=1`，且归属表已按上一节写入 region 1 拥有 shard `1`、`2`，region 2 拥有 shard `3`。

```sql
INSERT INTO t1 (id, shard_id, val) VALUES (1, 1, 10);  -- 允许
INSERT INTO t1 (id, shard_id, val) VALUES (2, 2, 20);  -- 允许
INSERT INTO t1 (id, shard_id, val) VALUES (3, 3, 30);  -- shard 3 属于 region 2
INSERT INTO t1 (id, val) VALUES (4, 40);               -- 未给出 SHARD KEY 列

UPDATE t1 SET shard_id = 2 WHERE id = 1;               -- 禁止修改 SHARD KEY
UPDATE t1 SET val = 11 WHERE val = 10;                 -- WHERE 既不是主键也不是 shard
UPDATE t1 SET val = 11 WHERE id = 1;                   -- 主键等值，允许
UPDATE t1 SET val = 21 WHERE shard_id = 2;             -- shard 等值，允许

DELETE FROM t1 WHERE val = 11;                         -- WHERE 不足
DELETE FROM t1 WHERE id = 1;                           -- 允许
DELETE FROM t1 WHERE shard_id = 2;                     -- 允许
```

读语句（`SELECT`）不经过这套校验。

## 错误

客户端错误定义在 [share/messages_to_clients.txt](share/messages_to_clients.txt)。

| 错误 | 何时出现 |
| --- | --- |
| `ER_SHARD_KEY_REQUIRED` | `region` 已启用，用户表建表或写入时没有 `SHARD KEY` |
| `ER_SHARD_KEY_INVALID` | `SHARD KEY` 指向的列不存在，或不是整数类型；行上的 shard 值为 `NULL` |
| `ER_INSERT_SHARD_REQUIRED` | `INSERT` 未给出 `SHARD KEY` 列，或给出的值不是常量、为 `NULL` |
| `ER_CANNOT_UPDATE_SHARD_KEY` | `UPDATE` 的赋值列表包含 `SHARD KEY` 列 |
| `ER_DML_NEEDS_PK_OR_SHARD` | `UPDATE` / `DELETE` 的 `WHERE` 缺少完整主键等值，也缺少 shard 等值 |
| `ER_REGION_SHARD_READ_ONLY` | shard 未登记，或属主 region 与本机 `region` 不同 |
| `ER_GLOBAL_CONFIG_INVALID_SHARDS` | `mysql.global_config.shards` 不是 JSON 整数数组 |
| `ER_GLOBAL_CONFIG_SHARD_CONFLICT` | shard 已经属于另一个 region |

## 代码入口

| 路径 | 作用 |
| --- | --- |
| [sql/sql_region_shard.h](sql/sql_region_shard.h)、[sql/sql_region_shard.cc](sql/sql_region_shard.cc) | 建表与 DML 的 shard 校验 |
| [sql/global_config_cache.h](sql/global_config_cache.h)、[sql/global_config_cache.cc](sql/global_config_cache.cc) | `mysql.global_config` 的内存映射、冲突检查与提交后重载 |
| [sql/sys_vars.cc](sql/sys_vars.cc) | 系统变量 `region` |
| [sql/sql_yacc.yy](sql/sql_yacc.yy) | `SHARD KEY` 语法 |
| [sql/handler.cc](sql/handler.cc) | 行写入前的 shard 校验，以及归属表提交后刷新缓存 |
| [scripts/mysql_system_tables.sql](scripts/mysql_system_tables.sql)、[scripts/mysql_system_tables_fix.sql](scripts/mysql_system_tables_fix.sql) | 系统表的创建与升级补建 |
| [mysql-test/suite/region_shard](mysql-test/suite/region_shard) | 回归测试 |

## 编译与测试

按 [Docs/README.build](Docs/README.build) 编译。多活相关的运行参数是 `--region=<id>`。

回归测试在 `mysql-test` 目录下执行：

```text
./mysql-test-run.pl --suite=region_shard
```

套件包含三个用例：

- `global_config_dml`：归属表的插入、冲突、非法 JSON、更新与删除，以及 `region = 0` 时的 `SHARD KEY` 建表
- `region_sysvar`：`region` 可见，且不能在线修改
- `region_write_check`：`--region=1` 下的插入、更新、删除，以及系统库豁免

## 当前边界

- 读路径不按 region 拦截，各地域都可以读到已复制过来的行。
- 业务行的 shard 归属不能靠 `UPDATE` 修改 `SHARD KEY` 列来变更。调整归属应修改 `mysql.global_config`。
- `region` 只在进程启动时通过 `--region` 指定。
- 复制应用端信任源端已经做过的归属校验，本地 SQL 线程不再拒绝对方 region 的行事件。

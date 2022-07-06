/* Copyright (c) 2020, 2022, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/join_optimizer/explain_access_path.h"

#include <functional>
#include <string>
#include <vector>

#include <openssl/sha.h>

#include "my_base.h"
#include "sha2.h"
#include "sql-common/json_dom.h"
#include "sql/filesort.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_sum.h"
#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/bka_iterator.h"
#include "sql/iterators/composite_iterators.h"
#include "sql/iterators/hash_join_iterator.h"
#include "sql/iterators/ref_row_iterators.h"
#include "sql/iterators/sorting_iterator.h"
#include "sql/iterators/timing_iterator.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/print_utils.h"
#include "sql/join_optimizer/relational_expression.h"
#include "sql/opt_explain.h"
#include "sql/opt_explain_traditional.h"
#include "sql/query_result.h"
#include "sql/range_optimizer/group_index_skip_scan_plan.h"
#include "sql/range_optimizer/index_skip_scan_plan.h"
#include "sql/range_optimizer/internal.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"
#include "template_utils.h"

using std::string;
using std::vector;

/// This structure encapsulates the information needed to create a Json object
/// for a child access path.
struct ExplainChild {
  AccessPath *path;

  // Normally blank. If not blank, a heading for this iterator
  // saying what kind of role it has to the parent if it is not
  // obvious. E.g., FilterIterator can print iterators that are
  // children because they come out of subselect conditions.
  std::string description = "";

  // If this child is the root of a new JOIN, it is contained here.
  JOIN *join = nullptr;

  // If it's convenient to assign json fields for this child while creating this
  // structure, then a json object can be allocated and set here.
  Json_object *obj = nullptr;
};

/// Convenience function to add a json field
template <class T, class... Args>
static bool AddMemberToObject(Json_object *obj, const char *alias,
                              Args &&... ctor_args) {
  return obj->add_alias(
      alias, create_dom_ptr<T, Args...>(std::forward<Args>(ctor_args)...));
}

static string PrintRanges(const QUICK_RANGE *const *ranges, unsigned num_ranges,
                          const KEY_PART_INFO *key_part, bool single_part_only);
static std::unique_ptr<Json_object> ExplainAccessPath(
    const AccessPath *path, JOIN *join, bool is_root_of_join,
    Json_object *input_obj = nullptr);
static std::unique_ptr<Json_object> AssignParentPath(
    AccessPath *parent_path, std::unique_ptr<Json_object> obj, JOIN *join);
inline static double GetJSONDouble(const Json_object *obj, const char *key) {
  return down_cast<const Json_double *>(obj->get(key))->value();
}

string JoinTypeToString(JoinType join_type) {
  switch (join_type) {
    case JoinType::INNER:
      return "inner join";
    case JoinType::OUTER:
      return "left join";
    case JoinType::ANTI:
      return "antijoin";
    case JoinType::SEMI:
      return "semijoin";
    default:
      assert(false);
      return "<error>";
  }
}

string HashJoinTypeToString(RelationalExpression::Type join_type) {
  switch (join_type) {
    case RelationalExpression::INNER_JOIN:
    case RelationalExpression::STRAIGHT_INNER_JOIN:
      return "Inner hash join";
    case RelationalExpression::LEFT_JOIN:
      return "Left hash join";
    case RelationalExpression::ANTIJOIN:
      return "Hash antijoin";
    case RelationalExpression::SEMIJOIN:
      return "Hash semijoin";
    default:
      assert(false);
      return "<error>";
  }
}

static bool GetAccessPathsFromItem(Item *item_arg, const char *source_text,
                                   vector<ExplainChild> *children) {
  return WalkItem(
      item_arg, enum_walk::POSTFIX, [children, source_text](Item *item) {
        if (item->type() != Item::SUBSELECT_ITEM) {
          return false;
        }

        Item_subselect *subselect = down_cast<Item_subselect *>(item);
        Query_block *query_block = subselect->unit->first_query_block();
        char description[256];
        if (query_block->is_dependent()) {
          snprintf(description, sizeof(description),
                   "Select #%d (subquery in %s; dependent)",
                   query_block->select_number, source_text);
        } else if (!query_block->is_cacheable()) {
          snprintf(description, sizeof(description),
                   "Select #%d (subquery in %s; uncacheable)",
                   query_block->select_number, source_text);
        } else {
          snprintf(description, sizeof(description),
                   "Select #%d (subquery in %s; run only once)",
                   query_block->select_number, source_text);
        }
        if (query_block->join->needs_finalize) {
          subselect->unit->finalize(current_thd);
        }
        AccessPath *path;
        if (subselect->unit->root_access_path() != nullptr) {
          path = subselect->unit->root_access_path();
        } else {
          path = subselect->unit->item->root_access_path();
        }
        children->push_back({path, description, query_block->join});
        return false;
      });
}

static bool GetAccessPathsFromSelectList(JOIN *join,
                                         vector<ExplainChild> *children) {
  if (join == nullptr) {
    return false;
  }

  // Look for any Items in the projection list itself.
  for (Item *item : *join->get_current_fields()) {
    if (GetAccessPathsFromItem(item, "projection", children)) return true;
  }

  // Look for any Items that were materialized into fields during execution.
  for (uint table_idx = join->primary_tables; table_idx < join->tables;
       ++table_idx) {
    QEP_TAB *qep_tab = &join->qep_tab[table_idx];
    if (qep_tab != nullptr && qep_tab->tmp_table_param != nullptr) {
      for (Func_ptr &func : *qep_tab->tmp_table_param->items_to_copy) {
        if (GetAccessPathsFromItem(func.func(), "projection", children))
          return true;
      }
    }
  }
  return false;
}

static std::unique_ptr<Json_object> ExplainMaterializeAccessPath(
    const AccessPath *path, JOIN *join, std::unique_ptr<Json_object> ret_obj,
    vector<ExplainChild> *children, bool explain_analyze) {
  int error = 0;
  MaterializePathParameters *param = path->materialize().param;

  /*
    There may be multiple references to a CTE, but we should only print the
    plan once.
  */
  const bool explain_cte_now = param->cte != nullptr && [&]() {
    if (explain_analyze) {
      /*
        Find the temporary table for which the CTE was materialized, if there
        is one.
      */
      if (path->iterator == nullptr ||
          path->iterator->GetProfiler()->GetNumInitCalls() == 0) {
        // If the CTE was never materialized, print it at the first reference.
        return param->table == param->cte->tmp_tables[0]->table &&
               std::none_of(param->cte->tmp_tables.cbegin(),
                            param->cte->tmp_tables.cend(),
                            [](const TABLE_LIST *tab) {
                              return tab->table->materialized;
                            });
      } else {
        // The CTE was materialized here, print it now with cost data.
        return true;
      }
    } else {
      // If we do not want cost data, print the plan at the first reference.
      return param->table == param->cte->tmp_tables[0]->table;
    }
  }();

  const bool is_union = param->query_blocks.size() > 1;
  string str;

  if (param->cte != nullptr) {
    if (param->cte->recursive) {
      str = "Materialize recursive CTE " + to_string(param->cte->name);
    } else {
      if (is_union) {
        str = "Materialize union CTE " + to_string(param->cte->name);
      } else {
        str = "Materialize CTE " + to_string(param->cte->name);
      }
      if (param->cte->tmp_tables.size() > 1) {
        str += " if needed";
        if (!explain_cte_now) {
          // See children().
          str += " (query plan printed elsewhere)";
        }
      }
    }
  } else if (is_union) {
    str = "Union materialize";
  } else if (param->rematerialize) {
    str = "Temporary table";
  } else {
    str = "Materialize";
  }

  if (MaterializeIsDoingDeduplication(param->table)) {
    str += " with deduplication";
  }

  if (param->invalidators != nullptr) {
    bool first = true;
    str += " (invalidate on row from ";
    for (const AccessPath *invalidator : *param->invalidators) {
      if (!first) {
        str += "; ";
      }

      first = false;
      str += invalidator->cache_invalidator().name;
    }
    str += ")";
  }

  error |= AddMemberToObject<Json_string>(ret_obj.get(), "operation", str);

  /* Move the Materialize to the bottom of its table path, and return a new
   * object for this table path.
   */
  ret_obj =
      AssignParentPath(path->materialize().table_path, move(ret_obj), join);

  // Children.

  // If a CTE is referenced multiple times, only bother printing its query plan
  // once, instead of repeating it over and over again.
  //
  // TODO(sgunders): Consider printing CTE query plans on the top level of the
  // query block instead?
  if (param->cte != nullptr && !explain_cte_now) {
    return (error ? nullptr : std::move(ret_obj));
  }

  char heading[256] = "";

  if (param->limit_rows != HA_POS_ERROR) {
    // We call this “Limit table size” as opposed to “Limit”, to be able
    // to distinguish between the two in EXPLAIN when debugging.
    if (MaterializeIsDoingDeduplication(param->table)) {
      snprintf(heading, sizeof(heading), "Limit table size: %llu unique row(s)",
               param->limit_rows);
    } else {
      snprintf(heading, sizeof(heading), "Limit table size: %llu row(s)",
               param->limit_rows);
    }
  }

  // We don't list the table iterator as an explicit child; we mark it in
  // our description instead. (Anything else would look confusingly much
  // like a join.)
  for (const MaterializePathParameters::QueryBlock &query_block :
       param->query_blocks) {
    string this_heading = heading;

    if (query_block.disable_deduplication_by_hash_field) {
      if (this_heading.empty()) {
        this_heading = "Disable deduplication";
      } else {
        this_heading += ", disable deduplication";
      }
    }

    if (query_block.is_recursive_reference) {
      if (this_heading.empty()) {
        this_heading = "Repeat until convergence";
      } else {
        this_heading += ", repeat until convergence";
      }
    }

    children->push_back(
        {query_block.subquery_path, this_heading, query_block.join});
  }

  return (error ? nullptr : std::move(ret_obj));
}

static std::unique_ptr<Json_object> AssignParentPath(
    AccessPath *parent_path, std::unique_ptr<Json_object> obj, JOIN *join) {
  // We don't want to include the SELECT subquery list in the parent path;
  // Let them get printed in the actual root node. So is_root_of_join=false.
  std::unique_ptr<Json_object> newobj =
      ExplainAccessPath(parent_path, join, /*is_root_of_join=*/false);
  if (newobj == nullptr) return nullptr;

  /* Get the bottommost object from the new object tree. */
  Json_object *bottom_obj = newobj.get();
  while (bottom_obj->get("inputs") != nullptr) {
    Json_dom *children = bottom_obj->get("inputs");
    assert(children->json_type() == enum_json_type::J_ARRAY);
    Json_array *children_array = down_cast<Json_array *>(children);
    bottom_obj = down_cast<Json_object *>((*children_array)[0]);
  }

  /* Place the input object as a child of the bottom-most object */
  std::unique_ptr<Json_array> children(new (std::nothrow) Json_array());
  if (children == nullptr || children->append_alias(move(obj))) return nullptr;
  if (bottom_obj->add_alias("inputs", move(children))) return nullptr;

  return newobj;
}

static void ExplainIndexSkipScanAccessPath(const AccessPath *path,
                                           JOIN *join [[maybe_unused]],
                                           string *description) {
  TABLE *table = path->index_skip_scan().table;
  KEY *key_info = table->key_info + path->index_skip_scan().index;

  // NOTE: Currently, index skip scan is always covering, but there's no
  // good reason why we cannot fix this limitation in the future.
  *description = string(table->key_read ? "Covering index skip scan on "
                                        : "Index skip scan on ") +
                 table->alias + " using " + key_info->name + " over ";
  IndexSkipScanParameters *param = path->index_skip_scan().param;

  // Print out any equality ranges.
  bool first = true;
  for (unsigned key_part_idx = 0; key_part_idx < param->eq_prefix_key_parts;
       ++key_part_idx) {
    if (!first) {
      *description += ", ";
    }
    first = false;

    *description += param->index_info->key_part[key_part_idx].field->field_name;
    Bounds_checked_array<unsigned char *> prefixes =
        param->eq_prefixes[key_part_idx].eq_key_prefixes;
    if (prefixes.size() == 1) {
      *description += " = ";
      String out;
      print_key_value(&out, &param->index_info->key_part[key_part_idx],
                      prefixes[0]);
      *description += to_string(out);
    } else {
      *description += " IN (";
      for (unsigned i = 0; i < prefixes.size(); ++i) {
        if (i == 2 && prefixes.size() > 3) {
          *description += StringPrintf(", (%zu more)", prefixes.size() - 2);
          break;
        } else if (i != 0) {
          *description += ", ";
        }
        String out;
        print_key_value(&out, &param->index_info->key_part[key_part_idx],
                        prefixes[i]);
        *description += to_string(out);
      }
      *description += ")";
    }
  }

  // Then the ranges.
  if (!first) {
    *description += ", ";
  }
  String out;
  append_range(&out, param->range_key_part, param->min_range_key,
               param->max_range_key, param->range_cond_flag);
  *description += to_string(out);
}

static void ExplainGroupIndexSkipScanAccessPath(const AccessPath *path,
                                                JOIN *join [[maybe_unused]],
                                                string *description) {
  TABLE *table = path->group_index_skip_scan().table;
  KEY *key_info = table->key_info + path->group_index_skip_scan().index;
  GroupIndexSkipScanParameters *param = path->group_index_skip_scan().param;

  // NOTE: Currently, group index skip scan is always covering, but there's no
  // good reason why we cannot fix this limitation in the future.
  if (param->min_max_arg_part != nullptr) {
    *description =
        string(table->key_read ? "Covering index skip scan for grouping on "
                               : "Index skip scan for grouping on ") +
        table->alias + " using " + key_info->name;
  } else {
    *description = string(table->key_read
                              ? "Covering index skip scan for deduplication on "
                              : "Index skip scan for deduplication on ") +
                   table->alias + " using " + key_info->name;
  }

  // Print out prefix ranges, if any.
  if (!param->prefix_ranges.empty()) {
    *description += " over ";
    *description +=
        PrintRanges(param->prefix_ranges.data(), param->prefix_ranges.size(),
                    key_info->key_part, /*single_part_only=*/false);
  }

  // Print out the ranges on the MIN/MAX keypart, if we have them.
  // (We don't print infix ranges, because they seem to be in an unusual
  // format.)
  if (!param->min_max_ranges.empty()) {
    if (param->prefix_ranges.empty()) {
      *description += " over ";
    } else {
      *description += ", ";
    }
    *description +=
        PrintRanges(param->min_max_ranges.data(), param->min_max_ranges.size(),
                    param->min_max_arg_part,
                    /*single_part_only=*/true);
  }
}

static bool AddChildrenFromPushedCondition(const TABLE *table,
                                           vector<ExplainChild> *children) {
  /*
    A table access path is normally a leaf node in the set of paths.
    The exception is if a subquery was included as part of an
    'engine_condition_pushdown'. In such cases the subquery has
    been evaluated prior to accessing this table, and the result(s)
    from the subquery materialized into the pushed condition.
    Report such subqueries as children of this table.
  */
  Item *pushed_cond = const_cast<Item *>(table->file->pushed_cond);

  if (pushed_cond != nullptr) {
    if (GetAccessPathsFromItem(pushed_cond, "pushed condition", children))
      return true;
  }
  return false;
}

static string PrintRanges(const QUICK_RANGE *const *ranges, unsigned num_ranges,
                          const KEY_PART_INFO *key_part,
                          bool single_part_only) {
  string ret;
  for (unsigned range_idx = 0; range_idx < num_ranges; ++range_idx) {
    if (range_idx == 2 && num_ranges > 3) {
      char str[256];
      snprintf(str, sizeof(str), " OR (%u more)", num_ranges - 2);
      ret += str;
      break;
    } else if (range_idx > 0) {
      ret += " OR ";
    }
    String str;
    if (single_part_only) {
      // key_part is the part we are printing on,
      // and we have to ignore min_keypart_map / max_keypart_map,
      // so we cannot use append_range_to_string().
      append_range(&str, key_part, ranges[range_idx]->min_key,
                   ranges[range_idx]->max_key, ranges[range_idx]->flag);
    } else {
      // NOTE: key_part is the first keypart in the key.
      append_range_to_string(ranges[range_idx], key_part, &str);
    }
    ret += "(" + to_string(str) + ")";
  }
  return ret;
}

static bool AddChildrenToObject(Json_object *obj,
                                const vector<ExplainChild> &children,
                                JOIN *parent_join, bool parent_is_root_of_join,
                                string alias) {
  if (children.empty()) return false;

  std::unique_ptr<Json_array> children_json(new (std::nothrow) Json_array());
  if (children_json == nullptr) return true;

  for (const ExplainChild &child : children) {
    JOIN *subjoin = child.join != nullptr ? child.join : parent_join;
    bool child_is_root_of_join =
        subjoin != parent_join || parent_is_root_of_join;

    std::unique_ptr<Json_object> child_obj = ExplainAccessPath(
        child.path, subjoin, child_is_root_of_join, child.obj);
    if (child_obj == nullptr) return true;
    if (!child.description.empty()) {
      if (AddMemberToObject<Json_string>(child_obj.get(), "heading",
                                         child.description))
        return true;
    }
    if (children_json->append_alias(move(child_obj))) return true;
  }

  return obj->add_alias(alias, move(children_json));
}

static std::unique_ptr<Json_object> ExplainQueryPlan(
    const AccessPath *path, THD::Query_plan const *query_plan, JOIN *join,
    bool is_root_of_join) {
  string dml_desc;
  std::unique_ptr<Json_object> obj = nullptr;

  /* Create a Json object for the SELECT path */
  if (path != nullptr) {
    obj = ExplainAccessPath(path, join, is_root_of_join);
    if (obj == nullptr) return nullptr;
  }
  if (query_plan != nullptr) {
    switch (query_plan->get_command()) {
      case SQLCOM_INSERT_SELECT:
      case SQLCOM_INSERT:
        dml_desc = string("Insert into ") +
                   query_plan->get_lex()->insert_table_leaf->table->alias;
        break;
      case SQLCOM_REPLACE_SELECT:
      case SQLCOM_REPLACE:
        dml_desc = string("Replace into ") +
                   query_plan->get_lex()->insert_table_leaf->table->alias;
        break;
      default:
        // SELECTs have no top-level node.
        break;
    }
  }

  /* If there is a DML node, add it on top of the SELECT plan */
  if (!dml_desc.empty()) {
    std::unique_ptr<Json_object> dml_obj(new (std::nothrow) Json_object());
    if (dml_obj == nullptr) return nullptr;
    if (AddMemberToObject<Json_string>(dml_obj.get(), "operation", dml_desc))
      return nullptr;

    /* There might not be a select plan. E.g. INSERT ... VALUES() */
    if (obj != nullptr) {
      std::unique_ptr<Json_array> children(new (std::nothrow) Json_array());
      if (children == nullptr || children->append_alias(move(obj)))
        return nullptr;
      if (dml_obj->add_alias("inputs", move(children))) return nullptr;
    }
    obj = move(dml_obj);
  }

  return obj;
}

/* Append the various costs. */
static bool AddPathCosts(const AccessPath *path, Json_object *obj,
                         bool explain_analyze) {
  int error = 0;

  if (path->num_output_rows >= 0.0) {
    // Calculate first row cost
    if (path->init_cost >= 0.0) {
      double first_row_cost;
      if (path->num_output_rows <= 1.0) {
        first_row_cost = path->cost;
      } else {
        first_row_cost = path->init_cost +
                         (path->cost - path->init_cost) / path->num_output_rows;
      }
      error |= AddMemberToObject<Json_double>(obj, "estimated_first_row_cost",
                                              first_row_cost);
    }
    error |=
        AddMemberToObject<Json_double>(obj, "estimated_total_cost", path->cost);
    error |= AddMemberToObject<Json_double>(obj, "estimated_rows",
                                            path->num_output_rows);
  } /* if (path->num_output_rows >= 0.0) */

  /* Add analyze figures */
  if (explain_analyze) {
    int num_init_calls = 0;

    if (path->iterator != nullptr) {
      const IteratorProfiler *const profiler = path->iterator->GetProfiler();
      if ((num_init_calls = profiler->GetNumInitCalls()) != 0) {
        error |= AddMemberToObject<Json_double>(
            obj, "actual_first_row_ms",
            profiler->GetFirstRowMs() / profiler->GetNumInitCalls());
        error |= AddMemberToObject<Json_double>(
            obj, "actual_last_row_ms",
            profiler->GetLastRowMs() / profiler->GetNumInitCalls());
        error |= AddMemberToObject<Json_double>(
            obj, "actual_rows",
            static_cast<double>(profiler->GetNumRows()) / num_init_calls);
        error |=
            AddMemberToObject<Json_int>(obj, "actual_loops", num_init_calls);
      }
    }

    if (num_init_calls == 0) {
      error |= AddMemberToObject<Json_null>(obj, "actual_first_row_ms");
      error |= AddMemberToObject<Json_null>(obj, "actual_last_row_ms");
      error |= AddMemberToObject<Json_null>(obj, "actual_rows");
      error |= AddMemberToObject<Json_null>(obj, "actual_loops");
    }
  }
  return error;
}

/*
   Given a json object, update it's appropriate json fields according to the
   input path. Also update the 'children' with a flat list of direct children
   of the passed object.  In most of cases, the returned object is same as the
   input object, but for some paths it can be different. So callers should use
   the returned object.

   Note: This function has shown to consume excessive stack space, particularly
   in debug builds. Hence make sure this function does not directly or
   indirectly create any json children objects recursively. It may cause stack
   overflow. Hence json children are created only after this function returns
   in function ExplainAccessPath().
*/
static std::unique_ptr<Json_object> SetObjectMembers(
    std::unique_ptr<Json_object> ret_obj, const AccessPath *path, JOIN *join,
    vector<ExplainChild> *children) {
  int error = 0;
  string description;

  // The obj to be returned might get changed when processing some of the
  // paths. So keep a handle to the original object, in case we later add any
  // more fields.
  Json_object *obj = ret_obj.get();

  /* Get path-specific info, including the description string */
  switch (path->type) {
    case AccessPath::TABLE_SCAN: {
      TABLE *table = path->table_scan().table;
      description += string("Table scan on ") + table->alias +
                     table->file->explain_extra();
      error |= AddChildrenFromPushedCondition(table, children);
      break;
    }
    case AccessPath::INDEX_SCAN: {
      TABLE *table = path->index_scan().table;
      assert(table->file->pushed_idx_cond == nullptr);

      const KEY *key = &table->key_info[path->index_scan().idx];
      description += string(table->key_read ? "Covering index scan on "
                                            : "Index scan on ") +
                     table->alias + " using " + key->name;
      if (path->index_scan().reverse) {
        description += " (reverse)";
      }
      description += table->file->explain_extra();
      error |= AddChildrenFromPushedCondition(table, children);
      break;
    }
    case AccessPath::REF: {
      TABLE *table = path->ref().table;
      const KEY *key = &table->key_info[path->ref().ref->key];
      description +=
          string(table->key_read ? "Covering index lookup on "
                                 : "Index lookup on ") +
          table->alias + " using " + key->name + " (" +
          RefToString(*path->ref().ref, key, /*include_nulls=*/false);
      if (path->ref().reverse) {
        description += "; iterate backwards";
      }
      description += ")";
      if (table->file->pushed_idx_cond != nullptr) {
        description += ", with index condition: " +
                       ItemToString(table->file->pushed_idx_cond);
      }
      description += table->file->explain_extra();
      error |= AddChildrenFromPushedCondition(table, children);
      break;
    }
    case AccessPath::REF_OR_NULL: {
      TABLE *table = path->ref_or_null().table;
      const KEY *key = &table->key_info[path->ref_or_null().ref->key];
      description =
          string(table->key_read ? "Covering index lookup on "
                                 : "Index lookup on ") +
          table->alias + " using " + key->name + " (" +
          RefToString(*path->ref_or_null().ref, key, /*include_nulls=*/true) +
          ")";
      if (table->file->pushed_idx_cond != nullptr) {
        description += ", with index condition: " +
                       ItemToString(table->file->pushed_idx_cond);
      }
      description += table->file->explain_extra();
      error |= AddChildrenFromPushedCondition(table, children);
      break;
    }
    case AccessPath::EQ_REF: {
      TABLE *table = path->eq_ref().table;
      const KEY *key = &table->key_info[path->eq_ref().ref->key];
      description =
          string(table->key_read ? "Single-row covering index lookup on "
                                 : "Single-row index lookup on ") +
          table->alias + " using " + key->name + " (" +
          RefToString(*path->eq_ref().ref, key, /*include_nulls=*/false) + ")";
      if (table->file->pushed_idx_cond != nullptr) {
        description += ", with index condition: " +
                       ItemToString(table->file->pushed_idx_cond);
      }
      description += table->file->explain_extra();
      error |= AddChildrenFromPushedCondition(table, children);
      break;
    }
    case AccessPath::PUSHED_JOIN_REF: {
      TABLE *table = path->pushed_join_ref().table;
      assert(table->file->pushed_idx_cond == nullptr);
      const KEY *key = &table->key_info[path->pushed_join_ref().ref->key];
      if (path->pushed_join_ref().is_unique) {
        description =
            table->key_read ? "Single-row covering index" : "Single-row index";
      } else {
        description += table->key_read ? "Covering index" : "Index";
      }
      description += " lookup on " + string(table->alias) + " using " +
                     key->name + " (" +
                     RefToString(*path->pushed_join_ref().ref, key,
                                 /*include_nulls=*/false) +
                     ")" + table->file->explain_extra();
      break;
    }
    case AccessPath::FULL_TEXT_SEARCH: {
      TABLE *table = path->full_text_search().table;
      assert(table->file->pushed_idx_cond == nullptr);
      const KEY *key = &table->key_info[path->full_text_search().ref->key];
      description +=
          string(table->key_read ? "Full-text covering index search on "
                                 : "Full-text index search on ") +
          table->alias + " using " + key->name + " (" +
          RefToString(*path->full_text_search().ref, key,
                      /*include_nulls=*/false) +
          ")" + table->file->explain_extra();
      break;
    }
    case AccessPath::CONST_TABLE: {
      TABLE *table = path->const_table().table;
      assert(table->file->pushed_idx_cond == nullptr);
      assert(table->file->pushed_cond == nullptr);
      description = string("Constant row from ") + table->alias;
      break;
    }
    case AccessPath::MRR: {
      TABLE *table = path->mrr().table;
      const KEY *key = &table->key_info[path->mrr().ref->key];
      description =
          string(table->key_read ? "Multi-range covering index lookup on "
                                 : "Multi-range index lookup on ") +
          table->alias + " using " + key->name + " (" +
          RefToString(*path->mrr().ref, key, /*include_nulls=*/false) + ")";
      if (table->file->pushed_idx_cond != nullptr) {
        description += ", with index condition: " +
                       ItemToString(table->file->pushed_idx_cond);
      }
      description += table->file->explain_extra();
      error |= AddChildrenFromPushedCondition(table, children);
      break;
    }
    case AccessPath::FOLLOW_TAIL:
      description =
          string("Scan new records on ") + path->follow_tail().table->alias;
      error |=
          AddChildrenFromPushedCondition(path->follow_tail().table, children);
      break;
    case AccessPath::INDEX_RANGE_SCAN: {
      const auto &param = path->index_range_scan();
      TABLE *table = param.used_key_part[0].field->table;
      KEY *key_info = table->key_info + param.index;
      description = string(table->key_read ? "Covering index range scan on "
                                           : "Index range scan on ") +
                    table->alias + " using " + key_info->name + " over ";
      description +=
          PrintRanges(param.ranges, param.num_ranges, key_info->key_part,
                      /*single_part_only=*/false);
      if (path->index_range_scan().reverse) {
        description += " (reverse)";
      }
      if (table->file->pushed_idx_cond != nullptr) {
        description += ", with index condition: " +
                       ItemToString(table->file->pushed_idx_cond);
      }
      description += table->file->explain_extra();
      error |= AddChildrenFromPushedCondition(table, children);
      break;
    }
    case AccessPath::INDEX_MERGE: {
      const auto &param = path->index_merge();
      description = "Sort-deduplicate by row ID";
      for (AccessPath *child : *path->index_merge().children) {
        if (param.allow_clustered_primary_key_scan &&
            param.table->file->primary_key_is_clustered() &&
            child->index_range_scan().index == param.table->s->primary_key) {
          children->push_back(
              {child, "Clustered primary key (scanned separately)"});
        } else {
          children->push_back({child});
        }
      }
      break;
    }
    case AccessPath::ROWID_INTERSECTION: {
      description = "Intersect rows sorted by row ID";
      for (AccessPath *child : *path->rowid_intersection().children) {
        children->push_back({child});
      }
      break;
    }
    case AccessPath::ROWID_UNION: {
      description = "Deduplicate rows sorted by row ID";
      for (AccessPath *child : *path->rowid_union().children) {
        children->push_back({child});
      }
      break;
    }
    case AccessPath::INDEX_SKIP_SCAN: {
      ExplainIndexSkipScanAccessPath(path, join, &description);
      break;
    }
    case AccessPath::GROUP_INDEX_SKIP_SCAN: {
      ExplainGroupIndexSkipScanAccessPath(path, join, &description);
      break;
    }
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN: {
      TABLE *table = path->dynamic_index_range_scan().table;
      description += string(table->key_read ? "Covering index range scan on "
                                            : "Index range scan on ") +
                     table->alias + " (re-planned for each iteration)";
      if (table->file->pushed_idx_cond != nullptr) {
        description += ", with index condition: " +
                       ItemToString(table->file->pushed_idx_cond);
      }
      description += table->file->explain_extra();
      error |= AddChildrenFromPushedCondition(table, children);
      break;
    }
    case AccessPath::TABLE_VALUE_CONSTRUCTOR:
    case AccessPath::FAKE_SINGLE_ROW:
      description = "Rows fetched before execution";
      break;
    case AccessPath::ZERO_ROWS:
      description = string("Zero rows (") + path->zero_rows().cause + ")";
      // The child is not printed as part of the iterator tree.
      break;
    case AccessPath::ZERO_ROWS_AGGREGATED:
      description = string("Zero input rows (") +
                    path->zero_rows_aggregated().cause +
                    "), aggregated into one output row";
      break;
    case AccessPath::MATERIALIZED_TABLE_FUNCTION:
      description = "Materialize table function";
      break;
    case AccessPath::UNQUALIFIED_COUNT:
      description = "Count rows in " + string(join->qep_tab->table()->alias);
      break;
    case AccessPath::NESTED_LOOP_JOIN:
      description =
          "Nested loop " + JoinTypeToString(path->nested_loop_join().join_type);

      children->push_back({path->nested_loop_join().outer});
      children->push_back({path->nested_loop_join().inner});
      break;
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
      description =
          string("Nested loop semijoin with duplicate removal on ") +
          path->nested_loop_semijoin_with_duplicate_removal().key->name;
      children->push_back(
          {path->nested_loop_semijoin_with_duplicate_removal().outer});
      children->push_back(
          {path->nested_loop_semijoin_with_duplicate_removal().inner});
      break;
    case AccessPath::BKA_JOIN:
      description =
          "Batched key access " + JoinTypeToString(path->bka_join().join_type);
      children->push_back({path->bka_join().outer, "Batch input rows"});
      children->push_back({path->bka_join().inner});
      break;
    case AccessPath::HASH_JOIN: {
      const JoinPredicate *predicate = path->hash_join().join_predicate;
      RelationalExpression::Type type = path->hash_join().rewrite_semi_to_inner
                                            ? RelationalExpression::INNER_JOIN
                                            : predicate->expr->type;
      description = HashJoinTypeToString(type);

      if (predicate->expr->equijoin_conditions.empty()) {
        description.append(" (no condition)");
      } else {
        for (Item_eq_base *cond : predicate->expr->equijoin_conditions) {
          if (cond != predicate->expr->equijoin_conditions[0]) {
            description.push_back(',');
          }
          HashJoinCondition hj_cond(cond, *THR_MALLOC);
          if (!hj_cond.store_full_sort_key()) {
            description.append(
                " (<hash>(" + ItemToString(hj_cond.left_extractor()) +
                ")=<hash>(" + ItemToString(hj_cond.right_extractor()) + "))");
          } else {
            description.append(" " + ItemToString(cond));
          }
        }
      }
      for (Item *cond : predicate->expr->join_conditions) {
        if (cond == predicate->expr->join_conditions[0]) {
          description.append(", extra conditions: ");
        } else {
          description += " and ";
        }
        description += ItemToString(cond);
      }
      children->push_back({path->hash_join().outer});
      children->push_back({path->hash_join().inner, "Hash"});
      break;
    }
    case AccessPath::FILTER:
      description = "Filter: " + ItemToString(path->filter().condition);
      children->push_back({path->filter().child});
      GetAccessPathsFromItem(path->filter().condition, "condition", children);
      break;
    case AccessPath::SORT: {
      if (path->sort().force_sort_rowids) {
        description = "Sort row IDs";
      } else {
        description = "Sort";
      }
      if (path->sort().remove_duplicates) {
        description += " with duplicate removal: ";
      } else {
        description += ": ";
      }

      for (ORDER *order = path->sort().order; order != nullptr;
           order = order->next) {
        if (order != path->sort().order) {
          description += ", ";
        }

        // We usually want to print the item_name if it's set, so that we get
        // the alias instead of the full expression when there is an alias. If
        // it is a field reference, we prefer ItemToString() because item_name
        // in Item_field doesn't include the table name.
        if (const Item *item = *order->item;
            item->item_name.is_set() && item->type() != Item::FIELD_ITEM) {
          description += item->item_name.ptr();
        } else {
          description += ItemToString(item);
        }
        if (order->direction == ORDER_DESC) {
          description += " DESC";
        }
      }

      if (const ha_rows limit = path->sort().limit; limit != HA_POS_ERROR) {
        char buf[256];
        snprintf(buf, sizeof(buf), ", limit input to %llu row(s) per chunk",
                 limit);
        description += buf;
      }
      children->push_back({path->sort().child});
      break;
    }
    case AccessPath::AGGREGATE: {
      if (join->grouped || join->group_optimized_away) {
        if (*join->sum_funcs == nullptr) {
          description = "Group (no aggregates)";
        } else if (path->aggregate().rollup) {
          description = "Group aggregate with rollup: ";
        } else {
          description = "Group aggregate: ";
        }
      } else {
        description = "Aggregate: ";
      }

      bool first = true;
      for (Item_sum **item = join->sum_funcs; *item != nullptr; ++item) {
        if (first) {
          first = false;
        } else {
          description += ", ";
        }
        if (path->aggregate().rollup) {
          description += ItemToString((*item)->unwrap_sum());
        } else {
          description += ItemToString(*item);
        }
      }
      children->push_back({path->aggregate().child});
      break;
    }
    case AccessPath::TEMPTABLE_AGGREGATE: {
      ret_obj = AssignParentPath(path->temptable_aggregate().table_path,
                                 move(ret_obj), join);
      if (ret_obj == nullptr) return nullptr;
      description = "Aggregate using temporary table";
      children->push_back({path->temptable_aggregate().subquery_path});
      break;
    }
    case AccessPath::LIMIT_OFFSET: {
      char buf[256];
      if (path->limit_offset().offset == 0) {
        snprintf(buf, sizeof(buf), "Limit: %llu row(s)",
                 path->limit_offset().limit);
      } else if (path->limit_offset().limit == HA_POS_ERROR) {
        snprintf(buf, sizeof(buf), "Offset: %llu row(s)",
                 path->limit_offset().offset);
      } else {
        snprintf(buf, sizeof(buf), "Limit/Offset: %llu/%llu row(s)",
                 path->limit_offset().limit - path->limit_offset().offset,
                 path->limit_offset().offset);
      }
      if (path->limit_offset().count_all_rows) {
        description =
            string(buf) + " (no early end due to SQL_CALC_FOUND_ROWS)";
      } else {
        description = buf;
      }
      children->push_back({path->limit_offset().child});
      break;
    }
    case AccessPath::STREAM:
      description = "Stream results";
      children->push_back({path->stream().child});
      break;
    case AccessPath::MATERIALIZE:
      ret_obj =
          ExplainMaterializeAccessPath(path, join, move(ret_obj), children,
                                       current_thd->lex->is_explain_analyze);
      if (ret_obj == nullptr) return nullptr;
      break;
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE:
      ret_obj = AssignParentPath(
          path->materialize_information_schema_table().table_path,
          move(ret_obj), join);
      if (ret_obj == nullptr) return nullptr;
      description = "Fill information schema table " +
                    string(path->materialize_information_schema_table()
                               .table_list->table->alias);
      break;
    case AccessPath::APPEND:
      description = "Append";
      for (const AppendPathParameters &child : *path->append().children) {
        children->push_back({child.path, "", child.join});
      }
      break;
    case AccessPath::WINDOW: {
      Window *const window = path->window().window;
      if (path->window().needs_buffering) {
        if (window->optimizable_row_aggregates() ||
            window->optimizable_range_aggregates() ||
            window->static_aggregates()) {
          description = "Window aggregate with buffering: ";
        } else {
          description = "Window multi-pass aggregate with buffering: ";
        }
      } else {
        description = "Window aggregate: ";
      }

      bool first = true;
      for (const Item_sum &func : window->functions()) {
        if (!first) {
          description += ", ";
        }
        description += ItemToString(&func);
        first = false;
      }
      children->push_back({path->window().child});
      break;
    }
    case AccessPath::WEEDOUT: {
      SJ_TMP_TABLE *sj = path->weedout().weedout_table;

      description = "Remove duplicate ";
      if (sj->tabs_end == sj->tabs + 1) {  // Only one table.
        description += sj->tabs->qep_tab->table()->alias;
      } else {
        description += "(";
        for (SJ_TMP_TABLE_TAB *tab = sj->tabs; tab != sj->tabs_end; ++tab) {
          if (tab != sj->tabs) {
            description += ", ";
          }
          description += tab->qep_tab->table()->alias;
        }
        description += ")";
      }
      description += " rows using temporary table (weedout)";
      children->push_back({path->weedout().child});
      break;
    }
    case AccessPath::REMOVE_DUPLICATES: {
      description = "Remove duplicates from input grouped on ";
      for (int i = 0; i < path->remove_duplicates().group_items_size; ++i) {
        if (i != 0) {
          description += ", ";
        }
        description += ItemToString(path->remove_duplicates().group_items[i]);
      }
      children->push_back({path->remove_duplicates().child});
      break;
    }
    case AccessPath::REMOVE_DUPLICATES_ON_INDEX:
      description = string("Remove duplicates from input sorted on ") +
                    path->remove_duplicates_on_index().key->name;
      children->push_back({path->remove_duplicates_on_index().child});
      break;
    case AccessPath::ALTERNATIVE: {
      const TABLE *table =
          path->alternative().table_scan_path->table_scan().table;
      const TABLE_REF *ref = path->alternative().used_ref;
      const KEY *key = &table->key_info[ref->key];

      int num_applicable_cond_guards = 0;
      for (unsigned key_part_idx = 0; key_part_idx < ref->key_parts;
           ++key_part_idx) {
        if (ref->cond_guards[key_part_idx] != nullptr) {
          ++num_applicable_cond_guards;
        }
      }

      description = "Alternative plans for IN subquery: Index lookup unless ";
      if (num_applicable_cond_guards > 1) {
        description += " any of (";
      }
      bool first = true;
      for (unsigned key_part_idx = 0; key_part_idx < ref->key_parts;
           ++key_part_idx) {
        if (ref->cond_guards[key_part_idx] != nullptr) {
          if (!first) {
            description += ", ";
          }
          first = false;
          description += key->key_part[key_part_idx].field->field_name;
        }
      }
      if (num_applicable_cond_guards > 1) {
        description += ")";
      }
      description += " IS NULL";
      children->push_back({path->alternative().child});
      children->push_back({path->alternative().table_scan_path});
      break;
    }
    case AccessPath::CACHE_INVALIDATOR:
      description = string("Invalidate materialized tables (row from ") +
                    path->cache_invalidator().name + ")";
      children->push_back({path->cache_invalidator().child});
      break;
    case AccessPath::DELETE_ROWS: {
      string tables;
      for (TABLE_LIST *t = join->query_block->leaf_tables; t != nullptr;
           t = t->next_leaf) {
        if (Overlaps(t->map(), path->delete_rows().tables_to_delete_from)) {
          if (!tables.empty()) {
            tables.append(", ");
          }
          tables.append(t->alias);
          if (Overlaps(t->map(), path->delete_rows().immediate_tables)) {
            tables.append(" (immediate)");
          } else {
            tables.append(" (buffered)");
          }
        }
      }
      description = string("Delete from ") + tables;
      children->push_back({path->delete_rows().child});
      break;
    }
    case AccessPath::UPDATE_ROWS: {
      string tables;
      for (TABLE_LIST *t = join->query_block->leaf_tables; t != nullptr;
           t = t->next_leaf) {
        if (Overlaps(t->map(), path->update_rows().tables_to_update)) {
          if (!tables.empty()) {
            tables.append(", ");
          }
          tables.append(t->alias);
          if (Overlaps(t->map(), path->update_rows().immediate_tables)) {
            tables.append(" (immediate)");
          } else {
            tables.append(" (buffered)");
          }
        }
      }
      description = string("Update ") + tables;
      children->push_back({path->update_rows().child});
      break;
    }
  }

  // Append the various costs.
  error |= AddPathCosts(path, obj, current_thd->lex->is_explain_analyze);

  // Empty description means the object already has the description set above.
  if (!description.empty()) {
    // Create JSON objects for description strings.
    error |= AddMemberToObject<Json_string>(obj, "operation", description);
  }

  return (error ? nullptr : move(ret_obj));
}

/*
   Convert the AccessPath into a Json object that represents the EXPLAIN output
   This Json object may in turn be used to output in whichever required format.
*/
static std::unique_ptr<Json_object> ExplainAccessPath(const AccessPath *path,
                                                      JOIN *join,
                                                      bool is_root_of_join,
                                                      Json_object *input_obj) {
  int error = 0;
  vector<ExplainChild> children;
  Json_object *obj;
  std::unique_ptr<Json_object> ret_obj(input_obj);

  if (ret_obj == nullptr) {
    ret_obj = create_dom_ptr<Json_object>();
  }
  // Keep a handle to the original object.
  obj = ret_obj.get();

  // This should not happen, but some unit tests have shown to cause null child
  // paths to be present in the AccessPath tree.
  if (path == nullptr) {
    if (AddMemberToObject<Json_string>(obj, "operation",
                                       "<not executable by iterator executor>"))
      return nullptr;
    return ret_obj;
  }

  if ((ret_obj = SetObjectMembers(move(ret_obj), path, join, &children)) ==
      nullptr)
    return nullptr;

  // If we are crossing into a different query block, but there's a streaming
  // or materialization node in the way, don't count it as the root; we want
  // any SELECT printouts to be on the actual root node.
  // TODO(sgunders): This gives the wrong result if a query block ends in a
  // materialization.
  bool delayed_root_of_join = false;
  if (path->type == AccessPath::STREAM ||
      path->type == AccessPath::MATERIALIZE) {
    delayed_root_of_join = is_root_of_join;
    is_root_of_join = false;
  }

  if (AddChildrenToObject(obj, children, join, delayed_root_of_join, "inputs"))
    return nullptr;

  // If we know that the join will return zero rows, we don't bother
  // optimizing any subqueries in the SELECT list, but end optimization
  // early (see Query_block::optimize()). If so, don't attempt to print
  // them either, as they have no query plan.
  if (is_root_of_join && path->type != AccessPath::ZERO_ROWS) {
    vector<ExplainChild> children_from_select;
    if (GetAccessPathsFromSelectList(join, &children_from_select))
      return nullptr;
    if (AddChildrenToObject(obj, children_from_select, join,
                            /*is_root_of_join*/ true,
                            "inputs_from_select_list"))
      return nullptr;
  }

  if (error == 0)
    return ret_obj;
  else
    return nullptr;
}

std::string PrintQueryPlan(THD *ethd, const THD *query_thd,
                           Query_expression *unit) {
  JOIN *join = nullptr;
  bool is_root_of_join = (unit != nullptr ? !unit->is_union() : false);
  AccessPath *path = (unit != nullptr ? unit->root_access_path() : nullptr);

  if (path == nullptr) return "<not executable by iterator executor>\n";

  // "join" should be set to the JOIN that "path" is part of (or nullptr
  // if it is not, e.g. if it's a part of executing a UNION).
  if (unit != nullptr && !unit->is_union())
    join = unit->first_query_block()->join;

  /* Create a Json object for the plan */
  std::unique_ptr<Json_object> obj =
      ExplainQueryPlan(path, &query_thd->query_plan, join, is_root_of_join);
  if (obj == nullptr) return "";

  // Append the (rewritten) query string, if any.
  // Skip this if applicable. See print_query_for_explain() comments.
  if (ethd == query_thd) {
    StringBuffer<1024> str;
    print_query_for_explain(query_thd, unit, &str);
    if (!str.is_empty()) {
      if (AddMemberToObject<Json_string>(obj.get(), "query", str.ptr(),
                                         str.length()))
        return "";
    }
  }

  /*
    Output should be either in json format, or a tree format, depending on
    the specified format
   */
  return ethd->lex->explain_format->ExplainJsonToString(obj.get());
}

/* PrintQueryPlan()
 * This overloaded function is for debugging purpose.
 */
std::string PrintQueryPlan(int level, AccessPath *path, JOIN *join,
                           bool is_root_of_join) {
  string ret;
  Explain_format_tree format;

  if (path == nullptr) {
    ret.assign(level * 4, ' ');
    return ret + "<not executable by iterator executor>\n";
  }

  /* Create a Json object for the plan */
  std::unique_ptr<Json_object> json =
      ExplainAccessPath(path, join, is_root_of_join);
  if (json == nullptr) return "";

  /* Output in tree format.*/
  string explain;
  format.ExplainPrintTreeNode(json.get(), level, &explain, /*tokens=*/nullptr);
  return explain;
}

// 0x
// truncated_sha256(desc1,desc2,...,[child1_desc:]0xchild1,[child2_desc:]0xchild2,...)
static string GetForceSubplanToken(const Json_object *obj,
                                   const string &children_digest) {
  string digest;
  digest += down_cast<Json_string *>(obj->get("operation"))->value() +
            children_digest;

  unsigned char sha256sum[SHA256_DIGEST_LENGTH];
  (void)SHA_EVP256(pointer_cast<const unsigned char *>(digest.data()),
                   digest.size(), sha256sum);

  char ret[8 * 2 + 2 + 1];
  snprintf(ret, sizeof(ret), "0x%02x%02x%02x%02x%02x%02x%02x%02x", sha256sum[0],
           sha256sum[1], sha256sum[2], sha256sum[3], sha256sum[4], sha256sum[5],
           sha256sum[6], sha256sum[7]);

  return ret;
}

string GetForceSubplanToken(AccessPath *path, JOIN *join) {
  if (path == nullptr) {
    return "";
  }

  Explain_format_tree format;
  string explain;
  vector<string> tokens_for_force_subplan;

  /* Create a Json object for the plan */
  std::unique_ptr<Json_object> json =
      ExplainAccessPath(path, join, /*is_root_of_join=*/true);
  if (json == nullptr) return "";

  format.ExplainPrintTreeNode(json.get(), 0, &explain,
                              &tokens_for_force_subplan);

  /* The object's token is present at the end of the token vector */
  return tokens_for_force_subplan.back();
}

/// Convert Json object to string.
string Explain_format_tree::ExplainJsonToString(Json_object *json) {
  string explain;

  vector<string> *token_ptr = nullptr;
#ifndef NDEBUG
  vector<string> tokens_for_force_subplan;
  DBUG_EXECUTE_IF("subplan_tokens", token_ptr = &tokens_for_force_subplan;);
#endif

  this->ExplainPrintTreeNode(json, 0, &explain, token_ptr);
  if (explain.empty()) return "";

  DBUG_EXECUTE_IF("subplan_tokens", {
    explain += "\nTo force this plan, use:\nSET DEBUG='+d,subplan_tokens";
    for (const string &token : tokens_for_force_subplan) {
      explain += ",force_subplan_";
      explain += token;
    }
    explain += "';\n";
  });

  return explain;
}

void Explain_format_tree::ExplainPrintTreeNode(const Json_dom *json, int level,
                                               string *explain,
                                               vector<string> *subplan_token) {
  string children_explain;
  string children_digest;

  explain->append(level * 4, ' ');

  if (json == nullptr || json->json_type() == enum_json_type::J_NULL) {
    explain->append("<not executable by iterator executor>\n");
    return;
  }

  const Json_object *obj = down_cast<const Json_object *>(json);

  AppendChildren(obj->get("inputs"), level + 1, &children_explain,
                 subplan_token, &children_digest);
  AppendChildren(obj->get("inputs_from_select_list"), level, &children_explain,
                 subplan_token, &children_digest);

  *explain += "-> ";
  if (subplan_token) {
    /*
     Include the current subplan node's token into the explain plan.
     Also append it to the subplan_token vector because parent will need it
     for generating its own subplan token.
     */
    string my_subplan_token = GetForceSubplanToken(obj, children_digest);
    *explain += '[' + my_subplan_token + "] ";
    subplan_token->push_back(my_subplan_token);
  }
  assert(obj->get("operation")->json_type() == enum_json_type::J_STRING);
  *explain += down_cast<Json_string *>(obj->get("operation"))->value();

  ExplainPrintCosts(obj, explain);

  *explain += children_explain;
}

void Explain_format_tree::ExplainPrintCosts(const Json_object *obj,
                                            string *explain) {
  bool has_first_cost = obj->get("estimated_first_row_cost") != nullptr;
  bool has_cost = obj->get("estimated_total_cost") != nullptr;

  if (has_cost) {
    double last_cost = GetJSONDouble(obj, "estimated_total_cost");
    assert(obj->get("estimated_rows") != nullptr);
    double rows = GetJSONDouble(obj, "estimated_rows");

    // NOTE: We cannot use %f, since MSVC and GCC round 0.5 in different
    // directions, so tests would not be reproducible between platforms.
    // Format/round using my_gcvt() and llrint() instead.
    char cost_as_string[FLOATING_POINT_BUFFER];
    my_fcvt(last_cost, 2, cost_as_string, /*error=*/nullptr);

    // Nominally, we only write number of rows as an integer.
    // However, if that should end up in zero, it's hard to know
    // whether that was 0.49 or 0.00001, so we add enough precision
    // to get one leading digit in that case.
    char rows_as_string[32];
    if (llrint(rows) == 0 && rows >= 1e-9) {
      snprintf(rows_as_string, sizeof(rows_as_string), "%.1g", rows);
    } else {
      snprintf(rows_as_string, sizeof(rows_as_string), "%lld", llrint(rows));
    }

    char str[1024];
    if (has_first_cost) {
      double first_row_cost = GetJSONDouble(obj, "estimated_first_row_cost");
      char first_row_cost_as_string[FLOATING_POINT_BUFFER];
      my_fcvt(first_row_cost, 2, first_row_cost_as_string, /*error=*/nullptr);
      snprintf(str, sizeof(str), "  (cost=%s..%s rows=%s)",
               first_row_cost_as_string, cost_as_string, rows_as_string);
    } else {
      snprintf(str, sizeof(str), "  (cost=%s rows=%s)", cost_as_string,
               rows_as_string);
    }

    *explain += str;
  }

  /* Show actual figures if timing info is present */
  if (obj->get("actual_rows") != nullptr) {
    if (!has_cost) {
      // We always want a double space between the iterator name and the costs.
      explain->push_back(' ');
    }
    explain->push_back(' ');

    if (obj->get("actual_rows")->json_type() == enum_json_type::J_NULL) {
      *explain += "(never executed)";
    } else {
      double actual_first_row_ms = GetJSONDouble(obj, "actual_first_row_ms");
      double actual_last_row_ms = GetJSONDouble(obj, "actual_last_row_ms");
      double actual_rows = GetJSONDouble(obj, "actual_rows");
      uint64_t actual_loops =
          down_cast<Json_int *>(obj->get("actual_loops"))->value();
      char str[1024];
      snprintf(str, sizeof(str),
               "(actual time=%.3f..%.3f rows=%lld loops=%" PRIu64 ")",
               actual_first_row_ms, actual_last_row_ms,
               llrintf(static_cast<double>(actual_rows)), actual_loops);
      *explain += str;
    }
  }
  *explain += "\n";
}

/*
  The out param 'child_token_digest' will have something like :
  ",[child1_desc:]0xchild1,[child2_desc:]0xchild2,....."
*/
void Explain_format_tree::AppendChildren(
    const Json_dom *children, int level, string *explain,
    vector<string> *tokens_for_force_subplan, string *child_token_digest) {
  if (children == nullptr) {
    return;
  }
  assert(children->json_type() == enum_json_type::J_ARRAY);
  for (const Json_dom_ptr &child : *down_cast<const Json_array *>(children)) {
    if (tokens_for_force_subplan) {
      *child_token_digest += ',';
    }
    if (child->json_type() == enum_json_type::J_OBJECT &&
        down_cast<const Json_object *>(child.get())->get("heading") !=
            nullptr) {
      string heading =
          down_cast<Json_string *>(
              down_cast<const Json_object *>(child.get())->get("heading"))
              ->value();

      /* If a token is being generated, append the child tokens */
      if (tokens_for_force_subplan) {
        *child_token_digest += heading + ":";
      }

      explain->append(level * 4, ' ');
      explain->append("-> ");
      explain->append(heading);
      explain->append("\n");
      this->ExplainPrintTreeNode(child.get(), level + 1, explain,
                                 tokens_for_force_subplan);
    } else {
      this->ExplainPrintTreeNode(child.get(), level, explain,
                                 tokens_for_force_subplan);
    }

    /* Include the child subtoken in the child digest. */
    if (tokens_for_force_subplan) {
      /* The child's token is present at the end of the token vector */
      child_token_digest->append(tokens_for_force_subplan->back());
    }
  }
}

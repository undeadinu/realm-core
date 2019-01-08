/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <new>
#include <algorithm>
#include <set>
#include <fstream>

#ifdef REALM_DEBUG
#include <iostream>
#include <iomanip>
#endif

#include <realm/util/file_mapper.hpp>
#include <realm/util/memory_stream.hpp>
#include <realm/util/miscellaneous.hpp>
#include <realm/util/thread.hpp>
#include <realm/impl/destroy_guard.hpp>
#include <realm/utilities.hpp>
#include <realm/exceptions.hpp>
#include <realm/column_linkbase.hpp>
#include <realm/column_backlink.hpp>
#include <realm/group_writer.hpp>
#include <realm/group.hpp>
#include <realm/replication.hpp>

using namespace realm;
using namespace realm::util;

namespace {

class Initialization {
public:
    Initialization()
    {
        realm::cpuid_init();
    }
};

Initialization initialization;

} // anonymous namespace


Group::Group()
    : m_alloc() // Throws
    , m_top(m_alloc)
    , m_tables(m_alloc)
    , m_table_names(m_alloc)
    , m_is_shared(false)
{
    init_array_parents();
    m_alloc.attach_empty(); // Throws
    m_file_format_version = get_target_file_format_version_for_session(0, Replication::hist_None);
    ref_type top_ref = 0; // Instantiate a new empty group
    bool create_group_when_missing = true;
    attach(top_ref, create_group_when_missing); // Throws
}


int Group::get_file_format_version() const noexcept
{
    return m_file_format_version;
}


void Group::set_file_format_version(int file_format) noexcept
{
    m_file_format_version = file_format;
}


int Group::get_committed_file_format_version() const noexcept
{
    return m_alloc.get_committed_file_format_version();
}


int Group::get_target_file_format_version_for_session(int current_file_format_version,
                                                      int requested_history_type) noexcept
{
    // Note: This function is responsible for choosing the target file format
    // for a sessions. If it selects a file format that is different from
    // `current_file_format_version`, it will trigger a file format upgrade
    // process.

    // Note: `current_file_format_version` may be zero at this time, which means
    // that the file format it is not yet decided (only possible for empty
    // Realms where top-ref is zero).

    // Please see Group::get_file_format_version() for information about the
    // individual file format versions.

    if (requested_history_type == Replication::hist_None && current_file_format_version == 6)
        return 6;

    if (requested_history_type == Replication::hist_None && current_file_format_version == 7)
        return 7;

    if (requested_history_type == Replication::hist_None && current_file_format_version == 8)
        return 8;

    return 9;
}


void Group::upgrade_file_format(int target_file_format_version)
{
    REALM_ASSERT(is_attached());

    // Be sure to revisit the following upgrade logic when a new file format
    // version is introduced. The following assert attempt to help you not
    // forget it.
    REALM_ASSERT_EX(target_file_format_version == 9, target_file_format_version);

    int current_file_format_version = get_file_format_version();
    REALM_ASSERT(current_file_format_version < target_file_format_version);

    // SharedGroup::do_open() must ensure this. Be sure to revisit the
    // following upgrade logic when SharedGroup::do_open() is changed (or
    // vice versa).
    REALM_ASSERT_EX(current_file_format_version >= 2 && current_file_format_version <= 8,
                    current_file_format_version);

    // Upgrade from version prior to 5 (datetime -> timestamp)
    if (current_file_format_version < 5) {
        for (size_t t = 0; t < m_tables.size(); t++) {
            TableRef table = get_table(t);
            table->upgrade_olddatetime();
        }
    }

    // Upgrade from version prior to 6 (StringIndex format changed last time)
    if (current_file_format_version < 6) {
        for (size_t t = 0; t < m_tables.size(); t++) {
            TableRef table = get_table(t);
            table->rebuild_search_index(current_file_format_version);
        }
    }

    // Upgrade from version prior to 7 (new history schema version in top array)
    if (current_file_format_version <= 6 && target_file_format_version >= 7) {
        // If top array size is 9, then add the missing 10th element containing
        // the history schema version.
        std::size_t top_size = m_top.size();
        REALM_ASSERT(top_size <= 9);
        if (top_size == 9) {
            int initial_history_schema_version = 0;
            m_top.add(initial_history_schema_version); // Throws
        }
    }

    // Upgrading to version 9 doesn't require changing anything.

    // NOTE: Additional future upgrade steps go here.

    set_file_format_version(target_file_format_version);
}

void Group::open(ref_type top_ref, const std::string& file_path)
{
    SlabAlloc::DetachGuard dg(m_alloc);

    // Select file format if it is still undecided.
    m_file_format_version = m_alloc.get_committed_file_format_version();

    bool file_format_ok = false;
    // In non-shared mode (Realm file opened via a Group instance) this version
    // of the core library is only able to open Realms using file format version
    // 6, 7, 8 or 9. These versions can be read without an upgrade.
    // Since a Realm file cannot be upgraded when opened in this mode
    // (we may be unable to write to the file), no earlier versions can be opened.
    // Please see Group::get_file_format_version() for information about the
    // individual file format versions.
    switch (m_file_format_version) {
        case 0:
            file_format_ok = (top_ref == 0);
            break;
        case 6:
        case 7:
        case 8:
        case 9:
            file_format_ok = true;
            break;
    }
    if (REALM_UNLIKELY(!file_format_ok))
        throw InvalidDatabase("Unsupported Realm file format version", file_path);

    Replication::HistoryType history_type = Replication::hist_None;
    int target_file_format_version = get_target_file_format_version_for_session(m_file_format_version, history_type);
    if (m_file_format_version == 0) {
        set_file_format_version(target_file_format_version);
    }
    else {
        // From a technical point of view, we could upgrade the Realm file
        // format in memory here, but since upgrading can be expensive, it is
        // currently disallowed.
        REALM_ASSERT(target_file_format_version == m_file_format_version);
    }

    // Make all dynamically allocated memory (space beyond the attached file) as
    // available free-space.
    reset_free_space_tracking(); // Throws

    bool create_group_when_missing = true;
    attach(top_ref, create_group_when_missing); // Throws
    dg.release();                               // Do not detach after all
}

void Group::open(const std::string& file_path, const char* encryption_key, OpenMode mode)
{
    if (is_attached() || m_is_shared)
        throw LogicError(LogicError::wrong_group_state);

    SlabAlloc::Config cfg;
    cfg.read_only = mode == mode_ReadOnly;
    cfg.no_create = mode == mode_ReadWriteNoCreate;
    cfg.encryption_key = encryption_key;
    ref_type top_ref = m_alloc.attach_file(file_path, cfg); // Throws

    open(top_ref, file_path);
}


void Group::open(BinaryData buffer, bool take_ownership)
{
    REALM_ASSERT(buffer.data());

    if (is_attached() || m_is_shared)
        throw LogicError(LogicError::wrong_group_state);

    ref_type top_ref = m_alloc.attach_buffer(buffer.data(), buffer.size()); // Throws

    open(top_ref, {});

    if (take_ownership)
        m_alloc.own_buffer();
}


Group::~Group() noexcept
{
    // If this group accessor is detached at this point in time, it is either
    // because it is SharedGroup::m_group (m_is_shared), or it is a free-stading
    // group accessor that was never successfully opened.
    if (!m_top.is_attached())
        return;

    // Free-standing group accessor

    detach_table_accessors();

    // Just allow the allocator to release all memory in one chunk without
    // having to traverse the entire tree first
    m_alloc.detach();
}


void Group::remap(size_t new_file_size)
{
    m_alloc.update_reader_view(new_file_size); // Throws
}


void Group::remap_and_update_refs(ref_type new_top_ref, size_t new_file_size)
{
    size_t old_baseline = m_alloc.get_baseline();

    m_alloc.update_reader_view(new_file_size); // Throws
    update_refs(new_top_ref, old_baseline);
}

void Group::validate_top_array(const Array& arr, const SlabAlloc& alloc)
{
    size_t top_size = arr.size();
    ref_type top_ref = arr.get_ref();

    switch (top_size) {
        // These are the valid sizes
        case 3:
        case 5:
        case 7:
        case 9:
        case 10: {
            ref_type table_names_ref = arr.get_as_ref_or_tagged(0).get_as_ref();
            ref_type tables_ref = arr.get_as_ref_or_tagged(1).get_as_ref();
            auto logical_file_size = arr.get_as_ref_or_tagged(2).get_as_int();

            // Logical file size must never exceed actual file size.
            // First two entries must be valid refs pointing inside the file
            auto file_size = alloc.get_baseline();
            if (logical_file_size > file_size || table_names_ref == 0 || table_names_ref > logical_file_size ||
                (table_names_ref & 7) || tables_ref == 0 || tables_ref > logical_file_size || (tables_ref & 7)) {
                std::string err = "Invalid top array (ref, [0], [1], [2]): " + util::to_string(top_ref) + ", " +
                                  util::to_string(table_names_ref) + ", " + util::to_string(tables_ref) + ", " +
                                  util::to_string(logical_file_size);
                throw InvalidDatabase(err, "");
            }
            break;
        }
        default: {
            std::string err =
                "Invalid top array (ref: " + util::to_string(top_ref) + ", size: " + util::to_string(top_size) + ")";
            throw InvalidDatabase(err, "");
            break;
        }
    }
}

void Group::attach(ref_type top_ref, bool create_group_when_missing)
{
    REALM_ASSERT(!m_top.is_attached());

    // If this function throws, it must leave the group accesor in a the
    // unattached state.

    m_tables.detach();
    m_table_names.detach();

    if (top_ref != 0) {
        m_top.init_from_ref(top_ref);
        validate_top_array(m_top, m_alloc);
        m_table_names.init_from_parent();
        m_tables.init_from_parent();
    }
    else if (create_group_when_missing) {
        create_empty_group(); // Throws
    }

    m_attached = true;

#if REALM_METRICS
    update_num_objects();
#endif // REALM_METRICS
}


void Group::detach() noexcept
{
    detach_table_accessors();
    m_table_accessors.clear();

    m_table_names.detach();
    m_tables.detach();
    m_top.detach();

    m_attached = false;
}

void Group::update_num_objects()
{
#if REALM_METRICS
    if (m_metrics) {
        // FIXME: this is quite invasive and completely defeats the lazy loading mechanism
        // where table accessors are only instantiated on demand, because they are all created here.

        m_total_rows = 0;
        size_t num_tables = size();
        for (size_t i = 0; i < num_tables; ++i) {
            ConstTableRef t = get_table(i);
            m_total_rows += t->size();
        }
    }
#endif // REALM_METRICS
}


void Group::attach_shared(ref_type new_top_ref, size_t new_file_size, bool writable)
{
    REALM_ASSERT_3(new_top_ref, <, new_file_size);
    REALM_ASSERT(!is_attached());

    // Make all dynamically allocated memory (space beyond the attached file) as
    // available free-space.
    reset_free_space_tracking(); // Throws

    // update readers view of memory
    m_alloc.update_reader_view(new_file_size); // Throws

    // When `new_top_ref` is null, ask attach() to create a new node structure
    // for an empty group, but only during the initiation of write
    // transactions. When the transaction being initiated is a read transaction,
    // we instead have to leave array accessors m_top, m_tables, and
    // m_table_names in their detached state, as there are no underlying array
    // nodes to attached them to. In the case of write transactions, the nodes
    // have to be created, as they have to be ready for being modified.
    bool create_group_when_missing = writable;
    attach(new_top_ref, create_group_when_missing); // Throws
}


void Group::detach_table_accessors() noexcept
{
    for (const auto& table_accessor : m_table_accessors) {
        if (Table* t = table_accessor) {
            typedef _impl::TableFriend tf;
            tf::detach(*t);
            tf::unbind_ptr(*t);
        }
    }
}


void Group::create_empty_group()
{
    m_top.create(Array::type_HasRefs); // Throws
    _impl::DeepArrayDestroyGuard dg_top(&m_top);
    {
        m_table_names.create(); // Throws
        _impl::DestroyGuard<ArrayString> dg(&m_table_names);
        m_top.add(m_table_names.get_ref()); // Throws
        dg.release();
    }
    {
        m_tables.create(Array::type_HasRefs); // Throws
        _impl::DestroyGuard<ArrayInteger> dg(&m_tables);
        m_top.add(m_tables.get_ref()); // Throws
        dg.release();
    }
    size_t initial_logical_file_size = sizeof(SlabAlloc::Header);
    m_top.add(RefOrTagged::make_tagged(initial_logical_file_size)); // Throws
    dg_top.release();
}


Table* Group::do_get_table(size_t table_ndx, DescMatcher desc_matcher)
{
    REALM_ASSERT(m_table_accessors.empty() || m_table_accessors.size() == m_tables.size());

    if (table_ndx >= m_tables.size())
        throw LogicError(LogicError::table_index_out_of_range);

    if (m_table_accessors.empty())
        m_table_accessors.resize(m_tables.size()); // Throws

    // Get table accessor from cache if it exists, else create
    Table* table = m_table_accessors[table_ndx];
    if (!table)
        table = create_table_accessor(table_ndx); // Throws

    if (desc_matcher) {
        typedef _impl::TableFriend tf;
        if (desc_matcher && !(*desc_matcher)(tf::get_spec(*table)))
            throw DescriptorMismatch();
    }

    return table;
}


Table* Group::do_get_table(StringData name, DescMatcher desc_matcher)
{
    if (!m_table_names.is_attached())
        return 0;
    size_t table_ndx = m_table_names.find_first(name);
    if (table_ndx == not_found)
        return 0;

    Table* table = do_get_table(table_ndx, desc_matcher); // Throws
    return table;
}


Table* Group::do_insert_table(size_t table_ndx, StringData name, DescSetter desc_setter, bool require_unique_name)
{
    if (require_unique_name && has_table(name))
        throw TableNameInUse();
    return do_insert_table(table_ndx, name, desc_setter); // Throws
}


Table* Group::do_insert_table(size_t table_ndx, StringData name, DescSetter desc_setter)
{
    if (table_ndx > m_tables.size())
        throw LogicError(LogicError::table_index_out_of_range);
    create_and_insert_table(table_ndx, name);        // Throws
    Table* table = do_get_table(table_ndx, nullptr); // Throws
    if (desc_setter)
        (*desc_setter)(*table); // Throws
    return table;
}

Table* Group::do_get_or_insert_table(size_t table_ndx, StringData name, DescMatcher desc_matcher,
                                     DescSetter desc_setter, bool* was_added)
{
    Table* table;
    size_t existing_table_ndx = m_table_names.find_first(name);
    if (existing_table_ndx == not_found) {
        table = do_insert_table(table_ndx, name, desc_setter); // Throws
        if (was_added)
            *was_added = true;
    }
    else {
        table = do_get_table(existing_table_ndx, desc_matcher); // Throws
        if (was_added)
            *was_added = false;
    }
    return table;
}


Table* Group::do_get_or_add_table(StringData name, DescMatcher desc_matcher, DescSetter desc_setter, bool* was_added)
{
    REALM_ASSERT(m_table_names.is_attached());
    Table* table;
    size_t table_ndx = m_table_names.find_first(name);
    if (table_ndx == not_found) {
        table = do_insert_table(m_tables.size(), name, desc_setter); // Throws
    }
    else {
        table = do_get_table(table_ndx, desc_matcher); // Throws
    }
    if (was_added)
        *was_added = (table_ndx == not_found);
    return table;
}


void Group::create_and_insert_table(size_t table_ndx, StringData name)
{
    if (REALM_UNLIKELY(name.size() > max_table_name_length))
        throw LogicError(LogicError::table_name_too_long);

    using namespace _impl;
    typedef TableFriend tf;
    ref_type ref = tf::create_empty_table(m_alloc); // Throws
    REALM_ASSERT_3(m_tables.size(), ==, m_table_names.size());
    size_t prior_num_tables = m_tables.size();
    m_tables.insert(table_ndx, ref);       // Throws
    m_table_names.insert(table_ndx, name); // Throws

    // Need slot for table accessor
    if (!m_table_accessors.empty()) {
        m_table_accessors.insert(m_table_accessors.begin() + table_ndx, nullptr); // Throws
    }

    update_table_indices([&](size_t old_table_ndx) {
        if (old_table_ndx >= table_ndx) {
            return old_table_ndx + 1;
        }
        return old_table_ndx;
    }); // Throws

    if (Replication* repl = m_alloc.get_replication())
        repl->insert_group_level_table(table_ndx, prior_num_tables, name); // Throws
}


Table* Group::create_table_accessor(size_t table_ndx)
{
    REALM_ASSERT(m_table_accessors.empty() || table_ndx < m_table_accessors.size());

    if (m_table_accessors.empty())
        m_table_accessors.resize(m_tables.size()); // Throws

    // Whenever a table has a link column, the column accessor must be set up to
    // refer to the target table accessor, so the target table accessor needs to
    // be created too, if it does not already exist. This, of course, applies
    // recursively, and it applies to the opposide direction of links too (from
    // target side to origin side). This means that whenever we create a table
    // accessor, we actually need to create the entire cluster of table
    // accessors, that is reachable in zero or more steps along links, or
    // backwards along links.
    //
    // To be able to do this, and to handle the cases where the link
    // relathionship graph contains cycles, each table accessor need to be
    // created in the following steps:
    //
    //  1) Create table accessor, but skip creation of column accessors
    //  2) Register incomplete table accessor in group accessor
    //  3) Mark table accessor
    //  4) Create column accessors
    //  5) Unmark table accessor
    //
    // The marking ensures that the establsihment of the connection between link
    // and backlink column accessors is postponed until both column accessors
    // are created. Infinite recursion due to cycles is prevented by the early
    // registration in the group accessor of inclomplete table accessors.

    typedef _impl::TableFriend tf;
    ref_type ref = m_tables.get_as_ref(table_ndx);
    Table* table = tf::create_incomplete_accessor(m_alloc, ref, this, table_ndx); // Throws

    // The new accessor cannot be leaked, because no exceptions can be thrown
    // before it becomes referenced from `m_column_accessors`.

    // Increase reference count from 0 to 1 to make the group accessor keep
    // the table accessor alive. This extra reference count will be revoked
    // during destruction of the group accessor.
    tf::bind_ptr(*table);

    tf::mark(*table);
    m_table_accessors[table_ndx] = table;
    tf::complete_accessor(*table); // Throws
    tf::unmark(*table);
    return table;
}


void Group::remove_table(StringData name)
{
    if (REALM_UNLIKELY(!is_attached()))
        throw LogicError(LogicError::detached_accessor);
    size_t table_ndx = m_table_names.find_first(name);
    if (table_ndx == not_found)
        throw NoSuchTable();
    remove_table(table_ndx); // Throws
}


void Group::remove_table(size_t table_ndx)
{
    if (REALM_UNLIKELY(!is_attached()))
        throw LogicError(LogicError::detached_accessor);
    REALM_ASSERT_3(m_tables.size(), ==, m_table_names.size());
    if (table_ndx >= m_tables.size())
        throw LogicError(LogicError::table_index_out_of_range);
    TableRef table = get_table(table_ndx);

    // In principle we could remove a table even if it is the target of link
    // columns of other tables, however, to do that, we would have to
    // automatically remove the "offending" link columns from those other
    // tables. Such a behaviour is deemed too obscure, and we shall therefore
    // require that a removed table does not contain foreigh origin backlink
    // columns.
    typedef _impl::TableFriend tf;
    if (tf::is_cross_table_link_target(*table))
        throw CrossTableLinkTarget();

    // There is no easy way for Group::TransactAdvancer to handle removal of
    // tables that contain foreign target table link columns, because that
    // involves removal of the corresponding backlink columns. For that reason,
    // we start by removing all columns, which will generate individual
    // replication instructions for each column removal with sufficient
    // information for Group::TransactAdvancer to handle them.
    size_t n = table->get_column_count();
    for (size_t i = n; i > 0; --i)
        table->remove_column(i - 1);

    size_t prior_num_tables = m_tables.size();
    if (Replication* repl = m_alloc.get_replication())
        repl->erase_group_level_table(table_ndx, prior_num_tables); // Throws

    int64_t ref_64 = m_tables.get(table_ndx);
    REALM_ASSERT(!int_cast_has_overflow<ref_type>(ref_64));
    ref_type ref = ref_type(ref_64);

    // Remove table and move all successive tables
    m_tables.erase(table_ndx);      // Throws
    m_table_names.erase(table_ndx); // Throws
    m_table_accessors.erase(m_table_accessors.begin() + table_ndx);

    tf::detach(*table);
    tf::unbind_ptr(*table);

    // Unless the removed table is the last, update all indices of tables after
    // the removed table.
    bool last_table_removed = table_ndx == m_tables.size();
    if (!last_table_removed) {
        update_table_indices([&](size_t old_table_ndx) {
            REALM_ASSERT(old_table_ndx != table_ndx); // We should not see links to the removed table
            if (old_table_ndx > table_ndx) {
                return old_table_ndx - 1;
            }
            return old_table_ndx;
        }); // Throws
    }

    // Destroy underlying node structure
    Array::destroy_deep(ref, m_alloc);
}


void Group::rename_table(StringData name, StringData new_name, bool require_unique_name)
{
    if (REALM_UNLIKELY(!is_attached()))
        throw LogicError(LogicError::detached_accessor);
    size_t table_ndx = m_table_names.find_first(name);
    if (table_ndx == not_found)
        throw NoSuchTable();
    rename_table(table_ndx, new_name, require_unique_name); // Throws
}


void Group::rename_table(size_t table_ndx, StringData new_name, bool require_unique_name)
{
    if (REALM_UNLIKELY(!is_attached()))
        throw LogicError(LogicError::detached_accessor);
    REALM_ASSERT_3(m_tables.size(), ==, m_table_names.size());
    if (table_ndx >= m_tables.size())
        throw LogicError(LogicError::table_index_out_of_range);
    if (require_unique_name && has_table(new_name))
        throw TableNameInUse();
    m_table_names.set(table_ndx, new_name);
    if (Replication* repl = m_alloc.get_replication())
        repl->rename_group_level_table(table_ndx, new_name); // Throws
}


class Group::DefaultTableWriter : public Group::TableWriter {
public:
    DefaultTableWriter(const Group& group)
        : m_group(group)
    {
    }
    ref_type write_names(_impl::OutputStream& out) override
    {
        bool deep = true;                                                // Deep
        bool only_if_modified = false;                                   // Always
        return m_group.m_table_names.write(out, deep, only_if_modified); // Throws
    }
    ref_type write_tables(_impl::OutputStream& out) override
    {
        bool deep = true;                                           // Deep
        bool only_if_modified = false;                              // Always
        return m_group.m_tables.write(out, deep, only_if_modified); // Throws
    }

    HistoryInfo write_history(_impl::OutputStream& out) override
    {
        bool deep = true;                                           // Deep
        bool only_if_modified = false;                              // Always
        ref_type history_ref = _impl::GroupFriend::get_history_ref(m_group);
        HistoryInfo info;
        if (history_ref) {
            _impl::History::version_type version;
            int history_type, history_schema_version;
            _impl::GroupFriend::get_version_and_history_info(_impl::GroupFriend::get_alloc(m_group),
                                                             m_group.m_top.get_ref(),
                                                             version,
                                                             history_type,
                                                             history_schema_version);
            REALM_ASSERT(history_type != Replication::hist_None);
            if (history_type != Replication::hist_SyncClient && history_type != Replication::hist_SyncServer) {
                return info; // Only sync history should be preserved when writing to a new file
            }
            info.type = history_type;
            info.version = history_schema_version;
            // FIXME: It's ugly that we have to instantiate a new array here,
            // but it isn't obvious that Group should have history as a member.
            Array history{const_cast<Allocator&>(_impl::GroupFriend::get_alloc(m_group))};
            history.init_from_ref(history_ref);
            info.ref = history.write(out, deep, only_if_modified); // Throws
        }
        return info;
    }

private:
    const Group& m_group;
};

void Group::write(std::ostream& out, bool pad) const
{
    write(out, pad, 0);
}

void Group::write(std::ostream& out, bool pad_for_encryption, uint_fast64_t version_number) const
{
    REALM_ASSERT(is_attached());
    DefaultTableWriter table_writer(*this);
    bool no_top_array = !m_top.is_attached();
    write(out, m_file_format_version, table_writer, no_top_array, pad_for_encryption, version_number); // Throws
}

void Group::write(const std::string& path, const char* encryption_key, uint64_t version_number) const
{
    File file;
    int flags = 0;
    file.open(path, File::access_ReadWrite, File::create_Must, flags);
    write(file, encryption_key, version_number);
}

void Group::write(File& file, const char* encryption_key, uint_fast64_t version_number) const
{
    REALM_ASSERT(file.get_size() == 0);

    file.set_encryption_key(encryption_key);
    File::Streambuf streambuf(&file);
    std::ostream out(&streambuf);
    out.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    write(out, encryption_key != 0, version_number);
    int sync_status = streambuf.pubsync();
    REALM_ASSERT(sync_status == 0);
}

BinaryData Group::write_to_mem() const
{
    REALM_ASSERT(is_attached());

    // Get max possible size of buffer
    //
    // FIXME: This size could potentially be vastly bigger that what
    // is actually needed.
    size_t max_size = m_alloc.get_total_size();

    char* buffer = static_cast<char*>(malloc(max_size)); // Throws
    if (!buffer)
        throw util::bad_alloc();
    try {
        MemoryOutputStream out; // Throws
        out.set_buffer(buffer, buffer + max_size);
        write(out); // Throws
        size_t buffer_size = out.size();
        return BinaryData(buffer, buffer_size);
    }
    catch (...) {
        free(buffer);
        throw;
    }
}


void Group::write(std::ostream& out, int file_format_version, TableWriter& table_writer, bool no_top_array,
                  bool pad_for_encryption, uint_fast64_t version_number)
{
    _impl::OutputStream out_2(out);

    // Write the file header
    SlabAlloc::Header streaming_header;
    if (no_top_array) {
        file_format_version = 0;
    }
    else if (file_format_version == 0) {
        // Use current file format version
        file_format_version = get_target_file_format_version_for_session(0, Replication::hist_None);
    }
    SlabAlloc::init_streaming_header(&streaming_header, file_format_version);
    out_2.write(reinterpret_cast<const char*>(&streaming_header), sizeof streaming_header);

    ref_type top_ref = 0;
    size_t final_file_size = sizeof streaming_header;
    if (no_top_array) {
        // Accept version number 1 as that number is (unfortunately) also used
        // to denote the empty initial state of a Realm file.
        REALM_ASSERT(version_number == 0 || version_number == 1);
    }
    else {
        // Because we need to include the total logical file size in the
        // top-array, we have to start by writing everything except the
        // top-array, and then finally compute and write a correct version of
        // the top-array. The free-space information of the group will only be
        // included if a non-zero version number is given as parameter,
        // indicating that versioning info is to be saved. This is used from
        // SharedGroup to compact the database by writing only the live data
        // into a separate file.
        ref_type names_ref = table_writer.write_names(out_2);   // Throws
        ref_type tables_ref = table_writer.write_tables(out_2); // Throws
        TableWriter::HistoryInfo history_info = table_writer.write_history(out_2); // Throws
        SlabAlloc new_alloc;
        new_alloc.attach_empty(); // Throws
        Array top(new_alloc);
        top.create(Array::type_HasRefs); // Throws
        _impl::ShallowArrayDestroyGuard dg_top(&top);
        // FIXME: We really need an alternative to Array::truncate() that is able to expand.
        int_fast64_t value_1 = from_ref(names_ref);
        int_fast64_t value_2 = from_ref(tables_ref);
        top.add(value_1); // Throws
        top.add(value_2); // Throws
        top.add(0);       // Throws

        int top_size = 3;
        if (version_number) {
            Array free_list(new_alloc);
            Array size_list(new_alloc);
            Array version_list(new_alloc);
            free_list.create(Array::type_Normal); // Throws
            _impl::DeepArrayDestroyGuard dg_1(&free_list);
            size_list.create(Array::type_Normal); // Throws
            _impl::DeepArrayDestroyGuard dg_2(&size_list);
            version_list.create(Array::type_Normal); // Throws
            _impl::DeepArrayDestroyGuard dg_3(&version_list);
            bool deep = true;              // Deep
            bool only_if_modified = false; // Always
            ref_type free_list_ref = free_list.write(out_2, deep, only_if_modified);
            ref_type size_list_ref = size_list.write(out_2, deep, only_if_modified);
            ref_type version_list_ref = version_list.write(out_2, deep, only_if_modified);
            top.add(RefOrTagged::make_ref(free_list_ref));     // Throws
            top.add(RefOrTagged::make_ref(size_list_ref));     // Throws
            top.add(RefOrTagged::make_ref(version_list_ref));  // Throws
            top.add(RefOrTagged::make_tagged(version_number)); // Throws
            top_size = 7;

            if (history_info.type != Replication::hist_None) {
                top.add(RefOrTagged::make_tagged(history_info.type));
                top.add(RefOrTagged::make_ref(history_info.ref));
                top.add(RefOrTagged::make_tagged(history_info.version));
                top_size = 10;
            }
        }
        top_ref = out_2.get_ref_of_next_array();

        // Produce a preliminary version of the top array whose
        // representation is guaranteed to be able to hold the final file
        // size
        size_t max_top_byte_size = Array::get_max_byte_size(top_size);
        size_t max_final_file_size = size_t(top_ref) + max_top_byte_size;
        top.ensure_minimum_width(RefOrTagged::make_tagged(max_final_file_size)); // Throws

        // Finalize the top array by adding the projected final file size
        // to it
        size_t top_byte_size = top.get_byte_size();
        final_file_size = size_t(top_ref) + top_byte_size;
        top.set(2, RefOrTagged::make_tagged(final_file_size)); // Throws

        // Write the top array
        bool deep = false;                        // Shallow
        bool only_if_modified = false;            // Always
        top.write(out_2, deep, only_if_modified); // Throws
        REALM_ASSERT_3(size_t(out_2.get_ref_of_next_array()), ==, final_file_size);

        dg_top.reset(nullptr); // Destroy now
    }

    // encryption will pad the file to a multiple of the page, so ensure the
    // footer is aligned to the end of a page
    if (pad_for_encryption) {
#if REALM_ENABLE_ENCRYPTION
        size_t unrounded_size = final_file_size + sizeof(SlabAlloc::StreamingFooter);
        size_t rounded_size = round_up_to_page_size(unrounded_size);
        if (rounded_size != unrounded_size) {
            std::unique_ptr<char[]> buffer(new char[rounded_size - unrounded_size]());
            out_2.write(buffer.get(), rounded_size - unrounded_size);
        }
#endif
    }

    // Write streaming footer
    SlabAlloc::StreamingFooter footer;
    footer.m_top_ref = top_ref;
    footer.m_magic_cookie = SlabAlloc::footer_magic_cookie;
    out_2.write(reinterpret_cast<const char*>(&footer), sizeof footer);
}


void Group::commit()
{
    if (!is_attached())
        throw LogicError(LogicError::detached_accessor);
    if (m_is_shared)
        throw LogicError(LogicError::wrong_group_state);

    GroupWriter out(*this); // Throws

    // Recursively write all changed arrays to the database file. We
    // postpone the commit until we are sure that no exceptions can be
    // thrown.
    ref_type top_ref = out.write_group(); // Throws

    // Since the group is persisiting in single-thread (unshared)
    // mode we have to make sure that the group stays valid after
    // commit

    // Mark all managed space (beyond the attached file) as free.
    reset_free_space_tracking(); // Throws

    size_t old_baseline = m_alloc.get_baseline();

    // Update view of the file
    size_t new_file_size = out.get_file_size();
    m_alloc.update_reader_view(new_file_size); // Throws

    out.commit(top_ref); // Throws

    // Recursively update refs in all active tables (columns, arrays..)
    update_refs(top_ref, old_baseline);
}


void Group::update_refs(ref_type top_ref, size_t old_baseline) noexcept
{
    old_baseline = 0; // force update of all refs
    // After Group::commit() we will always have free space tracking
    // info.
    REALM_ASSERT_3(m_top.size(), >=, 5);

    // Array nodes that are part of the previous version of the
    // database will not be overwritten by Group::commit(). This is
    // necessary for robustness in the face of abrupt termination of
    // the process. It also means that we can be sure that an array
    // remains unchanged across a commit if the new ref is equal to
    // the old ref and the ref is below the previous baseline.

    if (top_ref < old_baseline && m_top.get_ref() == top_ref)
        return;

    m_top.init_from_ref(top_ref);

    // Now we can update it's child arrays
    m_table_names.update_from_parent(old_baseline);

    // If m_tables has not been modfied we don't
    // need to update attached table accessors
    if (!m_tables.update_from_parent(old_baseline))
        return;

    // Update all attached table accessors including those attached to
    // subtables.
    for (const auto& table_accessor : m_table_accessors) {
        typedef _impl::TableFriend tf;
        if (Table* table = table_accessor)
            tf::update_from_parent(*table, old_baseline);
    }
}


bool Group::operator==(const Group& g) const
{
    size_t n = size();
    if (n != g.size())
        return false;
    for (size_t i = 0; i < n; ++i) {
        const StringData& table_name_1 = get_table_name(i);   // Throws
        const StringData& table_name_2 = g.get_table_name(i); // Throws
        if (table_name_1 != table_name_2)
            return false;

        ConstTableRef table_1 = get_table(i);   // Throws
        ConstTableRef table_2 = g.get_table(i); // Throws
        if (*table_1 != *table_2)
            return false;
    }
    return true;
}

namespace {

size_t size_of_tree_from_ref(ref_type ref, Allocator& alloc) {
    if (ref) {
        Array a(alloc);
        a.init_from_ref(ref);
        MemStats stats;
        a.stats(stats);
        return stats.allocated;
    }
    else
        return 0;
}

}

size_t Group::compute_aggregated_byte_size(SizeAggregateControl ctrl) const noexcept
{
    SlabAlloc& alloc = *const_cast<SlabAlloc*>(&m_alloc);
    if (!m_top.is_attached())
        return 0;
    size_t used = 0;
    if (ctrl & SizeAggregateControl::size_of_state) {
        MemStats stats;
        m_table_names.stats(stats);
        m_tables.stats(stats);
        used = stats.allocated + m_top.get_byte_size();
        used += sizeof(SlabAlloc::Header);
    }
    if (ctrl & SizeAggregateControl::size_of_freelists) {
        if (m_top.size() >= 6) {
            auto ref = m_top.get_as_ref_or_tagged(3).get_as_ref();
            used += size_of_tree_from_ref(ref, alloc);
            ref = m_top.get_as_ref_or_tagged(4).get_as_ref();
            used += size_of_tree_from_ref(ref, alloc);
            ref = m_top.get_as_ref_or_tagged(5).get_as_ref();
            used += size_of_tree_from_ref(ref, alloc);
        }
    }
    if (ctrl & SizeAggregateControl::size_of_history) {
        if (m_top.size() >= 9) {
            auto ref = m_top.get_as_ref_or_tagged(8).get_as_ref();
            used += size_of_tree_from_ref(ref, alloc);
        }
    }
    return used;
}

size_t Group::get_used_space() const noexcept
{
    if (!m_top.is_attached())
        return 0;

    size_t used_space = (size_t(m_top.get(2)) >> 1);

    if (m_top.size() > 4) {
        Array free_lengths(const_cast<SlabAlloc&>(m_alloc));
        free_lengths.init_from_ref(ref_type(m_top.get(4)));
        used_space -= size_t(free_lengths.sum());
    }

    return used_space;
}


void Group::to_string(std::ostream& out) const
{
    // Calculate widths
    size_t index_width = 4;
    size_t name_width = 10;
    size_t rows_width = 6;
    size_t count = size();
    for (size_t i = 0; i < count; ++i) {
        StringData name = get_table_name(i);
        if (name_width < name.size())
            name_width = name.size();

        ConstTableRef table = get_table(name);
        size_t row_count = table->size();
        if (rows_width < row_count) { // FIXME: should be the number of digits in row_count: floor(log10(row_count+1))
            rows_width = row_count;
        }
    }


    // Print header
    out << std::setw(int(index_width + 1)) << std::left << " ";
    out << std::setw(int(name_width + 1)) << std::left << "tables";
    out << std::setw(int(rows_width)) << std::left << "rows" << std::endl;

    // Print tables
    for (size_t i = 0; i < count; ++i) {
        StringData name = get_table_name(i);
        ConstTableRef table = get_table(name);
        size_t row_count = table->size();

        out << std::setw(int(index_width)) << std::right << i << " ";
        out << std::setw(int(name_width)) << std::left << std::string(name) << " ";
        out << std::setw(int(rows_width)) << std::left << row_count << std::endl;
    }
}


void Group::mark_all_table_accessors() noexcept
{
    size_t num_tables = m_table_accessors.size();
    for (size_t table_ndx = 0; table_ndx != num_tables; ++table_ndx) {
        if (Table* table = m_table_accessors[table_ndx]) {
            typedef _impl::TableFriend tf;
            tf::recursive_mark(*table); // Also all subtable accessors
        }
    }
}


namespace {

class MarkDirtyUpdater : public _impl::TableFriend::AccessorUpdater {
public:
    void update(Table& table) override
    {
        typedef _impl::TableFriend tf;
        tf::mark(table);
    }

    void update_parent(Table& table) override
    {
        typedef _impl::TableFriend tf;
        tf::mark(table);
    }

    size_t m_col_ndx;
    DataType m_type;
};


class InsertColumnUpdater : public _impl::TableFriend::AccessorUpdater {
public:
    InsertColumnUpdater(size_t col_ndx)
        : m_col_ndx(col_ndx)
    {
    }

    void update(Table& table) override
    {
        typedef _impl::TableFriend tf;
        tf::adj_insert_column(table, m_col_ndx); // Throws
        tf::mark_link_target_tables(table, m_col_ndx + 1);
    }

    void update_parent(Table&) override
    {
    }

private:
    size_t m_col_ndx;
};


class EraseColumnUpdater : public _impl::TableFriend::AccessorUpdater {
public:
    EraseColumnUpdater(size_t col_ndx)
        : m_col_ndx(col_ndx)
    {
    }

    void update(Table& table) override
    {
        typedef _impl::TableFriend tf;
        tf::adj_erase_column(table, m_col_ndx);
        tf::mark_link_target_tables(table, m_col_ndx);
    }

    void update_parent(Table&) override
    {
    }

private:
    size_t m_col_ndx;
};

} // anonymous namespace


// In general, this class cannot assume more than minimal accessor consistency
// (See AccessorConsistencyLevels., it can however assume that replication
// instruction arguments are meaningfull with respect to the current state of
// the accessor hierarchy. For example, a column index argument of `i` is known
// to refer to the `i`'th entry of Table::m_cols.
//
// FIXME: There is currently no checking on valid instruction arguments such as
// column index within bounds. Consider whether we can trust the contents of the
// transaction log enough to skip these checks.
class Group::TransactAdvancer {
public:
    TransactAdvancer(Group& group, bool& schema_changed)
        : m_group(group)
        , m_schema_changed(schema_changed)
    {
    }

    bool insert_group_level_table(size_t table_ndx, size_t num_tables, StringData) noexcept
    {
        REALM_ASSERT_3(table_ndx, <=, num_tables);
        REALM_ASSERT(m_group.m_table_accessors.empty() || m_group.m_table_accessors.size() == num_tables);

        if (!m_group.m_table_accessors.empty()) {
            m_group.m_table_accessors.insert(m_group.m_table_accessors.begin() + table_ndx, nullptr);
            for (size_t i = table_ndx + 1; i < m_group.m_table_accessors.size(); ++i) {
                if (Table* moved_table = m_group.m_table_accessors[i]) {
                    typedef _impl::TableFriend tf;
                    tf::mark(*moved_table);
                    tf::mark_opposite_link_tables(*moved_table);
                }
            }
        }

        m_schema_changed = true;

        return true;
    }

    bool erase_group_level_table(size_t table_ndx, size_t num_tables) noexcept
    {
        REALM_ASSERT_3(table_ndx, <, num_tables);
        REALM_ASSERT(m_group.m_table_accessors.empty() || m_group.m_table_accessors.size() == num_tables);

        if (!m_group.m_table_accessors.empty()) {
            // Link target tables do not need to be considered here, since all
            // columns will already have been removed at this point.
            if (Table* table = m_group.m_table_accessors[table_ndx]) {
                typedef _impl::TableFriend tf;
                tf::detach(*table);
                tf::unbind_ptr(*table);
            }

            m_group.m_table_accessors.erase(m_group.m_table_accessors.begin() + table_ndx);
            for (size_t i = table_ndx; i < m_group.m_table_accessors.size(); ++i) {
                if (Table* moved_table = m_group.m_table_accessors[i]) {
                    typedef _impl::TableFriend tf;
                    tf::mark(*moved_table);
                    tf::mark_opposite_link_tables(*moved_table);
                }
            }
        }

        m_schema_changed = true;

        return true;
    }

    bool rename_group_level_table(size_t, StringData) noexcept
    {
        // No-op since table names are properties of the group, and the group
        // accessor is always refreshed
        m_schema_changed = true;
        return true;
    }

    bool select_table(size_t group_level_ndx, int levels, const size_t* path) noexcept
    {
        m_table.reset();
        // The list of table accessors must either be empty or correctly reflect
        // the number of tables prior to this instruction (see
        // Group::do_get_table()). An empty list means that no table accessors
        // have been created yet (all entries are null).
        REALM_ASSERT(m_group.m_table_accessors.empty() || group_level_ndx < m_group.m_table_accessors.size());
        if (group_level_ndx < m_group.m_table_accessors.size()) {
            TableRef table(m_group.m_table_accessors[group_level_ndx]);
            if (table) {
                const size_t* path_begin = path;
                const size_t* path_end = path_begin + 2 * levels;
                for (;;) {
                    typedef _impl::TableFriend tf;
                    tf::mark(*table);
                    if (path_begin == path_end) {
                        m_table = std::move(table);
                        break;
                    }
                    size_t col_ndx = path_begin[0];
                    size_t row_ndx = path_begin[1];
                    table = tf::get_subtable_accessor(*table, col_ndx, row_ndx);
                    if (!table)
                        break;
                    path_begin += 2;
                }
            }
        }
        return true;
    }

    bool insert_empty_rows(size_t row_ndx, size_t num_rows_to_insert, size_t prior_num_rows, bool unordered) noexcept
    {
        typedef _impl::TableFriend tf;
        if (m_table) {
            if (unordered) {
                // Unordered insertion of multiple rows is not yet supported (and not
                // yet needed).
                REALM_ASSERT_EX((num_rows_to_insert == 1) || (num_rows_to_insert == 0), num_rows_to_insert);
                size_t from_row_ndx = row_ndx;
                size_t to_row_ndx = prior_num_rows;
                tf::adj_acc_move_over(*m_table, from_row_ndx, to_row_ndx);
            }
            else {
                tf::adj_acc_insert_rows(*m_table, row_ndx, num_rows_to_insert);
            }
        }
        return true;
    }

    bool add_row_with_key(size_t, size_t, size_t, int64_t) noexcept
    {
        return true;
    }

    bool erase_rows(size_t row_ndx, size_t num_rows_to_erase, size_t prior_num_rows, bool unordered) noexcept
    {
        if (unordered) {
            // Unordered removal of multiple rows is not yet supported (and not
            // yet needed).
            REALM_ASSERT_EX((num_rows_to_erase == 1) || (num_rows_to_erase == 0), num_rows_to_erase);
            typedef _impl::TableFriend tf;
            if (m_table) {
                size_t prior_last_row_ndx = prior_num_rows - 1;
                tf::adj_acc_move_over(*m_table, prior_last_row_ndx, row_ndx);
            }
        }
        else {
            typedef _impl::TableFriend tf;
            if (m_table) {
                // Linked tables must still be marked for accessor updates in the case
                // where num_rows_to_erase == 0. Without doing this here it wouldn't be done
                // at all because the contents of the for loop do not get executed.
                if (num_rows_to_erase == 0) {
                    tf::mark_opposite_link_tables(*m_table);
                }
                else {
                    for (size_t i = 0; i < num_rows_to_erase; ++i)
                        tf::adj_acc_erase_row(*m_table, row_ndx + num_rows_to_erase - 1 - i);
                }
            }
        }
        return true;
    }

    bool swap_rows(size_t row_ndx_1, size_t row_ndx_2) noexcept
    {
        using tf = _impl::TableFriend;
        if (m_table)
            tf::adj_acc_swap_rows(*m_table, row_ndx_1, row_ndx_2);
        return true;
    }

    bool move_row(size_t from_ndx, size_t to_ndx) noexcept
    {
        using tf = _impl::TableFriend;
        if (m_table)
            tf::adj_acc_move_row(*m_table, from_ndx, to_ndx);
        return true;
    }

    bool merge_rows(size_t row_ndx, size_t new_row_ndx) noexcept
    {
        typedef _impl::TableFriend tf;
        if (m_table)
            tf::adj_acc_merge_rows(*m_table, row_ndx, new_row_ndx);
        return true;
    }

    bool clear_table(size_t) noexcept
    {
        typedef _impl::TableFriend tf;
        if (m_table)
            tf::adj_acc_clear_root_table(*m_table);
        return true;
    }

    bool set_int(size_t, size_t, int_fast64_t, _impl::Instruction, size_t) noexcept
    {
        return true; // No-op
    }

    bool add_int(size_t, size_t, int_fast64_t) noexcept
    {
        return true; // No-op
    }

    bool set_bool(size_t, size_t, bool, _impl::Instruction) noexcept
    {
        return true; // No-op
    }

    bool set_float(size_t, size_t, float, _impl::Instruction) noexcept
    {
        return true; // No-op
    }

    bool set_double(size_t, size_t, double, _impl::Instruction) noexcept
    {
        return true; // No-op
    }

    bool set_string(size_t, size_t, StringData, _impl::Instruction, size_t) noexcept
    {
        return true; // No-op
    }

    bool set_binary(size_t, size_t, BinaryData, _impl::Instruction) noexcept
    {
        return true; // No-op
    }

    bool set_olddatetime(size_t, size_t, OldDateTime, _impl::Instruction) noexcept
    {
        return true; // No-op
    }

    bool set_timestamp(size_t, size_t, Timestamp, _impl::Instruction) noexcept
    {
        return true; // No-op
    }

    bool set_table(size_t col_ndx, size_t row_ndx, _impl::Instruction) noexcept
    {
        if (m_table) {
            typedef _impl::TableFriend tf;
            TableRef subtab(tf::get_subtable_accessor(*m_table, col_ndx, row_ndx));
            if (subtab) {
                tf::mark(*subtab);
                tf::adj_acc_clear_nonroot_table(*subtab);
            }
        }
        return true;
    }

    bool set_mixed(size_t col_ndx, size_t row_ndx, const Mixed&, _impl::Instruction) noexcept
    {
        typedef _impl::TableFriend tf;
        if (m_table)
            tf::discard_subtable_accessor(*m_table, col_ndx, row_ndx);
        return true;
    }

    bool set_null(size_t, size_t, _impl::Instruction, size_t) noexcept
    {
        return true; // No-op
    }

    bool set_link(size_t col_ndx, size_t, size_t, size_t, _impl::Instruction) noexcept
    {
        // When links are changed, the link-target table is also affected and
        // its accessor must therefore be marked dirty too. Indeed, when it
        // exists, the link-target table accessor must be marked dirty
        // regardless of whether an accessor exists for the origin table (i.e.,
        // regardless of whether `m_table` is null or not.) This would seem to
        // pose a problem, because there is no easy way to identify the
        // link-target table when there is no accessor for the origin
        // table. Fortunately, due to the fact that back-link column accessors
        // refer to the origin table accessor (and vice versa), it follows that
        // the link-target table accessor exists if, and only if the origin
        // table accessor exists.
        //
        // get_link_target_table_accessor() will return null if the
        // m_table->m_cols[col_ndx] is null, but this can happen only when the
        // column was inserted earlier during this transaction advance, and in
        // that case, we have already marked the target table accessor dirty.

        if (m_table) {
            using tf = _impl::TableFriend;
            if (Table* target = tf::get_link_target_table_accessor(*m_table, col_ndx))
                tf::mark(*target);
        }
        return true;
    }

    bool insert_substring(size_t, size_t, size_t, StringData)
    {
        return true; // No-op
    }

    bool erase_substring(size_t, size_t, size_t, size_t)
    {
        return true; // No-op
    }

    bool optimize_table() noexcept
    {
        return true; // No-op
    }

    bool select_descriptor(int levels, const size_t* path)
    {
        m_desc.reset();
        if (m_table) {
            REALM_ASSERT(!m_table->has_shared_type());
            typedef _impl::TableFriend tf;
            DescriptorRef desc = tf::get_root_table_desc_accessor(*m_table);
            int i = 0;
            while (desc) {
                if (i >= levels) {
                    m_desc = desc;
                    break;
                }
                typedef _impl::DescriptorFriend df;
                size_t col_ndx = path[i];
                desc = df::get_subdesc_accessor(*desc, col_ndx);
                ++i;
            }
            m_desc_path_begin = path;
            m_desc_path_end = path + levels;
            MarkDirtyUpdater updater;
            tf::update_accessors(*m_table, m_desc_path_begin, m_desc_path_end, updater);
        }
        return true;
    }

    bool insert_column(size_t col_ndx, DataType, StringData, bool nullable)
    {
        static_cast<void>(nullable);
        if (m_table) {
            typedef _impl::TableFriend tf;
            InsertColumnUpdater updater(col_ndx);
            tf::update_accessors(*m_table, m_desc_path_begin, m_desc_path_end, updater);
        }
        typedef _impl::DescriptorFriend df;
        if (m_desc)
            df::adj_insert_column(*m_desc, col_ndx);

        m_schema_changed = true;

        return true;
    }

    bool insert_link_column(size_t col_ndx, DataType, StringData, size_t link_target_table_ndx,
                            size_t backlink_column_ndx)
    {
        if (m_table) {
            InsertColumnUpdater updater(col_ndx);
            using tf = _impl::TableFriend;
            tf::update_accessors(*m_table, m_desc_path_begin, m_desc_path_end, updater);
        }
        // Since insertion of a link column also modifies the target table by
        // adding a backlink column there, the target table accessor needs to be
        // marked dirty if it exists. Normally, the target table accesssor
        // exists if, and only if the origin table accessor exists, but during
        // Group::advance_transact() there will be times where this is not the
        // case. Only after the final phase that updates all dirty accessors
        // will this be guaranteed to be true again. See also the comments on
        // link handling in TransactAdvancer::set_link().
        if (link_target_table_ndx < m_group.m_table_accessors.size()) {
            if (Table* target = m_group.m_table_accessors[link_target_table_ndx]) {
                using tf = _impl::TableFriend;
                tf::adj_insert_column(*target, backlink_column_ndx); // Throws
                tf::mark(*target);
            }
        }
        if (m_desc) {
            using df = _impl::DescriptorFriend;
            df::adj_insert_column(*m_desc, col_ndx);
        }

        m_schema_changed = true;

        return true;
    }

    bool erase_column(size_t col_ndx)
    {
        if (m_table) {
            typedef _impl::TableFriend tf;
            EraseColumnUpdater updater(col_ndx);
            tf::update_accessors(*m_table, m_desc_path_begin, m_desc_path_end, updater);
        }
        typedef _impl::DescriptorFriend df;
        if (m_desc)
            df::adj_erase_column(*m_desc, col_ndx);

        m_schema_changed = true;

        return true;
    }

    bool erase_link_column(size_t col_ndx, size_t link_target_table_ndx, size_t backlink_col_ndx)
    {
        // For link columns we need to handle the backlink column first in case
        // the target table is the same as the origin table (because the
        // backlink column occurs after regular columns.)
        //
        // Please also see comments on special handling of link columns in
        // TransactAdvancer::insert_link_column() and
        // TransactAdvancer::set_link().
        if (link_target_table_ndx < m_group.m_table_accessors.size()) {
            if (Table* target = m_group.m_table_accessors[link_target_table_ndx]) {
                using tf = _impl::TableFriend;
                tf::adj_erase_column(*target, backlink_col_ndx); // Throws
                tf::mark(*target);
            }
        }
        if (m_table) {
            EraseColumnUpdater updater(col_ndx);
            using tf = _impl::TableFriend;
            tf::update_accessors(*m_table, m_desc_path_begin, m_desc_path_end, updater);
        }
        if (m_desc) {
            using df = _impl::DescriptorFriend;
            df::adj_erase_column(*m_desc, col_ndx);
        }

        m_schema_changed = true;

        return true;
    }

    bool rename_column(size_t, StringData) noexcept
    {
        m_schema_changed = true;
        return true; // No-op
    }

    bool add_search_index(size_t) noexcept
    {
        return true; // No-op
    }

    bool remove_search_index(size_t) noexcept
    {
        return true; // No-op
    }

    bool add_primary_key(size_t) noexcept
    {
        return true; // No-op
    }

    bool remove_primary_key() noexcept
    {
        return true; // No-op
    }

    bool set_link_type(size_t, LinkType) noexcept
    {
        return true; // No-op
    }

    bool select_link_list(size_t col_ndx, size_t, size_t) noexcept
    {
        // See comments on link handling in TransactAdvancer::set_link().
        typedef _impl::TableFriend tf;
        if (m_table) {
            if (Table* target = tf::get_link_target_table_accessor(*m_table, col_ndx))
                tf::mark(*target);
        }
        return true; // No-op
    }

    bool link_list_set(size_t, size_t, size_t) noexcept
    {
        return true; // No-op
    }

    bool link_list_insert(size_t, size_t, size_t) noexcept
    {
        return true; // No-op
    }

    bool link_list_move(size_t, size_t) noexcept
    {
        return true; // No-op
    }

    bool link_list_swap(size_t, size_t) noexcept
    {
        return true; // No-op
    }

    bool link_list_erase(size_t, size_t) noexcept
    {
        return true; // No-op
    }

    bool link_list_clear(size_t) noexcept
    {
        return true; // No-op
    }

    bool nullify_link(size_t, size_t, size_t)
    {
        return true; // No-op
    }

    bool link_list_nullify(size_t, size_t)
    {
        return true; // No-op
    }

private:
    Group& m_group;
    TableRef m_table;
    DescriptorRef m_desc;
    const size_t* m_desc_path_begin;
    const size_t* m_desc_path_end;
    bool& m_schema_changed;
};

void Group::refresh_dirty_accessors()
{
    m_top.get_alloc().bump_global_version();

    // Refresh all remaining dirty table accessors
    size_t num_tables = m_table_accessors.size();
    for (size_t table_ndx = 0; table_ndx != num_tables; ++table_ndx) {
        if (Table* table = m_table_accessors[table_ndx]) {
            typedef _impl::TableFriend tf;
            tf::set_ndx_in_parent(*table, table_ndx);
            if (tf::is_marked(*table)) {
                tf::refresh_accessor_tree(*table); // Throws
                bool bump_global = false;
                tf::bump_version(*table, bump_global);
            }
        }
    }
}


template <class F>
void Group::update_table_indices(F&& map_function)
{
    using tf = _impl::TableFriend;

    // Update any link columns.
    for (size_t i = 0; i < m_tables.size(); ++i) {
        Array table_top{m_alloc};
        Spec dummy_spec{m_alloc};
        Spec* spec = &dummy_spec;

        // Ensure that we use spec objects in potential table accessors
        Table* table = m_table_accessors.empty() ? nullptr : m_table_accessors[i];
        if (table) {
            spec = &tf::get_spec(*table);
            table->set_ndx_in_parent(i);
        }
        else {
            table_top.set_parent(&m_tables, i);
            table_top.init_from_parent();
            dummy_spec.set_parent(&table_top, 0); // Spec has index 0 in table top
            dummy_spec.init_from_parent();
        }

        size_t num_cols = spec->get_column_count();
        bool spec_changed = false;
        for (size_t col_ndx = 0; col_ndx < num_cols; ++col_ndx) {
            ColumnType type = spec->get_column_type(col_ndx);
            if (tf::is_link_type(type) || type == col_type_BackLink) {
                size_t table_ndx = spec->get_opposite_link_table_ndx(col_ndx);
                size_t new_table_ndx = map_function(table_ndx);
                if (new_table_ndx != table_ndx) {
                    spec->set_opposite_link_table_ndx(col_ndx, new_table_ndx); // Throws
                    spec_changed = true;
                }
            }
        }

        if (spec_changed && table) {
            tf::mark(*table);
        }
    }

    // Update accessors.
    refresh_dirty_accessors(); // Throws
}


void Group::advance_transact(ref_type new_top_ref, size_t new_file_size, _impl::NoCopyInputStream& in)
{
    REALM_ASSERT(is_attached());

    // Exception safety: If this function throws, the group accessor and all of
    // its subordinate accessors are left in a state that may not be fully
    // consistent. Only minimal consistency is guaranteed (see
    // AccessorConsistencyLevels). In this case, the application is required to
    // either destroy the Group object, forcing all subordinate accessors to
    // become detached, or take some other equivalent action that involves a
    // call to Group::detach(), such as terminating the transaction in progress.
    // such actions will also lead to the detachment of all subordinate
    // accessors. Until then it is an error, and unsafe if the application
    // attempts to access the group one of its subordinate accessors.
    //
    //
    // The purpose of this function is to refresh all attached accessors after
    // the underlying node structure has undergone arbitrary change, such as
    // when a read transaction has been advanced to a later snapshot of the
    // database.
    //
    // Initially, when this function is invoked, we cannot assume any
    // correspondance between the accessor state and the underlying node
    // structure. We can assume that the hierarchy is in a state of minimal
    // consistency, and that it can be brought to a state of structural
    // correspondace using information in the transaction logs. When structural
    // correspondace is achieved, we can reliably refresh the accessor hierarchy
    // (Table::refresh_accessor_tree()) to bring it back to a fully concsistent
    // state. See AccessorConsistencyLevels.
    //
    // Much of the information in the transaction logs is not used in this
    // process, because the changes have already been applied to the underlying
    // node structure. All we need to do here is to bring the accessors back
    // into a state where they correctly reflect the underlying structure (or
    // detach them if the underlying object has been removed.)
    //
    // The consequences of the changes in the transaction logs can be divided
    // into two types; those that need to be applied to the accessors
    // immediately (Table::adj_insert_column()), and those that can be "lumped
    // together" and deduced during a final accessor refresh operation
    // (Table::refresh_accessor_tree()).
    //
    // Most transaction log instructions have consequences of both types. For
    // example, when an "insert column" instruction is seen, we must immediately
    // shift the positions of all existing columns accessors after the point of
    // insertion. For practical reasons, and for efficiency, we will just insert
    // a null pointer into `Table::m_cols` at this time, and then postpone the
    // creation of the column accessor to the final per-table accessor refresh
    // operation.
    //
    // The final per-table refresh operation visits each table accessor
    // recursively starting from the roots (group-level tables). It relies on
    // the the per-table accessor dirty flags (Table::m_dirty) to prune the
    // traversal to the set of accessors that were touched by the changes in the
    // transaction logs.
    // Update memory mapping if database file has grown

    m_alloc.update_reader_view(new_file_size); // Throws

    bool schema_changed = false;
    _impl::TransactLogParser parser; // Throws
    TransactAdvancer advancer(*this, schema_changed);
    parser.parse(in, advancer); // Throws

    m_top.detach();                                 // Soft detach
    bool create_group_when_missing = false;         // See Group::attach_shared().
    attach(new_top_ref, create_group_when_missing); // Throws
    refresh_dirty_accessors();                      // Throws

    if (schema_changed)
        send_schema_change_notification();
}


void Group::prepare_history_parent(Array& history_root, int history_type,
                                   int history_schema_version)
{
    REALM_ASSERT(m_file_format_version >= 7);
    if (m_top.size() < 10) {
        REALM_ASSERT(m_top.size() <= 7);
        while (m_top.size() < 7) {
            m_top.add(0); // Throws
        }
        ref_type history_ref = 0; // No history yet
        m_top.add(RefOrTagged::make_tagged(history_type)); // Throws
        m_top.add(RefOrTagged::make_ref(history_ref)); // Throws
        m_top.add(RefOrTagged::make_tagged(history_schema_version)); // Throws
    }
    else {
        int stored_history_type = int(m_top.get_as_ref_or_tagged(7).get_as_int());
        int stored_history_schema_version = int(m_top.get_as_ref_or_tagged(9).get_as_int());
        if (stored_history_type != Replication::hist_None) {
            REALM_ASSERT(stored_history_type == history_type);
            REALM_ASSERT(stored_history_schema_version == history_schema_version);
        }
        m_top.set(7, RefOrTagged::make_tagged(history_type)); // Throws
        m_top.set(9, RefOrTagged::make_tagged(history_schema_version)); // Throws
    }
    set_history_parent(history_root);
}


#ifdef REALM_DEBUG // LCOV_EXCL_START ignore debug functions

class MemUsageVerifier : public Array::MemUsageHandler {
public:
    MemUsageVerifier(ref_type ref_begin, ref_type immutable_ref_end, ref_type mutable_ref_end, ref_type baseline)
        : m_ref_begin(ref_begin)
        , m_immutable_ref_end(immutable_ref_end)
        , m_mutable_ref_end(mutable_ref_end)
        , m_baseline(baseline)
    {
    }
    void add_immutable(ref_type ref, size_t size)
    {
        REALM_ASSERT_3(ref % 8, ==, 0);  // 8-byte alignment
        REALM_ASSERT_3(size % 8, ==, 0); // 8-byte alignment
        REALM_ASSERT_3(size, >, 0);
        REALM_ASSERT_3(ref, >=, m_ref_begin);
        REALM_ASSERT_3(size, <=, m_immutable_ref_end - ref);
        Chunk chunk;
        chunk.ref = ref;
        chunk.size = size;
        m_chunks.push_back(chunk);
    }
    void add_mutable(ref_type ref, size_t size)
    {
        REALM_ASSERT_3(ref % 8, ==, 0);  // 8-byte alignment
        REALM_ASSERT_3(size % 8, ==, 0); // 8-byte alignment
        REALM_ASSERT_3(size, >, 0);
        REALM_ASSERT_3(ref, >=, m_immutable_ref_end);
        REALM_ASSERT_3(size, <=, m_mutable_ref_end - ref);
        Chunk chunk;
        chunk.ref = ref;
        chunk.size = size;
        m_chunks.push_back(chunk);
    }
    void add(ref_type ref, size_t size)
    {
        REALM_ASSERT_3(ref % 8, ==, 0);  // 8-byte alignment
        REALM_ASSERT_3(size % 8, ==, 0); // 8-byte alignment
        REALM_ASSERT_3(size, >, 0);
        REALM_ASSERT_3(ref, >=, m_ref_begin);
        REALM_ASSERT(size <= (ref < m_baseline ? m_immutable_ref_end : m_mutable_ref_end) - ref);
        Chunk chunk;
        chunk.ref = ref;
        chunk.size = size;
        m_chunks.push_back(chunk);
    }
    void add(const MemUsageVerifier& verifier)
    {
        m_chunks.insert(m_chunks.end(), verifier.m_chunks.begin(), verifier.m_chunks.end());
    }
    void handle(ref_type ref, size_t allocated, size_t) override
    {
        add(ref, allocated);
    }
    void canonicalize()
    {
        // Sort the chunks in order of increasing ref, then merge adjacent
        // chunks while checking that there is no overlap
        typedef std::vector<Chunk>::iterator iter;
        iter i_1 = m_chunks.begin(), end = m_chunks.end();
        iter i_2 = i_1;
        sort(i_1, end);
        if (i_1 != end) {
            while (++i_2 != end) {
                ref_type prev_ref_end = i_1->ref + i_1->size;
                REALM_ASSERT_3(prev_ref_end, <=, i_2->ref);
                if (i_2->ref == prev_ref_end) { // in-file
                    i_1->size += i_2->size; // Merge
                }
                else {
                    *++i_1 = *i_2;
                }
            }
            m_chunks.erase(i_1 + 1, end);
        }
    }
    void clear()
    {
        m_chunks.clear();
    }
    void check_total_coverage()
    {
        REALM_ASSERT_3(m_chunks.size(), ==, 1);
        REALM_ASSERT_3(m_chunks.front().ref, ==, m_ref_begin);
        REALM_ASSERT_3(m_chunks.front().size, ==, m_mutable_ref_end - m_ref_begin);
    }

private:
    struct Chunk {
        ref_type ref;
        size_t size;
        bool operator<(const Chunk& c) const
        {
            return ref < c.ref;
        }
    };
    std::vector<Chunk> m_chunks;
    ref_type m_ref_begin, m_immutable_ref_end, m_mutable_ref_end, m_baseline;
};

#endif

void Group::verify() const
{
#ifdef REALM_DEBUG
    REALM_ASSERT(is_attached());

    m_alloc.verify();

    if (!m_top.is_attached()) {
        REALM_ASSERT(m_alloc.is_free_space_clean());
        return;
    }

    // Verify tables
    {
        size_t n = m_tables.size();
        for (size_t i = 0; i != n; ++i) {
            ConstTableRef table = get_table(i);
            REALM_ASSERT_3(table->get_index_in_group(), ==, i);
            table->verify();
        }
    }

    // Verify history if present
    if (Replication* repl = get_replication()) {
        if (_impl::History* hist = repl->get_history()) {
            _impl::History::version_type version = 0;
            int history_type = 0;
            int history_schema_version = 0;
            get_version_and_history_info(m_top, version, history_type, history_schema_version);
            REALM_ASSERT(history_type != Replication::hist_None || history_schema_version == 0);
            hist->update_from_parent(version);
            hist->verify();
        }
    }

    size_t logical_file_size = to_size_t(m_top.get_as_ref_or_tagged(2).get_as_int());
    size_t ref_begin = sizeof(SlabAlloc::Header);
    ref_type immutable_ref_end = logical_file_size;
    ref_type mutable_ref_end = m_alloc.get_total_size();
    ref_type baseline = m_alloc.get_baseline();

    // Check the consistency of the allocation of used memory
    MemUsageVerifier mem_usage_1(ref_begin, immutable_ref_end, mutable_ref_end, baseline);
    m_top.report_memory_usage(mem_usage_1);
    mem_usage_1.canonicalize();

    // Check concistency of the allocation of the immutable memory that was
    // marked as free before the file was opened.
    MemUsageVerifier mem_usage_2(ref_begin, immutable_ref_end, mutable_ref_end, baseline);
    {
        REALM_ASSERT_EX(m_top.size() == 3 || m_top.size() == 5 || m_top.size() == 7 ||
                        m_top.size() == 10, m_top.size());
        Allocator& alloc = m_top.get_alloc();
        ArrayInteger pos(alloc), len(alloc), ver(alloc);
        size_t pos_ndx = 3, len_ndx = 4, ver_ndx = 5;
        pos.set_parent(const_cast<Array*>(&m_top), pos_ndx);
        len.set_parent(const_cast<Array*>(&m_top), len_ndx);
        ver.set_parent(const_cast<Array*>(&m_top), ver_ndx);
        if (m_top.size() > pos_ndx) {
            if (ref_type ref = m_top.get_as_ref(pos_ndx))
                pos.init_from_ref(ref);
        }
        if (m_top.size() > len_ndx) {
            if (ref_type ref = m_top.get_as_ref(len_ndx))
                len.init_from_ref(ref);
        }
        if (m_top.size() > ver_ndx) {
            if (ref_type ref = m_top.get_as_ref(ver_ndx))
                ver.init_from_ref(ref);
        }
        REALM_ASSERT(pos.is_attached() == len.is_attached());
        REALM_ASSERT(pos.is_attached() || !ver.is_attached()); // pos.is_attached() <== ver.is_attached()
        if (pos.is_attached()) {
            size_t n = pos.size();
            REALM_ASSERT_3(n, ==, len.size());
            if (ver.is_attached())
                REALM_ASSERT_3(n, ==, ver.size());
            for (size_t i = 0; i != n; ++i) {
                ref_type ref = to_ref(pos.get(i));
                size_t size_of_i = to_size_t(len.get(i));
                mem_usage_2.add_immutable(ref, size_of_i);
            }
            mem_usage_2.canonicalize();
            mem_usage_1.add(mem_usage_2);
            mem_usage_1.canonicalize();
            mem_usage_2.clear();
        }
    }

    // Check the concistency of the allocation of the immutable memory that has
    // been marked as free after the file was opened
    for (const auto& free_block : m_alloc.m_free_read_only) {
        mem_usage_2.add_immutable(free_block.first, free_block.second);
    }
    mem_usage_2.canonicalize();
    mem_usage_1.add(mem_usage_2);
    mem_usage_1.canonicalize();
    mem_usage_2.clear();

    // Check the consistency of the allocation of the mutable memory that has
    // been marked as free
    m_alloc.for_all_free_entries([&](ref_type ref, int sz) { mem_usage_2.add_mutable(ref, sz); });
    mem_usage_2.canonicalize();
    mem_usage_1.add(mem_usage_2);
    mem_usage_1.canonicalize();
    mem_usage_2.clear();

    // Due to a current problem with the baseline not reflecting the logical
    // file size, but the physical file size, there is a potential gap of
    // unusable ref-space between the logical file size and the baseline. We
    // need to take that into account here.
    REALM_ASSERT_3(immutable_ref_end, <=, baseline);
    if (immutable_ref_end < baseline) {
        ref_type ref = immutable_ref_end;
        size_t corrected_size = baseline - immutable_ref_end;
        mem_usage_1.add_mutable(ref, corrected_size);
        mem_usage_1.canonicalize();
    }

    // At this point we have accounted for all memory managed by the slab
    // allocator
    mem_usage_1.check_total_coverage();
#endif
}

#ifdef REALM_DEBUG

MemStats Group::get_stats()
{
    MemStats mem_stats;
    m_top.stats(mem_stats);

    return mem_stats;
}


void Group::print() const
{
    m_alloc.print();
}


void Group::print_free() const
{
    Allocator& alloc = m_top.get_alloc();
    ArrayInteger pos(alloc), len(alloc), ver(alloc);
    size_t pos_ndx = 3, len_ndx = 4, ver_ndx = 5;
    pos.set_parent(const_cast<Array*>(&m_top), pos_ndx);
    len.set_parent(const_cast<Array*>(&m_top), len_ndx);
    ver.set_parent(const_cast<Array*>(&m_top), ver_ndx);
    if (m_top.size() > pos_ndx) {
        if (ref_type ref = m_top.get_as_ref(pos_ndx))
            pos.init_from_ref(ref);
    }
    if (m_top.size() > len_ndx) {
        if (ref_type ref = m_top.get_as_ref(len_ndx))
            len.init_from_ref(ref);
    }
    if (m_top.size() > ver_ndx) {
        if (ref_type ref = m_top.get_as_ref(ver_ndx))
            ver.init_from_ref(ref);
    }

    if (!pos.is_attached()) {
        std::cout << "none\n";
        return;
    }
    bool has_versions = ver.is_attached();

    size_t n = pos.size();
    for (size_t i = 0; i != n; ++i) {
        size_t offset = to_size_t(pos[i]);
        size_t size_of_i = to_size_t(len[i]);
        std::cout << i << ": " << offset << " " << size_of_i;

        if (has_versions) {
            size_t version = to_size_t(ver[i]);
            std::cout << " " << version;
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}


void Group::to_dot(std::ostream& out) const
{
    out << "digraph G {" << std::endl;

    out << "subgraph cluster_group {" << std::endl;
    out << " label = \"Group\";" << std::endl;

    m_top.to_dot(out, "group_top");
    m_table_names.to_dot(out, "table_names");
    m_tables.to_dot(out, "tables");

    // Tables
    for (size_t i = 0; i < m_tables.size(); ++i) {
        ConstTableRef table = get_table(i);
        StringData name = get_table_name(i);
        table->to_dot(out, name);
    }

    out << "}" << std::endl;
    out << "}" << std::endl;
}


void Group::to_dot() const
{
    to_dot(std::cerr);
}


void Group::to_dot(const char* file_path) const
{
    std::ofstream out(file_path);
    to_dot(out);
}

#endif

std::pair<ref_type, size_t> Group::get_to_dot_parent(size_t ndx_in_parent) const
{
    return std::make_pair(m_tables.get_ref(), ndx_in_parent);
}

// LCOV_EXCL_STOP ignore debug functions

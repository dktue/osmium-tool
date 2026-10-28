/*

Osmium -- OpenStreetMap data manipulation command line tool
https://osmcode.org/osmium-tool/

Copyright (C) 2013-2026  Jochen Topf <jochen@topf.org>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/

#include "command_apply_changes.hpp"

#include "exception.hpp"
#include "util.hpp"

#include <osmium/index/id_set.hpp>
#include <osmium/index/map/sparse_mem_array.hpp>
#include <osmium/io/detail/pbf_decoder.hpp>
#include <osmium/io/detail/pbf_input_format.hpp>
#include <osmium/io/detail/pbf_output_format.hpp>
#include <osmium/io/detail/read_write.hpp>
#include <osmium/io/file.hpp>
#include <osmium/io/file_compression.hpp>
#include <osmium/io/file_format.hpp>
#include <osmium/io/header.hpp>
#include <osmium/io/input_iterator.hpp>
#include <osmium/io/output_iterator.hpp>
#include <osmium/io/reader.hpp>
#include <osmium/io/reader_with_progress_bar.hpp>
#include <osmium/io/writer.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/object_pointer_collection.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/osm/item_type.hpp>
#include <osmium/osm/metadata_options.hpp>
#include <osmium/osm/object.hpp>
#include <osmium/osm/object_comparisons.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/thread/pool.hpp>
#include <osmium/util/file.hpp>
#include <osmium/util/progress_bar.hpp>
#include <osmium/util/verbose_output.hpp>
#include <osmium/visitor.hpp>

#include <boost/program_options.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <deque>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using location_index_type = osmium::index::map::SparseMemArray<osmium::unsigned_object_id_type, osmium::Location>;

bool CommandApplyChanges::setup(const std::vector<std::string>& arguments) {
    po::options_description opts_cmd{"COMMAND OPTIONS"};
    opts_cmd.add_options()
    ("change-file-format", po::value<std::string>(), "Format of the change file(s)")
    ("redact",            "Redact (patch) OSM files")
    ("with-history,H",    "Apply changes to history file")
    ("locations-on-ways", "Expect and update locations on ways")
    ("no-passthrough",    "Do not copy unchanged PBF blocks, re-encode all data")
    ;

    const po::options_description opts_common{add_common_options()};
    const po::options_description opts_input{add_single_input_options()};
    const po::options_description opts_output{add_output_options()};

    po::options_description hidden;
    hidden.add_options()
    ("input-filename", po::value<std::string>(), "OSM input file")
    ("change-filenames", po::value<std::vector<std::string>>(), "OSM change input files")
    ;

    po::options_description desc;
    desc.add(opts_cmd).add(opts_common).add(opts_input).add(opts_output);

    po::options_description parsed_options;
    parsed_options.add(desc).add(hidden);

    po::positional_options_description positional;
    positional.add("input-filename", 1);
    positional.add("change-filenames", -1);

    po::variables_map vm;
    po::store(po::command_line_parser(arguments).options(parsed_options).positional(positional).run(), vm);
    po::notify(vm);

    if (!setup_common(vm, desc)) {
        return false;
    }
    setup_progress(vm);
    setup_input_file(vm);
    setup_output_file(vm);

    if (vm.count("change-filenames")) {
        m_change_filenames = vm["change-filenames"].as<std::vector<std::string>>();
    } else {
        throw argument_error{"Need data file and at least one change file on the command line."};
    }

    if (vm.count("change-file-format")) {
        m_change_file_format = vm["change-file-format"].as<std::string>();
    }

    if (vm.count("locations-on-ways")) {
        m_locations_on_ways = true;
    }

    if (vm.count("no-passthrough")) {
        m_passthrough = false;
    }

    if (vm.count("with-history")) {
        if (m_locations_on_ways) {
            throw argument_error{"Can not use --with-history/-H and --locations-on-ways together."};
        }
        m_with_history = true;
        m_output_file.set_has_multiple_object_versions(true);
    } else {
        if (m_input_file.has_multiple_object_versions() && m_output_file.has_multiple_object_versions()) {
            if (m_locations_on_ways) {
                throw argument_error{"Can not use --locations-on-ways on history files."};
            }
            m_with_history = true;
        } else if (m_input_file.has_multiple_object_versions() != m_output_file.has_multiple_object_versions()) {
            throw argument_error{"Input and output file must both be OSM data files or both OSM history files (force with --with-history)."};
        }
    }

    if (vm.count("redact")) {
        if (m_locations_on_ways) {
            throw argument_error{"Can not use --redact and --locations-on-ways together."};
        }
        m_with_history = true;
        m_redact = true;
        m_output_file.set_has_multiple_object_versions(true);
    }

    return true;
}

void CommandApplyChanges::show_arguments() {
    m_vout << "  input data file name: " << m_input_filename << "\n";
    m_vout << "  input change file names: \n";
    for (const auto& fn : m_change_filenames) {
        m_vout << "    " << fn << "\n";
    }
    m_vout << "  data file format: " << m_input_format << "\n";
    m_vout << "  change file format: " << m_change_file_format << "\n";
    show_output_arguments(m_vout);
    m_vout << "  reading and writing history file: " << yes_no(m_with_history);
    m_vout << "  locations on ways: " << yes_no(m_locations_on_ways);
    m_vout << "  copy unchanged PBF blocks: " << yes_no(m_passthrough);
}

namespace {

/**
 *  Objects are ordered by type and ID, negative IDs first, then positive
 *  IDs, both ordered by their absolute value. This is the same order as
 *  in osmium::object_order_type_id_reverse_version without the version.
 */
using object_key_type = std::tuple<osmium::item_type, bool, osmium::unsigned_object_id_type>;

object_key_type object_key(const osmium::OSMObject& object) noexcept {
    return object_key_type{object.type(), object.id() > 0, object.positive_id()};
}

/**
 *  Copy the first OSM object with a given Id to the output. Keep
 *  track of the Id of each object to do this.
 */
class copy_first_with_id {

    osmium::io::Writer* writer;
    object_key_type last_key{osmium::item_type::undefined, false, 0};

public:

    using iterator_category = std::output_iterator_tag;
    using value_type = const osmium::OSMObject;
    using reference = const osmium::OSMObject&;

    explicit copy_first_with_id(osmium::io::Writer* w) :
        writer(w) {
    }

    void push_back(const osmium::OSMObject& obj) {
        const auto key = object_key(obj);
        if (key != last_key) {
            if (obj.visible()) {
                (*writer)(obj);
            }
            last_key = key;
        }
    }

    const object_key_type& last() const noexcept {
        return last_key;
    }

    /// Continue after objects up to this key were written some other way.
    void set_last(const object_key_type& key) noexcept {
        last_key = key;
    }

}; // class copy_first_with_id

/**
 *  A data blob from a PBF file together with its decoded objects and the
 *  information needed to decide whether it can be copied unchanged.
 */
struct input_blob {
    // Shared with the task preparing the previous blob.
    std::shared_ptr<std::string> data;
    std::vector<osmium::memory::Buffer> buffers;
    object_key_type first_key{};
    object_key_type last_key{};
    bool ordered_and_visible = false;

    // There are no changes between the first key of this blob and the
    // first key of the next blob. The blob was not decoded and only
    // first_key is set.
    bool no_changes = false;

    // The changes from the first key of this blob up to (excluding) the
    // first key of the next blob have the indexes [changes_begin,
    // changes_end). Only set if both keys are known. next_key is empty
    // for the last blob.
    bool changes_known = false;
    std::optional<object_key_type> next_key;
    std::size_t changes_begin = 0;
    std::size_t changes_end = 0;

    // Set by prepare_input_blob() if the blob can be copied unchanged.
    // The changes after the blob start at changes_end.
    bool copy = false;

    // Set by prepare_input_blob() if the changes have been merged into
    // the objects of the blob and the result encoded into encoded_blobs.
    bool merged = false;
    std::vector<std::string> encoded_blobs;
    object_key_type merged_last_key{};

    // Set by prepare_input_blob() if the unchanged objects were copied
    // without decoding them when merging.
    bool spliced = false;
};

object_key_type object_key(osmium::item_type type, osmium::object_id_type id) noexcept {
    const auto positive_id = static_cast<osmium::unsigned_object_id_type>(id < 0 ? -id : id);
    return object_key_type{type, id > 0, positive_id};
}

/**
 *  Decode the objects of a blob from its uncompressed data.
 */
void decode_input_blob(input_blob& blob, const osmium::io::detail::data_view& data) {
    osmium::io::detail::PBFPrimitiveBlockDecoder decoder{
        data,
        osmium::osm_entity_bits::object,
        osmium::io::read_meta::yes};
    osmium::memory::Buffer buffer = decoder();

    // The decoder returns a chain of buffers with the oldest data in the
    // most deeply nested one.
    while (buffer.has_nested_buffers()) {
        blob.buffers.push_back(std::move(*buffer.get_last_nested()));
    }
    blob.buffers.push_back(std::move(buffer));

    bool empty = true;
    for (const auto& objects : blob.buffers) {
        for (const auto& object : objects.select<osmium::OSMObject>()) {
            const auto key = object_key(object);
            if (empty) {
                blob.first_key = key;
                blob.ordered_and_visible = true;
                empty = false;
            } else if (!(blob.last_key < key)) {
                blob.ordered_and_visible = false;
            }
            if (!object.visible()) {
                blob.ordered_and_visible = false;
            }
            blob.last_key = key;
        }
    }
}

void decode_input_blob(input_blob& blob) {
    std::string uncompressed;
    decode_input_blob(blob, osmium::io::detail::decode_blob(*blob.data, uncompressed));
}

/**
 *  Get the key of the first object in a blob by uncompressing and decoding
 *  only as much of the blob as needed.
 */
std::optional<object_key_type> first_object_key(const std::string& data) {
    std::string uncompressed;
    for (uint64_t size = 16 * 1024; ; size *= 4) {
        const auto prefix = osmium::io::detail::decode_blob(data, uncompressed, size);
        const auto key = osmium::io::detail::decode_first_object_key(prefix);
        if (key.first != osmium::item_type::undefined) {
            return object_key(key.first, key.second);
        }
        if (prefix.size() != size) { // got the whole blob, but no object
            return std::nullopt;
        }
    }
}

/**
 *  Find out in a worker thread which blobs of a group of consecutive blobs
 *  have to be decoded. A blob can only contain objects from its first key
 *  up to (excluding) the first key of the next blob, because the input
 *  file must be sorted. If none of the (sorted) changes is in this range,
 *  the blob does not have to be decoded and is marked with no_changes.
 *  This is cheap, because only the beginning of each blob and of the blob
 *  after the group have to be decoded. The order and visibility of the
 *  objects in such a blob is not checked. For the other blobs the range
 *  of changes up to the first key of the next blob is stored.
 *
 *  @param group The blobs.
 *  @param next The blob after the group, nullptr after the last blob.
 *  @param changes The sorted changes, nullptr to decode all blobs.
 */
std::vector<input_blob> classify_input_blobs(std::vector<std::shared_ptr<std::string>> group,
                                             std::shared_ptr<std::string> next,
                                             const osmium::ObjectPointerCollection* changes) {
    std::vector<input_blob> blobs(group.size());
    for (std::size_t i = 0; i < group.size(); ++i) {
        blobs[i].data = std::move(group[i]);
    }

    if (!changes) {
        return blobs;
    }

    std::vector<std::optional<object_key_type>> keys;
    keys.reserve(blobs.size() + 1);
    for (const auto& blob : blobs) {
        keys.push_back(first_object_key(*blob.data));
    }
    keys.push_back(next ? first_object_key(*next) : std::nullopt);
    const bool last_group = !next;
    // Release the next blob before the result is ready, so that the
    // main thread can move it to the output.
    next.reset();

    // Find the first change not before a key. The first keys of the
    // blobs are usually increasing, so the search can start where the
    // last search ended.
    auto search_begin = changes->cbegin();
    std::optional<object_key_type> search_key;
    const auto find_change = [&](const object_key_type& key) {
        if (!search_key || key < *search_key) {
            search_begin = changes->cbegin();
        }
        search_begin = std::lower_bound(search_begin, changes->cend(), key,
                                        [](const osmium::OSMObject& object, const object_key_type& k) {
            return object_key(object) < k;
        });
        search_key = key;
        return search_begin;
    };
    const auto index = [&](osmium::ObjectPointerCollection::const_iterator change) {
        return static_cast<std::size_t>(change - changes->cbegin());
    };

    for (std::size_t i = 0; i < blobs.size(); ++i) {
        auto& blob = blobs[i];
        const auto& first_key = keys[i];
        const auto& next_key = keys[i + 1];
        if (first_key && (!next_key || *first_key < *next_key)) {
            const auto change = find_change(*first_key);
            if (change == changes->cend() || (next_key && !(object_key(*change) < *next_key))) {
                blob.first_key = *first_key;
                blob.no_changes = true;
            } else if (next_key || (last_group && i + 1 == blobs.size())) {
                blob.changes_known = true;
                blob.next_key = next_key;
                blob.changes_begin = index(change);
                blob.changes_end = next_key ? index(find_change(*next_key)) : changes->size();
            }
        }
    }

    return blobs;
}

/**
 *  Do any of the changes in [begin, end) have to be merged into the
 *  objects of the blob? The changes must all be in the ID range of the
 *  blob. Created or modified objects always have to be merged. Deleted
 *  objects only if they are in the blob, otherwise they don't change
 *  anything.
 */
bool changes_touch_blob(osmium::ObjectPointerCollection::const_iterator begin,
                        osmium::ObjectPointerCollection::const_iterator end,
                        const input_blob& blob) {
    if (std::any_of(begin, end, [](const osmium::OSMObject& object) {
            return object.visible();
        })) {
        return true;
    }

    auto it = begin;
    for (const auto& objects : blob.buffers) {
        for (const auto& object : objects.select<osmium::OSMObject>()) {
            const auto key = object_key(object);
            while (it != end && object_key(*it) < key) {
                ++it;
            }
            if (it == end) {
                return false;
            }
            if (object_key(*it) == key) {
                return true;
            }
        }
    }

    return false;
}

/**
 *  Waits for all futures in a queue when it is destroyed.
 */
template <typename T>
class wait_for_futures {

    const std::deque<std::future<T>>& m_futures;

public:

    explicit wait_for_futures(const std::deque<std::future<T>>& futures) noexcept :
        m_futures(futures) {
    }

    wait_for_futures(const wait_for_futures&) = delete;
    wait_for_futures& operator=(const wait_for_futures&) = delete;
    wait_for_futures(wait_for_futures&&) = delete;
    wait_for_futures& operator=(wait_for_futures&&) = delete;

    ~wait_for_futures() {
        for (const auto& future : m_futures) {
            if (future.valid()) {
                future.wait();
            }
        }
    }

}; // class wait_for_futures

/**
 *  Add a change to the result of a merge (see merge_objects()) unless
 *  an object with the same key was added before or the change is a
 *  deletion.
 */
template <typename TFunction>
void merge_change(const osmium::OSMObject& change, object_key_type& last_key, TFunction&& add) {
    const auto key = object_key(change);
    if (key != last_key) {
        if (change.visible()) {
            add(change.data(), change.data() + change.padded_size());
        }
        last_key = key;
    }
}

/**
 *  Merge the changes into the objects of a blob like std::set_union()
 *  does and keep only the first object with each ID like
 *  copy_first_with_id does. The merged objects are not copied, instead
 *  the function add is called with the begin and end of each run of
 *  input objects between changes and of each change.
 *
 *  @param blob The input blob.
 *  @param it Iterator to the next change. Will be moved past all changes
 *            merged.
 *  @param end End of the changes.
 *  @param last_key Key of the last object written. Will be updated.
 *  @param add Function called with each run of merged objects.
 */
template <typename TFunction>
void merge_objects(const input_blob& blob,
                   osmium::ObjectPointerCollection::const_iterator& it,
                   osmium::ObjectPointerCollection::const_iterator end,
                   object_key_type& last_key,
                   TFunction&& add) {
    for (const auto& buffer : blob.buffers) {
        // Start of the input objects not yet added or nullptr.
        const unsigned char* run = nullptr;
        const auto add_run = [&](const unsigned char* run_end) {
            if (run) {
                add(run, run_end);
                run = nullptr;
            }
        };

        for (auto object = buffer.cbegin<osmium::OSMObject>(); object != buffer.cend<osmium::OSMObject>(); ++object) {
            const auto key = object_key(*object);
            if (it != end && !(key < object_key(*it))) {
                add_run(object.data());
                const osmium::object_order_type_id_reverse_version order;
                while (it != end && order(*it, *object)) {
                    merge_change(*it, last_key, add);
                    ++it;
                }
                if (it != end && !order(*object, *it)) {
                    // On equivalence the change wins.
                    merge_change(*it, last_key, add);
                    ++it;
                    continue;
                }
            }
            if (key != last_key && object->visible()) {
                if (!run) {
                    run = object.data();
                }
            } else {
                add_run(object.data());
            }
            last_key = key;
        }
        add_run(buffer.data() + buffer.committed());
    }
}

/**
 *  Merge the changes into the objects of a blob (see merge_objects()).
 *
 *  @returns Buffer with the merged objects.
 */
osmium::memory::Buffer merge_blob(const input_blob& blob,
                                  osmium::ObjectPointerCollection::const_iterator& it,
                                  osmium::ObjectPointerCollection::const_iterator end,
                                  object_key_type& last_key) {
    std::size_t size = 0;
    for (const auto& buffer : blob.buffers) {
        size += buffer.committed();
    }
    for (auto next = it; next != end && !(blob.last_key < object_key(*next)); ++next) {
        size += next->padded_size();
    }
    osmium::memory::Buffer out{size, osmium::memory::Buffer::auto_grow::yes};

    merge_objects(blob, it, end, last_key, [&](const unsigned char* begin, const unsigned char* run_end) {
        const auto run_size = static_cast<std::size_t>(run_end - begin);
        std::copy_n(begin, run_size, out.reserve_space(run_size));
        out.commit();
    });

    return out;
}

/**
 *  Prepare a blob with dense nodes for the merge like prepare_input_blob(),
 *  but without decoding the nodes into OSM objects. The changes are merged
 *  into the encoded nodes with osmium::io::detail::DenseNodesBlockEditor
 *  instead, which is much cheaper. This only works if all changes are
 *  nodes, the blob is supported by DenseNodesBlockEditor and has positive,
 *  strictly increasing IDs, and the result fits into one block.
 *
 *  @returns false if this does not work. The blob must then be prepared in
 *           the normal way.
 */
bool prepare_dense_node_blob(input_blob& blob,
                             const osmium::io::detail::data_view& data,
                             osmium::ObjectPointerCollection::const_iterator begin,
                             osmium::ObjectPointerCollection::const_iterator end,
                             const osmium::io::detail::pbf_output_options& options) {
    if (!std::all_of(begin, end, [](const osmium::OSMObject& object) {
            return object.type() == osmium::item_type::node;
        })) {
        return false;
    }

    osmium::io::detail::DenseNodesBlockEditor editor{data, options};
    if (!editor.valid()) {
        return false;
    }

    // Find out cheaply whether the result could get too large for one
    // block. Changes of nodes in the block do not make it larger.
    const auto ids = editor.node_ids();
    std::size_t max_count = ids.size();
    for (auto it = begin; it != end; ++it) {
        const bool first_with_key = it == begin || object_key(*std::prev(it)) != object_key(*it);
        if (first_with_key && it->visible() && !std::binary_search(ids.cbegin(), ids.cend(), it->id())) {
            ++max_count;
        }
    }
    if (max_count > osmium::io::detail::max_entities_per_block || !editor.next()) {
        return false;
    }

    const auto node_key = [&]() {
        return object_key_type{osmium::item_type::node, true, static_cast<osmium::unsigned_object_id_type>(editor.node_id())};
    };

    // Same as osmium::object_order_type_id_reverse_version for a change and
    // the current node.
    const auto order = [&](const osmium::OSMObject& change, bool change_first) {
        const bool tsvalid = change.timestamp().valid() && editor.node_timestamp().valid();
        const auto change_side = std::make_tuple(object_key(change), editor.node_version(), tsvalid ? editor.node_timestamp() : osmium::Timestamp{});
        const auto node_side = std::make_tuple(node_key(), change.version(), tsvalid ? change.timestamp() : osmium::Timestamp{});
        return change_first ? change_side < node_side : node_side < change_side;
    };

    // The same merge as in merge_objects().
    bool changed = false;
    object_key_type last{osmium::item_type::undefined, false, 0};
    const auto add_change = [&](const osmium::OSMObject& change) {
        merge_change(change, last, [&](const unsigned char* /*begin*/, const unsigned char* /*end*/) {
            editor.add_node(static_cast<const osmium::Node&>(change));
            changed = true;
        });
    };

    const auto first_key = node_key();
    object_key_type last_key{osmium::item_type::undefined, false, 0};
    auto it = begin;
    do {
        const auto key = node_key();
        if (editor.node_id() <= 0 || !(last_key < key)) {
            return false;
        }
        last_key = key;
        if (it != end && !(key < object_key(*it))) {
            while (it != end && order(*it, true)) {
                add_change(*it);
                ++it;
            }
            if (it != end && !order(*it, false)) {
                // On equivalence the change wins.
                add_change(*it);
                ++it;
                editor.drop_node();
                changed = true;
                continue;
            }
        }
        if (key != last) {
            editor.copy_node();
        } else {
            editor.drop_node();
            changed = true;
        }
        last = key;
    } while (editor.next());

    if (!editor.valid() || (blob.next_key && !(last_key < *blob.next_key))) {
        return false;
    }

    // The changes up to the last node, these can not add anything here.
    auto blob_end = it;
    while (blob_end != end && !(last_key < object_key(*blob_end))) {
        add_change(*blob_end);
        ++blob_end;
    }

    blob.first_key = first_key;
    blob.last_key = last_key;
    if (!changed) {
        // The changes after the last node are merged later.
        blob.copy = true;
        blob.changes_end = blob.changes_begin + static_cast<std::size_t>(blob_end - begin);
        return true;
    }

    for (it = blob_end; it != end; ++it) {
        add_change(*it);
    }

    // Use the normal way if the block gets too large or too many strings
    // are not used any more, those would be copied again and again.
    if (editor.count() > osmium::io::detail::max_entities_per_block ||
        editor.size() > osmium::io::detail::max_uncompressed_blob_size ||
        editor.unused_strings() * 4 > editor.string_table_size()) {
        return false;
    }

    if (editor.count() > 0) {
        blob.encoded_blobs.push_back(osmium::io::detail::serialize_blob(editor.serialize(), options.use_compression, options.compression_level));
    }
    blob.merged = true;
    blob.spliced = true;
    blob.merged_last_key = last;
    return true;
}

/**
 *  Does the same as prepare_input_blob() for a blob with ways or relations,
 *  but without decoding the objects of the blob: The objects not replaced
 *  by changes are copied as encoded messages together with the string
 *  table of the blob into a new block. Only the changes are encoded. The
 *  result is a single blob.
 *
 *  This is not done for blobs that can not be handled this way and if too
 *  many strings of the string table are not used any more, so that it
 *  doesn't grow without bounds when the output is updated again.
 *
 *  @returns false if the blob was not handled, nothing in blob is changed
 *           in this case.
 */
bool splice_input_blob(input_blob& blob,
                       const osmium::io::detail::data_view& data,
                       const osmium::ObjectPointerCollection& changes,
                       const osmium::io::detail::pbf_output_options& options) {
    const osmium::io::detail::PBFEncodedPrimitiveBlock block{data};
    const auto type = block.type();
    if (type == osmium::item_type::undefined) {
        return false;
    }

    std::vector<osmium::io::detail::pbf_encoded_object> objects;
    bool ordered_and_visible = true;
    block.for_each_object([&](const osmium::io::detail::pbf_encoded_object& object) {
        if (!objects.empty() && !(object_key(type, objects.back().id) < object_key(type, object.id))) {
            ordered_and_visible = false;
        }
        if (!object.visible) {
            ordered_and_visible = false;
        }
        objects.push_back(object);
    });
    if (!ordered_and_visible || objects.empty()) {
        return false;
    }

    const auto first_key = object_key(type, objects.front().id);
    const auto last_key = object_key(type, objects.back().id);
    if (blob.next_key && !(last_key < *blob.next_key)) {
        return false;
    }

    const auto begin = osmium::ObjectPointerCollection::const_iterator{changes.cbegin() + static_cast<std::ptrdiff_t>(blob.changes_begin)};
    const auto end = osmium::ObjectPointerCollection::const_iterator{changes.cbegin() + static_cast<std::ptrdiff_t>(blob.changes_end)};
    if (std::any_of(begin, end, [type](const osmium::OSMObject& change) {
            return change.type() != type;
        })) {
        return false;
    }

    // The objects of the result in order: either an object of the blob
    // or a change.
    struct merged_object {
        const osmium::io::detail::pbf_encoded_object* object;
        const osmium::OSMObject* change;
    };
    std::vector<merged_object> merged;
    merged.reserve(objects.size());

    // This is the same merge as in merge_objects().
    auto it = begin;
    object_key_type merged_last_key{osmium::item_type::undefined, false, 0};
    bool touched = false;
    const auto add_change = [&](const osmium::OSMObject& change) {
        const auto key = object_key(change);
        if (key != merged_last_key) {
            if (change.visible()) {
                merged.push_back({nullptr, &change});
            }
            merged_last_key = key;
        }
    };
    for (const auto& object : objects) {
        const auto key = object_key(type, object.id);
        while (it != end && object_key(*it) < key) {
            touched = touched || it->visible();
            add_change(*it);
            ++it;
        }
        if (it != end && object_key(*it) == key) {
            touched = true;
            // The object stays if the first (newest) change with its ID
            // is older, see osmium::object_order_type_id_reverse_version.
            const bool tsvalid = it->timestamp().valid() && object.timestamp.valid();
            if (std::make_tuple(it->version(), tsvalid ? it->timestamp() : osmium::Timestamp{}) <
                std::make_tuple(object.version, tsvalid ? object.timestamp : osmium::Timestamp{})) {
                merged.push_back({&object, nullptr});
                merged_last_key = key;
            } else {
                add_change(*it);
                ++it;
            }
            continue;
        }
        merged.push_back({&object, nullptr});
        merged_last_key = key;
    }

    auto blob_end = it;
    while (blob_end != end && !(last_key < object_key(*blob_end))) {
        touched = touched || blob_end->visible();
        ++blob_end;
    }

    if (!touched) {
        // The changes after the last object are merged later.
        blob.first_key = first_key;
        blob.last_key = last_key;
        blob.ordered_and_visible = true;
        blob.copy = true;
        blob.changes_end = blob.changes_begin + static_cast<std::size_t>(blob_end - begin);
        return true;
    }

    for (; it != end; ++it) {
        add_change(*it);
    }

    if (merged.size() > static_cast<std::size_t>(osmium::io::detail::max_entities_per_block)) {
        return false;
    }

    // Count the strings still used. Index 0 is usually not used.
    const auto stringtable_size = block.stringtable_size();
    std::vector<bool> used(stringtable_size);
    std::size_t used_count = 0;
    bool valid_indexes = true;
    for (const auto& m : merged) {
        if (m.object) {
            block.for_each_string_index(*m.object, [&](uint32_t index) {
                if (index >= stringtable_size) {
                    valid_indexes = false;
                } else if (!used[index]) {
                    used[index] = true;
                    ++used_count;
                }
            });
        }
    }
    if (!valid_indexes || (stringtable_size > 1 && used_count * 4 < (stringtable_size - 1) * 3)) {
        return false;
    }

    osmium::io::detail::PrimitiveBlock primitive_block{options, osmium::io::detail::primitive_group_type(type, options), block.stringtable()};
    for (const auto& m : merged) {
        if (m.object) {
            primitive_block.add_encoded_object(m.object->data);
        } else if (type == osmium::item_type::way) {
            primitive_block.add_way(static_cast<const osmium::Way&>(*m.change));
        } else {
            primitive_block.add_relation(static_cast<const osmium::Relation&>(*m.change));
        }
    }
    const std::string msg = osmium::io::detail::serialize_primitive_block(primitive_block);
    if (msg.size() > osmium::io::detail::max_uncompressed_blob_size) {
        return false;
    }

    if (!merged.empty()) {
        blob.encoded_blobs.push_back(osmium::io::detail::serialize_blob(msg, options.use_compression, options.compression_level));
    }
    blob.first_key = first_key;
    blob.last_key = last_key;
    blob.ordered_and_visible = true;
    blob.merged = true;
    blob.spliced = true;
    blob.merged_last_key = merged_last_key;
    return true;
}

/**
 *  Prepare a blob for the merge in a worker thread. The blob is decoded
 *  and then, if its objects are in order and its range of changes is
 *  known, either marked as unchanged (copy) or the changes are merged
 *  into it and the result is encoded into PBF blobs (merged). This
 *  assumes that nothing at or after the first key of the blob has been
 *  written before, the main thread has to check this before using the
 *  result.
 *
 *  @param blob The input blob.
 *  @param changes The sorted changes.
 *  @param options Options for encoding the merged objects.
 *  @param bucket_count String table bucket count hint shared by the
 *                      tasks.
 */
void prepare_input_blob(input_blob& blob,
                        const osmium::ObjectPointerCollection& changes,
                        const osmium::io::detail::pbf_output_options& options,
                        std::atomic<std::size_t>& bucket_count) {
    std::string uncompressed;
    const auto data = osmium::io::detail::decode_blob(*blob.data, uncompressed);
    const auto begin = osmium::ObjectPointerCollection::const_iterator{changes.cbegin() + static_cast<std::ptrdiff_t>(blob.changes_begin)};
    const auto end = osmium::ObjectPointerCollection::const_iterator{changes.cbegin() + static_cast<std::ptrdiff_t>(blob.changes_end)};
    if (blob.changes_known &&
        (prepare_dense_node_blob(blob, data, begin, end, options) ||
         splice_input_blob(blob, data, changes, options))) {
        return;
    }

    decode_input_blob(blob, data);
    if (!blob.changes_known || !blob.ordered_and_visible ||
        (blob.next_key && !(blob.last_key < *blob.next_key))) {
        return;
    }

    auto blob_end = begin;
    while (blob_end != end && !(blob.last_key < object_key(*blob_end))) {
        ++blob_end;
    }
    if (!changes_touch_blob(begin, blob_end, blob)) {
        // The changes after the last object are merged later.
        blob.copy = true;
        blob.changes_end = blob.changes_begin + static_cast<std::size_t>(blob_end - begin);
        blob.buffers.clear();
        return;
    }

    std::vector<osmium::io::detail::pbf_item_range> ranges;
    const auto add = [&](const unsigned char* run_begin, const unsigned char* run_end) {
        ranges.push_back(osmium::io::detail::pbf_item_range{run_begin, run_end});
    };
    auto it = begin;
    object_key_type last_key{osmium::item_type::undefined, false, 0};
    merge_objects(blob, it, end, last_key, add);
    for (; it != end; ++it) {
        merge_change(*it, last_key, add);
    }

    std::size_t hint = bucket_count.load(std::memory_order_relaxed);
    blob.encoded_blobs = osmium::io::detail::encode_pbf_data_blobs(ranges, options, hint);
    bucket_count.store(hint, std::memory_order_relaxed);

    blob.merged = true;
    blob.merged_last_key = last_key;
    blob.buffers.clear();
}

bool has_locations_on_ways(const osmium::io::Header& header) {
    return std::any_of(header.begin(), header.end(), [](const std::pair<const std::string, std::string>& option) {
        return option.first.rfind("pbf_optional_feature_", 0) == 0 && option.second == "LocationsOnWays";
    });
}

// Same check as in osmium::io::Reader which reads URLs using curl.
bool is_url(const std::string& filename) {
    const std::string protocol{filename.substr(0, filename.find_first_of(':'))};
    return protocol == "http" || protocol == "https" || protocol == "ftp" || protocol == "file";
}

void update_nodes_if_way(osmium::OSMObject* object, const location_index_type& location_index) {
    if (object->type() != osmium::item_type::way) {
        return;
    }

    for (auto& node_ref : static_cast<osmium::Way*>(object)->nodes()) {
        auto location = location_index.get_noexcept(node_ref.positive_ref());
        if (location) {
            node_ref.set_location(location);
        }
    }
}

/**
 *  Read all objects from a change file.
 */
std::vector<osmium::memory::Buffer> read_change_file(const osmium::io::File& file) {
    std::vector<osmium::memory::Buffer> buffers;
    osmium::io::Reader reader{file, osmium::osm_entity_bits::object};
    while (osmium::memory::Buffer buffer = reader.read()) {
        buffers.push_back(std::move(buffer));
    }
    reader.close();
    return buffers;
}

} // anonymous namespace

void CommandApplyChanges::apply_changes_and_write(osmium::ObjectPointerCollection &objects,
                                                  const std::vector<osmium::memory::Buffer> &changes,
                                                  osmium::io::Reader &reader,
                                                  osmium::io::Writer &writer) {
    objects.unique(osmium::object_equal_type_id{});
    m_vout << "There are " << objects.size() << " unique objects in the change files\n";

    osmium::index::IdSetSmall<osmium::unsigned_object_id_type> node_ids;
    m_vout << "Creating node index...\n";
    for (const auto& buffer : changes) {
        for (const auto& way : buffer.select<osmium::Way>()) {
            for (const auto& nr : way.nodes()) {
                node_ids.set(nr.positive_ref());
            }
        }
    }
    node_ids.sort_unique();
    m_vout << "Node index has " << node_ids.size() << " entries\n";

    m_vout << "Creating location index...\n";
    location_index_type location_index;
    for (const auto& buffer : changes) {
        for (const auto& node : buffer.select<osmium::Node>()) {
            location_index.set(node.positive_id(), node.location());
        }
    }
    m_vout << "Location index has " << location_index.size() << " entries\n";

    m_vout << "Applying changes and writing them to output...\n";
    auto it = objects.begin();
    auto last_type = osmium::item_type::undefined;
    while (osmium::memory::Buffer buffer = reader.read()) {
        for (auto& object : buffer.select<osmium::OSMObject>()) {
            if (object.type() < last_type) {
                throw std::runtime_error{"Input data out of order. Need nodes, ways, relations in ID order."};
            }
            if (object.type() == osmium::item_type::node) {
                const auto& node = static_cast<osmium::Node&>(object);
                if (node_ids.get_binary_search(node.positive_id())) {
                    const auto location = location_index.get_noexcept(node.positive_id());
                    if (!location) {
                        location_index.set(node.positive_id(), node.location());
                    }
                }
            } else if (object.type() == osmium::item_type::way) {
                if (last_type == osmium::item_type::node) {
                    location_index.sort();
                    node_ids.clear();
                }
            }

            last_type = object.type();

            auto last_it = it;
            while (it != objects.end() && osmium::object_order_type_id_reverse_version{}(*it, object)) {
                if (it->visible()) {
                    update_nodes_if_way(&*it, location_index);
                    writer(*it);
                }
                last_it = it;
                ++it;
            }

            if (last_it == objects.end() || last_it->type() != object.type() || last_it->id() != object.id()) {
                update_nodes_if_way(&object, location_index);
                writer(object);
            }
        }
    }
    while (it != objects.end()) {
        if (it->visible()) {
            update_nodes_if_way(&*it, location_index);
            writer(*it);
        }
        ++it;
    }
}

/**
 *  Unchanged blocks can only be copied from the input to the output if
 *  both are PBF files and the output would be encoded in the same way
 *  as the input. Returns nullptr if this is the case, otherwise the
 *  reason why not.
 */
const char* CommandApplyChanges::why_no_passthrough() const {
    if (!m_passthrough) {
        return "disabled on command line";
    }
    if (m_with_history) {
        return "not supported for history files";
    }
    if (m_locations_on_ways) {
        return "not supported with --locations-on-ways";
    }
    if (m_input_file.format() != osmium::io::file_format::pbf ||
        m_input_file.compression() != osmium::io::file_compression::none ||
        is_url(m_input_filename)) {
        return "input is not a PBF file";
    }
    if (m_output_file.format() != osmium::io::file_format::pbf) {
        return "output is not a PBF file";
    }
    if (!m_output_file.get("pbf_compression").empty() ||
        !m_output_file.get("pbf_compression_level").empty()) {
        return "output compression is set";
    }
    if (!m_output_file.is_not_false("pbf_dense_nodes")) {
        return "output without dense nodes";
    }
    if (!osmium::metadata_options{m_output_file.get("add_metadata")}.all()) {
        return "output without some metadata";
    }
    if (m_output_file.is_true("locations_on_ways")) {
        return "output with locations on ways";
    }
    return nullptr;
}

/**
 *  Does the same as the std::set_union() in run(), but reads the input
 *  PBF file blob by blob. Blobs not touched by any change are copied to
 *  the output without re-encoding them.
 */
void CommandApplyChanges::apply_changes_with_passthrough(osmium::ObjectPointerCollection &objects,
                                                         osmium::io::Writer &writer) {
    m_vout << "Opening input file...\n";
    const int fd = osmium::io::detail::open_for_reading(m_input_filename);
    osmium::ProgressBar progress_bar{osmium::file_size(fd), display_progress()};

    const auto input_header = osmium::io::detail::decode_header(osmium::io::detail::read_pbf_blob(fd, "OSMHeader"));
    const bool passthrough = !input_header.has_multiple_object_versions() && !has_locations_on_ways(input_header);
    if (!passthrough) {
        m_vout << "Not copying unchanged blocks: input has history or locations on ways\n";
    }

    m_vout << "Applying changes and writing them to output...\n";
    auto& pool = osmium::thread::Pool::default_instance();
    const auto queue_size = osmium::io::detail::get_osmdata_queue_size();

    // Consecutive blobs are classified together in one task to save
    // overhead per task. Classifying is cheap and only needs the raw
    // blobs, so it can run far ahead of the merge.
    constexpr const std::size_t max_group_blobs = 16;
    constexpr const std::size_t max_group_bytes = 1024UL * 1024UL;
    const std::size_t max_blobs_classifying = 8 * queue_size;
    std::deque<std::future<std::vector<input_blob>>> groups_in_flight;
    std::size_t blobs_classifying = 0;

    // Blobs with changes are merged and encoded in the pool in the same
    // way as the Writer would encode them.
    const auto output_options = osmium::io::detail::make_pbf_output_options(m_output_file);
    std::atomic<std::size_t> bucket_count{osmium::io::detail::StringTable::min_bucket_count};

    // Classified blobs in input order. Only blobs that have to be decoded
    // get their own task. Their results are mostly encoded blobs, which
    // are not larger than the input blobs, so many of them can be in
    // flight. This keeps the pool busy even if one task takes longer.
    const std::size_t max_blobs_in_flight = 8 * queue_size;
    const std::size_t max_blobs_decoding = 2 * queue_size;
    std::deque<std::future<input_blob>> blobs_in_flight;
    std::size_t blobs_decoding = 0;
    bool input_done = false;

    // The tasks use the objects, make sure they are done before returning
    // even if there is an exception.
    const wait_for_futures<std::vector<input_blob>> groups_guard{groups_in_flight};
    const wait_for_futures<input_blob> blobs_guard{blobs_in_flight};

    // A group is only submitted when the next blob has been read, because
    // its task needs the beginning of the next blob.
    std::vector<std::shared_ptr<std::string>> group;
    std::size_t group_bytes = 0;

    auto it = objects.cbegin();
    const auto end = objects.cend();
    copy_first_with_id copy_first{&writer};
    std::size_t blobs_copied = 0;
    std::size_t blobs_not_decoded = 0;
    std::size_t blobs_encoded = 0;
    std::size_t blobs_spliced = 0;

    while (true) {
        while (!input_done && blobs_classifying < max_blobs_classifying) {
            std::shared_ptr<std::string> data;
            std::string raw = osmium::io::detail::read_pbf_blob(fd, "OSMData");
            if (raw.empty()) {
                input_done = true;
            } else {
                data = std::make_shared<std::string>(std::move(raw));
            }
            if (!group.empty() && (input_done || group.size() >= max_group_blobs || group_bytes >= max_group_bytes)) {
                blobs_classifying += group.size();
                groups_in_flight.push_back(pool.submit([group = std::move(group), next = data, changes = passthrough ? &objects : nullptr]() mutable {
                    return classify_input_blobs(std::move(group), std::move(next), changes);
                }));
                group.clear();
                group_bytes = 0;
            }
            if (data) {
                group_bytes += data->size();
                group.push_back(std::move(data));
            }
        }
        while (!groups_in_flight.empty() &&
               (blobs_in_flight.empty() ||
                (blobs_in_flight.size() < max_blobs_in_flight && blobs_decoding < max_blobs_decoding))) {
            std::vector<input_blob> blobs = groups_in_flight.front().get();
            groups_in_flight.pop_front();
            blobs_classifying -= blobs.size();
            for (auto& blob : blobs) {
                if (blob.no_changes) {
                    std::promise<input_blob> ready;
                    ready.set_value(std::move(blob));
                    blobs_in_flight.push_back(ready.get_future());
                } else {
                    ++blobs_decoding;
                    blobs_in_flight.push_back(pool.submit([blob = std::move(blob), &objects, &output_options, &bucket_count]() mutable {
                        prepare_input_blob(blob, objects, output_options, bucket_count);
                        return std::move(blob);
                    }));
                }
            }
        }
        if (blobs_in_flight.empty()) {
            break;
        }
        input_blob blob = blobs_in_flight.front().get();
        blobs_in_flight.pop_front();
        if (!blob.no_changes) {
            --blobs_decoding;
        }
        progress_bar.update(osmium::file_offset(fd));

        while (it != end && object_key(*it) < blob.first_key) {
            copy_first.push_back(*it);
            ++it;
        }

        if (blob.no_changes) {
            // There are no changes up to the first key of the next blob,
            // so after this blob the merge continues there.
            if (copy_first.last() < blob.first_key) {
                writer.write_pbf_blob(std::move(*blob.data));
                copy_first.set_last(blob.first_key);
                ++blobs_copied;
                ++blobs_not_decoded;
                continue;
            }
            decode_input_blob(blob); // input not sorted
        }

        if (blob.copy || blob.merged) {
            // The task assumed that the merge would continue at the first
            // key of the blob, which is true if the input is sorted.
            if (copy_first.last() < blob.first_key &&
                static_cast<std::size_t>(it - objects.cbegin()) == blob.changes_begin) {
                if (blob.copy) {
                    writer.write_pbf_blob(std::move(*blob.data));
                    copy_first.set_last(blob.last_key);
                    ++blobs_copied;
                } else {
                    for (auto& encoded : blob.encoded_blobs) {
                        writer.write_pbf_blob(std::move(encoded));
                    }
                    copy_first.set_last(blob.merged_last_key);
                    ++blobs_encoded;
                    if (blob.spliced) {
                        ++blobs_spliced;
                    }
                }
                it = osmium::ObjectPointerCollection::const_iterator{objects.cbegin() + static_cast<std::ptrdiff_t>(blob.changes_end)};
                continue;
            }
            decode_input_blob(blob);
        }

        // The merge would write exactly the objects in the blob if they
        // are in order, none of them is a duplicate of the last object
        // written, and no change has to be merged into them.
        if (passthrough && blob.ordered_and_visible && copy_first.last() < blob.first_key) {
            auto next = it;
            while (next != end && !(blob.last_key < object_key(*next))) {
                ++next;
            }
            if (!changes_touch_blob(it, next, blob)) {
                writer.write_pbf_blob(std::move(*blob.data));
                copy_first.set_last(blob.last_key);
                it = next;
                ++blobs_copied;
                continue;
            }
        }

        auto last_key = copy_first.last();
        writer(merge_blob(blob, it, end, last_key));
        copy_first.set_last(last_key);
        ++blobs_encoded;
    }

    while (it != end) {
        copy_first.push_back(*it);
        ++it;
    }

    progress_bar.done();
    osmium::io::detail::reliable_close(fd);

    m_vout << "Copied " << blobs_copied << " unchanged PBF blocks (" << blobs_not_decoded
           << " without decoding them), re-encoded " << blobs_encoded << " blocks ("
           << blobs_spliced << " of them copying unchanged objects)\n";
}

bool CommandApplyChanges::run() {
    if (m_locations_on_ways) {
        m_output_file.set("locations_on_ways");
    }

    // Copied blobs are ready to be written immediately, but in the output
    // queue they have to wait behind blocks still being encoded. A longer
    // queue lets reading and merging go on in the meantime.
    const char* no_passthrough_reason = why_no_passthrough();
    const osmium::io::output_queue_size output_queue_size{no_passthrough_reason ? 0U : 256U};

    m_vout << "Opening output file...\n";
    osmium::io::Writer writer{m_output_file, m_output_overwrite, m_fsync, output_queue_size};

    std::vector<osmium::memory::Buffer> changes;
    osmium::ObjectPointerCollection objects;

    if (m_change_file_format.empty() &&
        std::find(m_change_filenames.cbegin(), m_change_filenames.cend(), "-") != m_change_filenames.cend()) {
        throw argument_error{"When reading the change file from STDIN you have to use\n"
                             "the --change-file-format option to specify the file format."};
    }

    m_vout << "Reading change file contents...\n";

    // Several change files are read at the same time, but their contents are
    // used in the order of the command line, because later changes have to
    // win. STDIN is read in this thread when its turn comes.
    const std::size_t max_reads = std::clamp(std::thread::hardware_concurrency(), 1U, 8U);
    std::deque<std::future<std::vector<osmium::memory::Buffer>>> reads;
    auto next_file = m_change_filenames.cbegin();

    while (next_file != m_change_filenames.cend() || !reads.empty()) {
        while (next_file != m_change_filenames.cend() && reads.size() < max_reads) {
            const auto policy = *next_file == "-" ? std::launch::deferred : std::launch::async;
            reads.push_back(std::async(policy, read_change_file, osmium::io::File{*next_file, m_change_file_format}));
            ++next_file;
        }

        for (auto& buffer : reads.front().get()) {
            osmium::apply(buffer, objects);
            changes.push_back(std::move(buffer));
        }
        reads.pop_front();
    }

    // With passthrough the input file is read in apply_changes_with_passthrough()
    std::optional<osmium::io::ReaderWithProgressBar> reader;
    if (no_passthrough_reason) {
        m_vout << "Not copying unchanged blocks: " << no_passthrough_reason << "\n";
        m_vout << "Opening input file...\n";
        reader.emplace(display_progress(), m_input_file, osmium::osm_entity_bits::object);
    }

    osmium::io::Header header;
    setup_header(header);
    if (m_with_history) {
        header.set_has_multiple_object_versions(true);
    }
    writer.set_header(header);

    if (m_with_history) {
        // For history files this is a straightforward sort of the change
        // files followed by a merge with the input file.
        m_vout << "Sorting change data...\n";
        objects.sort(osmium::object_order_type_id_version());

        m_vout << "Applying changes and writing them to output...\n";
        const auto input = osmium::io::make_input_iterator_range<osmium::OSMObject>(*reader);
        auto out = osmium::io::make_output_iterator(writer);

        if (m_redact) {
            std::set_union(objects.begin(),
                           objects.end(),
                           input.begin(),
                           input.end(),
                           out,
                           osmium::object_order_type_id_version_without_timestamp());
        } else {
            std::set_union(objects.begin(),
                           objects.end(),
                           input.begin(),
                           input.end(),
                           out);
        }
    } else {
        // For normal data files we sort with the largest version of each
        // object first and then only copy this last version of any object
        // to the output.
        m_vout << "Sorting change data...\n";

        // This is needed for a special case: When change files have been
        // created from extracts it is possible that they contain objects with
        // the same type, id, version, and timestamp. In that case we still
        // want to get the last object available, because changes should be
        // applied in the order of the change files on the command line. So we
        // have to make sure it appears first in the objects vector before
        // doing the stable sort.
        std::reverse(objects.ptr_begin(), objects.ptr_end());
        objects.sort(osmium::object_order_type_id_reverse_version{});

        if (m_locations_on_ways) {
            apply_changes_and_write(objects, changes, *reader, writer);
        } else if (!no_passthrough_reason) {
            apply_changes_with_passthrough(objects, writer);
        } else {
            m_vout << "Applying changes and writing them to output...\n";
            const auto input = osmium::io::make_input_iterator_range<osmium::OSMObject>(*reader);
            copy_first_with_id copy_first{&writer};
            auto output_it = std::back_inserter(copy_first);

            std::set_union(objects.begin(),
                           objects.end(),
                           input.begin(),
                           input.end(),
                           output_it,
                           osmium::object_order_type_id_reverse_version());
        }
    }

    writer.close();
    if (reader) {
        reader->close();
    }

    show_memory_used();
    m_vout << "Done.\n";

    return true;
}


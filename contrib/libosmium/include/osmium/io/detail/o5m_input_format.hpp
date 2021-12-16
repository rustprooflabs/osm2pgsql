#ifndef OSMIUM_IO_DETAIL_O5M_INPUT_FORMAT_HPP
#define OSMIUM_IO_DETAIL_O5M_INPUT_FORMAT_HPP

/*

This file is part of Osmium (https://osmcode.org/libosmium).

Copyright 2013-2021 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/io/detail/input_format.hpp>
#include <osmium/io/detail/queue_util.hpp>
#include <osmium/io/error.hpp>
#include <osmium/io/file_format.hpp>
#include <osmium/io/header.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm/box.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/osm/item_type.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/node.hpp>
#include <osmium/osm/object.hpp>
#include <osmium/osm/relation.hpp>
#include <osmium/osm/timestamp.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/osm/way.hpp>
#include <osmium/thread/util.hpp>
#include <osmium/util/delta.hpp>

#include <protozero/exception.hpp>
#include <protozero/varint.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace osmium {

    namespace builder {
        class Builder;
    } // namespace builder

    /**
     * Exception thrown when the o5m deocder failed. The exception contains
     * (if available) information about the place where the error happened
     * and the type of error.
     */
    struct o5m_error : public io_error {

        explicit o5m_error(const char* what) :
            io_error(std::string{"o5m format error: "} + what) {
        }

    }; // struct o5m_error

    namespace io {

        namespace detail {

            // Implementation of the o5m/o5c file formats according to the
            // description at https://wiki.openstreetmap.org/wiki/O5m .

            class ReferenceTable {

                // The following settings are from the o5m description:

                enum : uint64_t {
                    // The maximum number of entries in this table.
                    number_of_entries = 15000UL,

                    // The size of one entry in the table.
                    entry_size = 256UL
                };

                // The maximum length of a string in the table including
                // two \0 bytes.
                enum {
                    max_length = 250U + 2U
                };

                // The data is stored in this string. It is default constructed
                // and then resized on demand the first time something is added.
                // This is done because the ReferenceTable is in a O5mParser
                // object which will be copied from one thread to another. This
                // way the string is still small when it is copied.
                std::string m_table;

                unsigned int current_entry = 0;

            public:

                void clear() {
                    current_entry = 0;
                }

                void add(const char* string, std::size_t size) {
                    assert(string);

                    if (m_table.empty()) {
                        m_table.resize(entry_size * number_of_entries);
                    }
                    if (size <= max_length) {
                        std::copy_n(string, size, &m_table[current_entry * entry_size]);
                        if (++current_entry == number_of_entries) {
                            current_entry = 0;
                        }
                    }
                }

                const char* get(uint64_t index) const {
                    if (m_table.empty() || index == 0 || index > number_of_entries) {
                        throw o5m_error{"reference to non-existing string in table"};
                    }
                    const auto entry = (current_entry + number_of_entries - index) % number_of_entries;
                    return &m_table[entry * entry_size];
                }

            }; // class ReferenceTable

            class O5mParser final : public ParserWithBuffer {

                osmium::io::Header m_header{};

                std::string m_input{};

                const char* m_data;
                const char* m_end;

                ReferenceTable m_reference_table;

                static int64_t zvarint(const char** data, const char* end) {
                    return protozero::decode_zigzag64(protozero::decode_varint(data, end));
                }

                bool ensure_bytes_available(std::size_t need_bytes) {
                    if (static_cast<std::size_t>(m_end - m_data) >= need_bytes) {
                        return true;
                    }

                    if (input_done() && (m_input.size() < need_bytes)) {
                        return false;
                    }

                    m_input.erase(0, m_data - m_input.data());

                    while (m_input.size() < need_bytes) {
                        const std::string data{get_input()};
                        if (input_done()) {
                            return false;
                        }
                        m_input.append(data);
                    }

                    m_data = m_input.data();
                    m_end = m_input.data() + m_input.size();

                    return true;
                }

                void check_header_magic() {
                    static const unsigned char header_magic[] = {0xff, 0xe0, 0x04, 'o', '5'};

                    if (std::strncmp(reinterpret_cast<const char*>(header_magic), m_data, sizeof(header_magic)) != 0) {
                        throw o5m_error{"wrong header magic"};
                    }

                    m_data += sizeof(header_magic);
                }

                void check_file_type() {
                    if (*m_data == 'm') {         // o5m data file
                        m_header.set_has_multiple_object_versions(false);
                    } else if (*m_data == 'c') {  // o5c change file
                        m_header.set_has_multiple_object_versions(true);
                    } else {
                        throw o5m_error{"wrong header magic"};
                    }

                    m_data++;
                }

                void check_file_format_version() {
                    if (*m_data != '2') {
                        throw o5m_error{"wrong header magic"};
                    }

                    m_data++;
                }

                void decode_header() {
                    if (!ensure_bytes_available(7)) { // overall length of header
                        throw o5m_error{"file too short (incomplete header info)"};
                    }

                    check_header_magic();
                    check_file_type();
                    check_file_format_version();
                }

                void mark_header_as_done() {
                    set_header_value(m_header);
                }

                osmium::DeltaDecode<osmium::object_id_type> m_delta_id;

                osmium::DeltaDecode<int64_t> m_delta_timestamp;
                osmium::DeltaDecode<osmium::changeset_id_type> m_delta_changeset;
                osmium::DeltaDecode<int64_t> m_delta_lon;
                osmium::DeltaDecode<int64_t> m_delta_lat;

                osmium::DeltaDecode<osmium::object_id_type> m_delta_way_node_id;
                std::array<osmium::DeltaDecode<osmium::object_id_type>, 3> m_delta_member_ids;

                void reset() {
                    m_reference_table.clear();

                    m_delta_id.clear();
                    m_delta_timestamp.clear();
                    m_delta_changeset.clear();
                    m_delta_lon.clear();
                    m_delta_lat.clear();

                    m_delta_way_node_id.clear();
                    m_delta_member_ids[0].clear();
                    m_delta_member_ids[1].clear();
                    m_delta_member_ids[2].clear();
                }

                const char* decode_string(const char** dataptr, const char* const end) {
                    assert(*dataptr != end);

                    if (**dataptr == 0x00) { // get inline string
                        ++(*dataptr);
                        if (*dataptr == end) {
                            throw o5m_error{"string format error"};
                        }
                        return *dataptr;
                    }
                    // get from reference table
                    const auto index = protozero::decode_varint(dataptr, end);
                    return m_reference_table.get(index);
                }

                std::pair<osmium::user_id_type, const char*> decode_user(const char** dataptr, const char* const end) {
                    assert(*dataptr != end);

                    const bool update_pointer = (**dataptr == 0x00);
                    const char* data = decode_string(dataptr, end);
                    const char* start = data;

                    const auto uid = protozero::decode_varint(&data, end);
                    if (uid > std::numeric_limits<user_id_type>::max()) {
                        throw o5m_error{"uid out of range"};
                    }

                    if (data == end) {
                        throw o5m_error{"missing user name"};
                    }

                    const char* user = ++data;

                    if (uid == 0 && update_pointer) {
                        m_reference_table.add("\0\0", 2);
                        *dataptr = data;
                        return {0, ""};
                    }

                    do {
                        if (data == end) {
                            throw o5m_error{"no null byte in user name"};
                        }
                    } while (*data++);

                    if (update_pointer) {
                        m_reference_table.add(start, data - start);
                        *dataptr = data;
                    }

                    return {static_cast<osmium::user_id_type>(uid), user};
                }

                void decode_tags(osmium::builder::Builder& parent, const char** dataptr, const char* const end) {
                    osmium::builder::TagListBuilder builder{parent};

                    while (*dataptr != end) {
                        const bool update_pointer = (**dataptr == 0x00);
                        const char* data = decode_string(dataptr, end);
                        const char* start = data;

                        while (*data++) {
                            if (data == end) {
                                throw o5m_error{"no null byte in tag key"};
                            }
                        }

                        if (data == end) {
                            throw o5m_error{"no null byte in tag value"};
                        }

                        const char* value = data;
                        while (*data++) {
                            if (data == end) {
                                throw o5m_error{"no null byte in tag value"};
                            }
                        }

                        if (update_pointer) {
                            m_reference_table.add(start, data - start);
                            *dataptr = data;
                        }

                        builder.add_tag(start, value);
                    }
                }

                const char* decode_info(osmium::OSMObject& object, const char** dataptr, const char* const end) {
                    const char* user = "";

                    if (*dataptr == end) {
                        throw o5m_error{"premature end of file while parsing object metadata"};
                    }

                    if (**dataptr == 0x00) { // no info section
                        ++*dataptr;
                    } else { // has info section
                        const auto version = protozero::decode_varint(dataptr, end);
                        if (version > std::numeric_limits<object_version_type>::max()) {
                            throw o5m_error{"object version too large"};
                        }
                        object.set_version(static_cast<object_version_type>(version));

                        const auto timestamp = m_delta_timestamp.update(zvarint(dataptr, end));
                        if (timestamp != 0) { // has timestamp
                            object.set_timestamp(timestamp);
                            object.set_changeset(m_delta_changeset.update(zvarint(dataptr, end)));
                            if (*dataptr != end) {
                                const auto uid_user = decode_user(dataptr, end);
                                object.set_uid(uid_user.first);
                                user = uid_user.second;
                            } else {
                                object.set_uid(user_id_type{0});
                            }
                        }
                    }

                    return user;
                }

                void decode_node(const char* data, const char* const end) {
                    osmium::builder::NodeBuilder builder{buffer()};

                    builder.set_id(m_delta_id.update(zvarint(&data, end)));

                    builder.set_user(decode_info(builder.object(), &data, end));

                    if (data == end) {
                        // no location, object is deleted
                        builder.set_visible(false);
                        builder.set_location(osmium::Location{});
                    } else {
                        const auto lon = m_delta_lon.update(zvarint(&data, end));
                        const auto lat = m_delta_lat.update(zvarint(&data, end));
                        builder.set_location(osmium::Location{lon, lat});

                        if (data != end) {
                            decode_tags(builder, &data, end);
                        }
                    }
                }

                void decode_way(const char* data, const char* const end) {
                    osmium::builder::WayBuilder builder{buffer()};

                    builder.set_id(m_delta_id.update(zvarint(&data, end)));

                    builder.set_user(decode_info(builder.object(), &data, end));

                    if (data == end) {
                        // no reference section, object is deleted
                        builder.set_visible(false);
                    } else {
                        const auto reference_section_length = protozero::decode_varint(&data, end);
                        if (reference_section_length > 0) {
                            const char* const end_refs = data + reference_section_length;
                            if (end_refs > end) {
                                throw o5m_error{"way nodes ref section too long"};
                            }

                            osmium::builder::WayNodeListBuilder wn_builder{builder};

                            while (data < end_refs) {
                                wn_builder.add_node_ref(m_delta_way_node_id.update(zvarint(&data, end)));
                            }
                        }

                        if (data != end) {
                            decode_tags(builder, &data, end);
                        }
                    }
                }

                static osmium::item_type decode_member_type(char c) {
                    if (c < '0' || c > '2') {
                        throw o5m_error{"unknown member type"};
                    }
                    return osmium::nwr_index_to_item_type(c - '0');
                }

                std::pair<osmium::item_type, const char*> decode_role(const char** dataptr, const char* const end) {
                    assert(*dataptr != end);

                    const bool update_pointer = (**dataptr == 0x00);
                    const char* data = decode_string(dataptr, end);
                    const char* start = data;

                    const auto member_type = decode_member_type(*data++);
                    if (data == end) {
                        throw o5m_error{"missing role"};
                    }
                    const char* role = data;

                    while (*data++) {
                        if (data == end) {
                            throw o5m_error{"no null byte in role"};
                        }
                    }

                    if (update_pointer) {
                        m_reference_table.add(start, data - start);
                        *dataptr = data;
                    }

                    return {member_type, role};
                }

                void decode_relation(const char* data, const char* const end) {
                    osmium::builder::RelationBuilder builder{buffer()};

                    builder.set_id(m_delta_id.update(zvarint(&data, end)));

                    builder.set_user(decode_info(builder.object(), &data, end));

                    if (data == end) {
                        // no reference section, object is deleted
                        builder.set_visible(false);
                    } else {
                        const auto reference_section_length = protozero::decode_varint(&data, end);
                        if (reference_section_length > 0) {
                            const char* const end_refs = data + reference_section_length;
                            if (end_refs > end) {
                                throw o5m_error{"relation format error"};
                            }

                            osmium::builder::RelationMemberListBuilder rml_builder{builder};

                            while (data < end_refs) {
                                const auto delta_id = zvarint(&data, end);
                                if (data == end) {
                                    throw o5m_error{"relation member format error"};
                                }
                                const auto type_role = decode_role(&data, end);
                                const auto i = osmium::item_type_to_nwr_index(type_role.first);
                                const auto ref = m_delta_member_ids[i].update(delta_id);
                                rml_builder.add_member(type_role.first, ref, type_role.second);
                            }
                        }

                        if (data != end) {
                            decode_tags(builder, &data, end);
                        }
                    }
                }

                void decode_bbox(const char* data, const char* const end) {
                    const auto sw_lon = zvarint(&data, end);
                    const auto sw_lat = zvarint(&data, end);
                    const auto ne_lon = zvarint(&data, end);
                    const auto ne_lat = zvarint(&data, end);

                    m_header.add_box(osmium::Box{osmium::Location{sw_lon, sw_lat},
                                                 osmium::Location{ne_lon, ne_lat}});
                }

                void decode_timestamp(const char* data, const char* const end) {
                    const auto timestamp = osmium::Timestamp{zvarint(&data, end)}.to_iso();
                    m_header.set("o5m_timestamp", timestamp);
                    m_header.set("timestamp", timestamp);
                }

                enum class dataset_type : unsigned char {
                    node         = 0x10,
                    way          = 0x11,
                    relation     = 0x12,
                    bounding_box = 0xdb,
                    timestamp    = 0xdc,
                    header       = 0xe0,
                    sync         = 0xee,
                    jump         = 0xef,
                    reset        = 0xff
                };

                void decode_data() {
                    while (ensure_bytes_available(1)) {
                        const auto ds_type = static_cast<dataset_type>(*m_data++);
                        if (ds_type > dataset_type::jump) {
                            if (ds_type == dataset_type::reset) {
                                reset();
                            }
                        } else {
                            ensure_bytes_available(protozero::max_varint_length);

                            uint64_t length = 0;
                            try {
                                length = protozero::decode_varint(&m_data, m_end);
                            } catch (const protozero::end_of_buffer_exception&) {
                                throw o5m_error{"premature end of file"};
                            }

                            if (!ensure_bytes_available(length)) {
                                throw o5m_error{"premature end of file"};
                            }

                            switch (ds_type) {
                                case dataset_type::node:
                                    mark_header_as_done();
                                    if (read_types() & osmium::osm_entity_bits::node) {
                                        maybe_new_buffer(osmium::item_type::node);
                                        decode_node(m_data, m_data + length);
                                        buffer().commit();
                                    }
                                    break;
                                case dataset_type::way:
                                    mark_header_as_done();
                                    if (read_types() & osmium::osm_entity_bits::way) {
                                        maybe_new_buffer(osmium::item_type::way);
                                        decode_way(m_data, m_data + length);
                                        buffer().commit();
                                    }
                                    break;
                                case dataset_type::relation:
                                    mark_header_as_done();
                                    if (read_types() & osmium::osm_entity_bits::relation) {
                                        maybe_new_buffer(osmium::item_type::relation);
                                        decode_relation(m_data, m_data + length);
                                        buffer().commit();
                                    }
                                    break;
                                case dataset_type::bounding_box:
                                    decode_bbox(m_data, m_data + length);
                                    break;
                                case dataset_type::timestamp:
                                    decode_timestamp(m_data, m_data + length);
                                    break;
                                default:
                                    // ignore unknown datasets
                                    break;
                            }

                            if (read_types() == osmium::osm_entity_bits::nothing && header_is_done()) {
                                break;
                            }

                            m_data += length;

                            flush_nested_buffer();
                        }
                    }

                    mark_header_as_done();
                    flush_final_buffer();
                }

            public:

                explicit O5mParser(parser_arguments& args) :
                    ParserWithBuffer(args),
                    m_data(m_input.data()),
                    m_end(m_data) {
                }

                O5mParser(const O5mParser&) = delete;
                O5mParser& operator=(const O5mParser&) = delete;

                O5mParser(O5mParser&&) = delete;
                O5mParser& operator=(O5mParser&&) = delete;

                ~O5mParser() noexcept override = default;

                void run() override {
                    osmium::thread::set_thread_name("_osmium_o5m_in");

                    decode_header();
                    decode_data();
                }

            }; // class O5mParser

            // we want the register_parser() function to run, setting
            // the variable is only a side-effect, it will never be used
            const bool registered_o5m_parser = ParserFactory::instance().register_parser(
                file_format::o5m,
                [](parser_arguments& args) {
                    return std::unique_ptr<Parser>(new O5mParser{args});
                });

            // dummy function to silence the unused variable warning from above
            inline bool get_registered_o5m_parser() noexcept {
                return registered_o5m_parser;
            }

        } // namespace detail

    } // namespace io

} // namespace osmium

#endif // OSMIUM_IO_DETAIL_O5M_INPUT_FORMAT_HPP

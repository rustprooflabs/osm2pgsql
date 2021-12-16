#ifndef OSMIUM_MEMORY_BUFFER_HPP
#define OSMIUM_MEMORY_BUFFER_HPP

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

#include <osmium/memory/item.hpp>
#include <osmium/memory/item_iterator.hpp>
#include <osmium/osm/entity.hpp>
#include <osmium/util/compatibility.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <utility>

namespace osmium {

    /**
     * Exception thrown by the osmium::memory::Buffer class when somebody tries
     * to write data into a buffer and it doesn't fit. Buffers with internal
     * memory management will not throw this exception, but increase their size.
     */
    struct OSMIUM_EXPORT buffer_is_full : public std::runtime_error {

        buffer_is_full() :
            std::runtime_error{"Osmium buffer is full"} {
        }

    }; // struct buffer_is_full

    /**
     * @brief Memory management of items in buffers and iterators over this data.
     */
    namespace memory {

        /**
         * A memory area for storing OSM objects and other items. Each item stored
         * has a type and a length. See the Item class for details.
         *
         * Data can be added to a buffer piece by piece using reserve_space() and
         * add_item(). After all data that together forms an item is added, it must
         * be committed using the commit() call. Usually this is done through the
         * Builder class and its derived classes.
         *
         * You can iterate over all items in a buffer using the iterators returned
         * by begin(), end(), cbegin(), and cend().
         *
         * Buffers exist in two flavours, those with external memory management and
         * those with internal memory management. If you already have some memory
         * with data in it (for instance read from disk), you create a Buffer with
         * external memory management. It is your job then to free the memory once
         * the buffer isn't used any more. If you don't have memory already, you can
         * create a Buffer object and have it manage the memory internally. It will
         * dynamically allocate memory and free it again after use.
         *
         * By default, if a buffer gets full it will throw a buffer_is_full exception.
         * You can use the set_full_callback() method to set a callback functor
         * which will be called instead of throwing an exception. The full
         * callback functionality is deprecated and will be removed in the
         * future. See the documentation for set_full_callback() for alternatives.
         */
        class Buffer {

        public:

            // This is needed so we can call std::back_inserter() on a Buffer.
            using value_type = Item;

            enum class auto_grow {
                no       = 0,
                yes      = 1,
                internal = 2
            }; // enum class auto_grow

        private:

            std::unique_ptr<Buffer> m_next_buffer;
            std::unique_ptr<unsigned char[]> m_memory{};
            unsigned char* m_data = nullptr;
            std::size_t m_capacity = 0;
            std::size_t m_written = 0;
            std::size_t m_committed = 0;
#ifndef NDEBUG
            uint8_t m_builder_count = 0;
#endif
            auto_grow m_auto_grow{auto_grow::no};
            std::function<void(Buffer&)> m_full;

            static std::size_t calculate_capacity(std::size_t capacity) noexcept {
                enum {
                    // The majority of all Nodes will fit into this size.
                    min_capacity = 64
                };

                if (capacity < min_capacity) {
                    return min_capacity;
                }
                return padded_length(capacity);
            }

            void grow_internal() {
                assert(m_data && "This must be a valid buffer");
                if (!m_memory) {
                    throw std::logic_error{"Can't grow Buffer if it doesn't use internal memory management."};
                }

                std::unique_ptr<Buffer> old{new Buffer{std::move(m_memory), m_capacity, m_committed}};
                m_memory = std::unique_ptr<unsigned char[]>{new unsigned char[m_capacity]};
                m_data = m_memory.get();

                m_written -= m_committed;
                std::copy_n(old->data() + m_committed, m_written, m_data);
                m_committed = 0;

                old->m_next_buffer = std::move(m_next_buffer);
                m_next_buffer = std::move(old);
            }

        public:

            /**
             * The constructor without any parameters creates an invalid,
             * buffer, ie an empty hull of a buffer that has no actual memory
             * associated with it. It can be used to signify end-of-data.
             *
             * Most methods of the Buffer class will not work with an invalid
             * buffer.
             */
            Buffer() noexcept = default;

            /**
             * Constructs a valid externally memory-managed buffer using the
             * given memory and size.
             *
             * @param data A pointer to some already initialized data.
             * @param size The size of the initialized data.
             *
             * @throws std::invalid_argument if the size isn't a multiple of
             *         the alignment.
             */
            explicit Buffer(unsigned char* data, std::size_t size) :
                m_data(data),
                m_capacity(size),
                m_written(size),
                m_committed(size) {
                if (size % align_bytes != 0) {
                    throw std::invalid_argument{"buffer size needs to be multiple of alignment"};
                }
            }

            /**
             * Constructs a valid externally memory-managed buffer with the
             * given capacity that already contains 'committed' bytes of data.
             *
             * @param data A pointer to some (possibly initialized) data.
             * @param capacity The size of the memory for this buffer.
             * @param committed The size of the initialized data. If this is 0, the buffer startes out empty.
             *
             * @throws std::invalid_argument if the capacity or committed isn't
             *         a multiple of the alignment or if committed is larger
             *         than capacity.
             */
            explicit Buffer(unsigned char* data, std::size_t capacity, std::size_t committed) :
                m_data(data),
                m_capacity(capacity),
                m_written(committed),
                m_committed(committed) {
                if (capacity % align_bytes != 0) {
                    throw std::invalid_argument{"buffer capacity needs to be multiple of alignment"};
                }
                if (committed % align_bytes != 0) {
                    throw std::invalid_argument{"buffer parameter 'committed' needs to be multiple of alignment"};
                }
                if (committed > capacity) {
                    throw std::invalid_argument{"buffer parameter 'committed' can not be larger than capacity"};
                }
            }

            /**
             * Constructs a valid internally memory-managed buffer with the
             * given capacity that already contains 'committed' bytes of data.
             *
             * @param data A unique pointer to some (possibly initialized) data.
             *             The Buffer will manage this memory.
             * @param capacity The size of the memory for this buffer.
             * @param committed The size of the initialized data. If this is 0, the buffer startes out empty.
             *
             * @throws std::invalid_argument if the capacity or committed isn't
             *         a multiple of the alignment or if committed is larger
             *         than capacity.
             */
            explicit Buffer(std::unique_ptr<unsigned char[]> data, std::size_t capacity, std::size_t committed) :
                m_memory(std::move(data)),
                m_data(m_memory.get()),
                m_capacity(capacity),
                m_written(committed),
                m_committed(committed) {
                if (capacity % align_bytes != 0) {
                    throw std::invalid_argument{"buffer capacity needs to be multiple of alignment"};
                }
                if (committed % align_bytes != 0) {
                    throw std::invalid_argument{"buffer parameter 'committed' needs to be multiple of alignment"};
                }
                if (committed > capacity) {
                    throw std::invalid_argument{"buffer parameter 'committed' can not be larger than capacity"};
                }
            }

            /**
             * Constructs a valid internally memory-managed buffer with the
             * given capacity.
             * Will internally get dynamic memory of the required size.
             * The dynamic memory will be automatically freed when the Buffer
             * is destroyed.
             *
             * @param capacity The (initial) size of the memory for this buffer.
             *        Actual capacity might be larger tue to alignment.
             * @param auto_grow Should this buffer automatically grow when it
             *        becomes to small?
             */
            explicit Buffer(std::size_t capacity, auto_grow auto_grow = auto_grow::yes) :
                m_memory(new unsigned char[calculate_capacity(capacity)]),
                m_data(m_memory.get()),
                m_capacity(calculate_capacity(capacity)),
                m_auto_grow(auto_grow) {
            }

            // buffers can not be copied
            Buffer(const Buffer&) = delete;
            Buffer& operator=(const Buffer&) = delete;

            // buffers can be moved
            Buffer(Buffer&& other) noexcept :
                m_next_buffer(std::move(other.m_next_buffer)),
                m_memory(std::move(other.m_memory)),
                m_data(other.m_data),
                m_capacity(other.m_capacity),
                m_written(other.m_written),
                m_committed(other.m_committed),
#ifndef NDEBUG
                m_builder_count(other.m_builder_count),
#endif
                m_auto_grow(other.m_auto_grow),
                m_full(std::move(other.m_full)) {
                other.m_data = nullptr;
                other.m_capacity = 0;
                other.m_written = 0;
                other.m_committed = 0;
#ifndef NDEBUG
                other.m_builder_count = 0;
#endif
            }

            Buffer& operator=(Buffer&& other) noexcept {
                m_next_buffer = std::move(other.m_next_buffer);
                m_memory = std::move(other.m_memory);
                m_data = other.m_data;
                m_capacity = other.m_capacity;
                m_written = other.m_written;
                m_committed = other.m_committed;
#ifndef NDEBUG
                m_builder_count = other.m_builder_count;
#endif
                m_auto_grow = other.m_auto_grow;
                m_full = std::move(other.m_full);
                other.m_data = nullptr;
                other.m_capacity = 0;
                other.m_written = 0;
                other.m_committed = 0;
#ifndef NDEBUG
                other.m_builder_count = 0;
#endif
                return *this;
            }

            ~Buffer() noexcept = default;

#ifndef NDEBUG
            void increment_builder_count() noexcept {
                ++m_builder_count;
            }

            void decrement_builder_count() noexcept {
                assert(m_builder_count > 0);
                --m_builder_count;
            }

            uint8_t builder_count() const noexcept {
                return m_builder_count;
            }
#endif

            /**
             * Return a pointer to data inside the buffer.
             *
             * @pre The buffer must be valid.
             */
            unsigned char* data() const noexcept {
                assert(m_data && "This must be a valid buffer");
                return m_data;
            }

            /**
             * Returns the capacity of the buffer, ie how many bytes it can
             * contain. Always returns 0 on invalid buffers.
             */
            std::size_t capacity() const noexcept {
                return m_capacity;
            }

            /**
             * Returns the number of bytes already filled in this buffer.
             * Always returns 0 on invalid buffers.
             */
            std::size_t committed() const noexcept {
                return m_committed;
            }

            /**
             * Returns the number of bytes currently filled in this buffer that
             * are not yet committed.
             * Always returns 0 on invalid buffers.
             */
            std::size_t written() const noexcept {
                return m_written;
            }

            /**
             * This tests if the current state of the buffer is aligned
             * properly. Can be used for asserts.
             *
             * @pre The buffer must be valid.
             */
            bool is_aligned() const noexcept {
                assert(m_data && "This must be a valid buffer");
                return (m_written % align_bytes == 0) && (m_committed % align_bytes == 0);
            }

            /**
             * Set functor to be called whenever the buffer is full
             * instead of throwing buffer_is_full.
             *
             * The behaviour is undefined if you call this on an invalid
             * buffer.
             *
             * @pre The buffer must be valid.
             *
             * @deprecated
             * Callback functionality will be removed in the future. Either
             * detect the buffer_is_full exception or use a buffer with
             * auto_grow::yes. If you want to avoid growing buffers, check
             * the CallbackBuffer class.
             */
            OSMIUM_DEPRECATED void set_full_callback(const std::function<void(Buffer&)>& full) {
                assert(m_data && "This must be a valid buffer");
                m_full = full;
            }

            /**
             * Grow capacity of this buffer to the given size (which will be
             * rounded up to the alignment needed).
             * This works only with internally memory-managed buffers.
             * If the given size is not larger than the current capacity,
             * nothing is done.
             *
             * @pre The buffer must be valid.
             *
             * @param size New capacity.
             *
             * @throws std::logic_error if the buffer doesn't use internal
             *         memory management.
             * @throws std::bad_alloc if there isn't enough memory available.
             */
            void grow(std::size_t size) {
                assert(m_data && "This must be a valid buffer");
                if (!m_memory) {
                    throw std::logic_error{"Can't grow Buffer if it doesn't use internal memory management."};
                }
                size = calculate_capacity(size);
                if (m_capacity < size) {
                    std::unique_ptr<unsigned char[]> memory{new unsigned char[size]};
                    std::copy_n(m_memory.get(), m_capacity, memory.get());
                    using std::swap;
                    swap(m_memory, memory);
                    m_data = m_memory.get();
                    m_capacity = size;
                }
            }

            /**
             * Does this buffer have nested buffers inside. This happens
             * when a buffer is full and auto_grow is defined as internal.
             *
             * @returns Are there nested buffers or not?
             */
            bool has_nested_buffers() const noexcept {
                return m_next_buffer != nullptr;
            }

            /**
             * Return the most deeply nested buffer. The buffer will be moved
             * out.
             *
             * @pre has_nested_buffers()
             */
            std::unique_ptr<Buffer> get_last_nested() {
                assert(has_nested_buffers());
                Buffer* buffer = this;
                while (buffer->m_next_buffer->has_nested_buffers()) {
                    buffer = buffer->m_next_buffer.get();
                }
                return std::move(buffer->m_next_buffer);
            }

            /**
             * Mark currently written bytes in the buffer as committed.
             *
             * @pre The buffer must be valid.
             * @pre The buffer must be aligned properly (as indicated
             *      by is_aligned().
             * @pre No builder can be open on this buffer.
             *
             * @returns Number of committed bytes before this commit. Can be
             *          used as an offset into the buffer to get to the
             *          object being committed by this call.
             */
            std::size_t commit() {
                assert(m_data && "This must be a valid buffer");
                assert(m_builder_count == 0 && "Make sure there are no Builder objects still in scope");
                assert(is_aligned());

                const std::size_t offset = m_committed;
                m_committed = m_written;
                return offset;
            }

            /**
             * Roll back changes in buffer to last committed state.
             *
             * @pre The buffer must be valid.
             * @pre No builder can be open on this buffer.
             */
            void rollback() {
                assert(m_data && "This must be a valid buffer");
                assert(m_builder_count == 0 && "Make sure there are no Builder objects still in scope");
                m_written = m_committed;
            }

            /**
             * Clear the buffer.
             *
             * No-op on an invalid buffer.
             *
             * @pre No builder can be open on this buffer.
             *
             * @returns Number of bytes in the buffer before it was cleared.
             */
            std::size_t clear() {
                assert(m_builder_count == 0 && "Make sure there are no Builder objects still in scope");
                const std::size_t num_used_bytes = m_committed;
                m_written = 0;
                m_committed = 0;
                return num_used_bytes;
            }

            /**
             * Get the data in the buffer at the given offset.
             *
             * @pre The buffer must be valid.
             *
             * @tparam T Type we want to the data to be interpreted as.
             *
             * @returns Reference of given type pointing to the data in the
             *          buffer.
             */
            template <typename T>
            T& get(const std::size_t offset) const {
                assert(m_data && "This must be a valid buffer");
                assert(offset % alignof(T) == 0 && "Wrong alignment");
                return *reinterpret_cast<T*>(&m_data[offset]);
            }

            /**
             * Reserve space of given size in buffer and return pointer to it.
             * This is the only way of adding data to the buffer. You reserve
             * the space and then fill it.
             *
             * Note that you have to eventually call commit() to actually
             * commit this data.
             *
             * If there isn't enough space in the buffer, one of three things
             * can happen:
             *
             * * If you have set a callback with set_full_callback(), it is
             *   called. After the call returns, you must have either grown
             *   the buffer or cleared it by calling buffer.clear(). (Usage
             *   of the full callback is deprecated and this functionality
             *   will be removed in the future. See the documentation for
             *   set_full_callback() for alternatives.
             * * If no callback is defined and this buffer uses internal
             *   memory management, the buffers capacity is grown, so that
             *   the new data will fit.
             * * Else the buffer_is_full exception is thrown.
             *
             * @pre The buffer must be valid.
             *
             * @param size Number of bytes to reserve.
             *
             * @returns Pointer to reserved space. Note that this pointer is
             *          only guaranteed to be valid until the next call to
             *          reserve_space().
             *
             * @throws osmium::buffer_is_full if the buffer is full there is
             *         no callback defined and the buffer isn't auto-growing.
             */
            unsigned char* reserve_space(const std::size_t size) {
                assert(m_data && "This must be a valid buffer");
                // try to flush the buffer empty first.
                if (m_written + size > m_capacity && m_full) {
                    m_full(*this);
                }
                // if there's still not enough space, then try growing the buffer.
                if (m_written + size > m_capacity) {
                    if (!m_memory || m_auto_grow == auto_grow::no) {
                        throw osmium::buffer_is_full{};
                    }
                    if (m_auto_grow == auto_grow::internal && m_committed != 0) {
                        grow_internal();
                    }
                    if (m_written + size > m_capacity) {
                        // double buffer size until there is enough space
                        std::size_t new_capacity = m_capacity * 2;
                        while (m_written + size > new_capacity) {
                            new_capacity *= 2;
                        }
                        grow(new_capacity);
                    }
                }
                unsigned char* reserved_space = &m_data[m_written];
                m_written += size;
                return reserved_space;
            }

            /**
             * Add an item to the buffer. The size of the item is stored inside
             * the item, so we know how much memory to copy.
             *
             * Note that you have to eventually call commit() to actually
             * commit this data.
             *
             * @pre The buffer must be valid.
             *
             * @tparam T Class of the item to be copied.
             *
             * @param item Reference to the item to be copied.
             *
             * @returns Reference to newly copied data in the buffer.
             */
            template <typename T>
            T& add_item(const T& item) {
                assert(m_data && "This must be a valid buffer");
                unsigned char* target = reserve_space(item.padded_size());
                std::copy_n(reinterpret_cast<const unsigned char*>(&item), item.padded_size(), target);
                return *reinterpret_cast<T*>(target);
            }

            /**
             * Add committed contents of the given buffer to this buffer.
             *
             * @pre The buffer must be valid.
             * @pre No builder can be open on this buffer.
             *
             * Note that you have to eventually call commit() to actually
             * commit this data.
             *
             * @param buffer The source of the copy. Must be valid.
             */
            void add_buffer(const Buffer& buffer) {
                assert(m_data && "This must be a valid buffer");
                assert(buffer && "Buffer parameter must be a valid buffer");
                assert(m_builder_count == 0 && "Make sure there are no Builder objects still in scope");
                unsigned char* target = reserve_space(buffer.committed());
                std::copy_n(buffer.data(), buffer.committed(), target);
            }

            /**
             * Add an item to the buffer. This function is provided so that
             * you can use std::back_inserter.
             *
             * @pre The buffer must be valid.
             * @pre No builder can be open on this buffer.
             *
             * @param item The item to be added.
             */
            void push_back(const osmium::memory::Item& item) {
                assert(m_data && "This must be a valid buffer");
                assert(m_builder_count == 0 && "Make sure there are no Builder objects still in scope");
                add_item(item);
                commit();
            }

            /**
             * An iterator that can be used to iterate over all items of
             * type T in a buffer.
             */
            template <typename T>
            using t_iterator = osmium::memory::ItemIterator<T>;

            /**
             * A const iterator that can be used to iterate over all items of
             * type T in a buffer.
             */
            template <typename T>
            using t_const_iterator = osmium::memory::ItemIterator<const T>;

            /**
             * An iterator that can be used to iterate over all OSMEntity
             * objects in a buffer.
             */
            using iterator = t_iterator<osmium::OSMEntity>;

            /**
             * A const iterator that can be used to iterate over all OSMEntity
             * objects in a buffer.
             */
            using const_iterator = t_const_iterator<osmium::OSMEntity>;

            template <typename T>
            ItemIteratorRange<T> select() {
                return ItemIteratorRange<T>{m_data, m_data + m_committed};
            }

            template <typename T>
            ItemIteratorRange<const T> select() const {
                return ItemIteratorRange<const T>{m_data, m_data + m_committed};
            }

            /**
             * Get iterator for iterating over all items of type T in the
             * buffer.
             *
             * @pre The buffer must be valid.
             *
             * @returns Iterator to first item of type T in the buffer.
             */
            template <typename T>
            t_iterator<T> begin() {
                assert(m_data && "This must be a valid buffer");
                return t_iterator<T>(m_data, m_data + m_committed);
            }

            /**
             * Get iterator for iterating over all objects of class OSMEntity
             * in the buffer.
             *
             * @pre The buffer must be valid.
             *
             * @returns Iterator to first OSMEntity in the buffer.
             */
            iterator begin() {
                assert(m_data && "This must be a valid buffer");
                return {m_data, m_data + m_committed};
            }

            /**
             * Get iterator for iterating over all items of type T in the
             * buffer.
             *
             * @pre The buffer must be valid.
             *
             * @returns Iterator to first item of type T after given offset
             *          in the buffer.
             */
            template <typename T>
            t_iterator<T> get_iterator(std::size_t offset) {
                assert(m_data && "This must be a valid buffer");
                assert(offset % alignof(T) == 0 && "Wrong alignment");
                return {m_data + offset, m_data + m_committed};
            }

            /**
             * Get iterator for iterating over all objects of class OSMEntity
             * in the buffer.
             *
             * @pre The buffer must be valid.
             *
             * @returns Iterator to first OSMEntity after given offset in the
             *          buffer.
             */
            iterator get_iterator(std::size_t offset) {
                assert(m_data && "This must be a valid buffer");
                assert(offset % alignof(OSMEntity) == 0 && "Wrong alignment");
                return {m_data + offset, m_data + m_committed};
            }

            /**
             * Get iterator for iterating over all items of type T in the
             * buffer.
             *
             * @pre The buffer must be valid.
             *
             * @returns End iterator.
             */
            template <typename T>
            t_iterator<T> end() {
                assert(m_data && "This must be a valid buffer");
                return {m_data + m_committed, m_data + m_committed};
            }

            /**
             * Get iterator for iterating over all objects of class OSMEntity
             * in the buffer.
             *
             * @pre The buffer must be valid.
             *
             * @returns End iterator.
             */
            iterator end() {
                assert(m_data && "This must be a valid buffer");
                return {m_data + m_committed, m_data + m_committed};
            }

            template <typename T>
            t_const_iterator<T> cbegin() const {
                assert(m_data && "This must be a valid buffer");
                return {m_data, m_data + m_committed};
            }

            const_iterator cbegin() const {
                assert(m_data && "This must be a valid buffer");
                return {m_data, m_data + m_committed};
            }

            template <typename T>
            t_const_iterator<T> get_iterator(std::size_t offset) const {
                assert(m_data && "This must be a valid buffer");
                assert(offset % alignof(T) == 0 && "Wrong alignment");
                return {m_data + offset, m_data + m_committed};
            }

            const_iterator get_iterator(std::size_t offset) const {
                assert(m_data && "This must be a valid buffer");
                assert(offset % alignof(OSMEntity) == 0 && "Wrong alignment");
                return {m_data + offset, m_data + m_committed};
            }

            template <typename T>
            t_const_iterator<T> cend() const {
                assert(m_data && "This must be a valid buffer");
                return {m_data + m_committed, m_data + m_committed};
            }

            const_iterator cend() const {
                assert(m_data && "This must be a valid buffer");
                return {m_data + m_committed, m_data + m_committed};
            }

            template <typename T>
            t_const_iterator<T> begin() const {
                return cbegin<T>();
            }

            const_iterator begin() const {
                return cbegin();
            }

            template <typename T>
            t_const_iterator<T> end() const {
                return cend<T>();
            }

            const_iterator end() const {
                return cend();
            }

            /**
             * In a bool context any valid buffer is true.
             */
            explicit operator bool() const noexcept {
                return m_data != nullptr;
            }

            void swap(Buffer& other) {
                using std::swap;

                swap(m_next_buffer, other.m_next_buffer);
                swap(m_memory, other.m_memory);
                swap(m_data, other.m_data);
                swap(m_capacity, other.m_capacity);
                swap(m_written, other.m_written);
                swap(m_committed, other.m_committed);
                swap(m_auto_grow, other.m_auto_grow);
                swap(m_full, other.m_full);
            }

            /**
             * Purge removed items from the buffer. This is done by moving all
             * non-removed items forward in the buffer overwriting removed
             * items and then correcting the m_written and m_committed numbers.
             *
             * Note that calling this function invalidates all iterators on
             * this buffer and all offsets in this buffer.
             *
             * For every non-removed item that moves its position, the function
             * 'moving_in_buffer' is called on the given callback object with
             * the old and new offsets in the buffer where the object used to
             * be and is now, respectively. This call can be used to update any
             * indexes.
             *
             * @pre The buffer must be valid.
             * @pre @code callback != nullptr @endptr
             */
            template <typename TCallbackClass>
            void purge_removed(TCallbackClass* callback) {
                assert(m_data && "This must be a valid buffer");
                assert(callback);

                if (begin() == end()) {
                    return;
                }

                iterator it_write = begin();

                iterator next;
                for (iterator it_read = begin(); it_read != end(); it_read = next) {
                    next = std::next(it_read);
                    if (!it_read->removed()) {
                        if (it_read != it_write) {
                            assert(it_read.data() >= data());
                            assert(it_write.data() >= data());
                            const auto old_offset = static_cast<std::size_t>(it_read.data() - data());
                            const auto new_offset = static_cast<std::size_t>(it_write.data() - data());
                            callback->moving_in_buffer(old_offset, new_offset);
                            std::memmove(it_write.data(), it_read.data(), it_read->padded_size());
                        }
                        it_write.advance_once();
                    }
                }

                assert(it_write.data() >= data());
                m_written = static_cast<std::size_t>(it_write.data() - data());
                m_committed = m_written;
            }

            /**
             * Purge removed items from the buffer. This is done by moving all
             * non-removed items forward in the buffer overwriting removed
             * items and then correcting the m_written and m_committed numbers.
             *
             * Note that calling this function invalidates all iterators on
             * this buffer and all offsets in this buffer.
             *
             * @pre The buffer must be valid.
             */
            void purge_removed() {
                assert(m_data && "This must be a valid buffer");
                if (begin() == end()) {
                    return;
                }

                iterator it_write = begin();

                iterator next;
                for (iterator it_read = begin(); it_read != end(); it_read = next) {
                    next = std::next(it_read);
                    if (!it_read->removed()) {
                        if (it_read != it_write) {
                            assert(it_read.data() >= data());
                            assert(it_write.data() >= data());
                            std::memmove(it_write.data(), it_read.data(), it_read->padded_size());
                        }
                        it_write.advance_once();
                    }
                }

                assert(it_write.data() >= data());
                m_written = static_cast<std::size_t>(it_write.data() - data());
                m_committed = m_written;
            }

        }; // class Buffer

        inline void swap(Buffer& lhs, Buffer& rhs) {
            lhs.swap(rhs);
        }

        /**
         * Compare two buffers for equality.
         *
         * Buffers are equal if they are both invalid or if they are both
         * valid and have the same data pointer, capacity and committed
         * data.
         */
        inline bool operator==(const Buffer& lhs, const Buffer& rhs) noexcept {
            if (!lhs || !rhs) {
                return !lhs && !rhs;
            }
            return lhs.data() == rhs.data() && lhs.capacity() == rhs.capacity() && lhs.committed() == rhs.committed();
        }

        inline bool operator!=(const Buffer& lhs, const Buffer& rhs) noexcept {
            return !(lhs == rhs);
        }

    } // namespace memory

} // namespace osmium

#endif // OSMIUM_MEMORY_BUFFER_HPP

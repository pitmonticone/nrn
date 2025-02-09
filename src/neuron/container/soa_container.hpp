#pragma once
#include "backtrace_utils.h"
#include "neuron/container/data_handle.hpp"
#include "neuron/container/generic_data_handle.hpp"
#include "neuron/container/soa_identifier.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace neuron::container {
namespace detail {
template <typename T, typename... Ts>
inline constexpr bool type_in_pack_v = std::disjunction_v<std::is_same<T, Ts>...>;
// https://stackoverflow.com/a/67106430
template <typename T, typename... Types>
inline constexpr bool are_types_unique_v =
    (!std::is_same_v<T, Types> && ...) && are_types_unique_v<Types...>;
template <typename T>
inline constexpr bool are_types_unique_v<T> = true;
// https://stackoverflow.com/a/18063608
template <typename T, typename Tuple>
struct index_of_type_helper;
template <typename T, typename... Ts>
struct index_of_type_helper<T, std::tuple<T, Ts...>> {
    static constexpr std::size_t value = 0;
};
template <typename T, typename U, typename... Ts>
struct index_of_type_helper<T, std::tuple<U, Ts...>> {
    static constexpr std::size_t value = 1 + index_of_type_helper<T, std::tuple<Ts...>>::value;
};
template <typename T, typename... Ts>
inline constexpr std::size_t index_of_type_v = []() {
    constexpr bool Ts_are_unique = are_types_unique_v<Ts...>;
    constexpr bool T_is_in_Ts = type_in_pack_v<T, Ts...>;
    static_assert(Ts_are_unique,
                  "index_of_type_v<T, Ts...> assumes there are no duplicates in Ts...");
    static_assert(T_is_in_Ts, "index_of_type_v<T, Ts...> assumes that T occurs in Ts...");
    // make the error message better by avoiding instantiating index_of_type_helper if the
    // assertions fail
    if constexpr (Ts_are_unique && T_is_in_Ts) {
        return index_of_type_helper<T, std::tuple<Ts...>>::value;
    } else {
        return std::numeric_limits<std::size_t>::max();  // unreachable without hitting
                                                         // static_assert
    }
}();

// Detect if a type T has a non-static member function called default_value
template <typename T, typename = void>
inline constexpr bool has_default_value_v = false;
template <typename T>
inline constexpr bool
    has_default_value_v<T, std::void_t<decltype(std::declval<T>().default_value())>> = true;

// Get the array dimension for a given field within a given tag, or 1 if the array_dimension
// function is not defined in the tag type
template <typename T>
auto get_array_dimension(T const& t, std::nullptr_t /* higher precedence than the fallback case */)
    -> decltype(t.array_dimension(), 0) {
    return t.array_dimension();
}
template <typename T>
auto get_array_dimension(T const& t, int i) -> decltype(t.array_dimension(i), 0) {
    return t.array_dimension(i);
}
template <typename T>
int get_array_dimension(T const&, ...) {
    return 1;
}

// Detect if a type T has a non-static member function called num_variables().
template <typename T, typename = void>
struct has_num_variables: std::false_type {};
template <typename T>
struct has_num_variables<T, std::void_t<decltype(std::declval<T>().num_variables())>>
    : std::true_type {};
template <typename T>
inline constexpr bool has_num_variables_v = has_num_variables<T>::value;

// Get the value of a static member variable called optional, or false if it doesn't exist.
template <typename T, typename = void>
struct optional: std::false_type {};
template <typename T>
struct optional<T, std::void_t<decltype(T::optional)>> {
    constexpr static bool value = T::optional;
};
template <typename T>
inline constexpr bool optional_v = optional<T>::value;

enum struct FieldImplementation {
    AlwaysSingle,    // field always exists -> std::vector<T>
    OptionalSingle,  // field exists 0 or 1 time -> std::vector<T> that might be skipped
    RuntimeVariable  // field is duplicated a number of times that is set at runtime ->
                     // std::vector<std::vector<T>>
};

template <typename Tag>
inline constexpr FieldImplementation field_impl_v =
    (has_num_variables_v<Tag> ? FieldImplementation::RuntimeVariable
                              : (optional_v<Tag> ? FieldImplementation::OptionalSingle
                                                 : FieldImplementation::AlwaysSingle));

// Get a name for a given field within a given tag
template <typename Tag>
auto get_name_impl(Tag const& tag, int field_index, std::nullptr_t)
    -> decltype(static_cast<void>(tag.name(field_index)), std::string()) {
    return tag.name(field_index);
}

template <typename Tag>
std::string get_name_impl(Tag const& tag, int field_index, ...) {
    auto ret = cxx_demangle(typeid(Tag).name());
    if (field_index >= 0) {
        ret.append(1, '#');
        ret.append(std::to_string(field_index));
    }
    constexpr std::string_view prefix{"neuron::container::"};
    if (std::string_view{ret}.substr(0, prefix.size()) == prefix) {
        ret.erase(0, prefix.size());
    }
    return ret;
}

/**
 * @brief Get the nicest available name for the field_index-th instance of Tag.
 *
 * This should elegantly handle field_index == -1 (=> the tag doesn't get have num_variables()) and
 * field_index being out of range.
 */
template <typename Tag>
auto get_name(Tag const& tag, int field_index) {
    if constexpr (has_num_variables_v<Tag>) {
        if (field_index >= 0 && field_index < tag.num_variables()) {
            // use tag.name(field_index) if it's available, otherwise fall back
            return get_name_impl(tag, field_index, nullptr);
        }
    }
    // no num_variables() or invalid field_index, use the fallback
    return get_name_impl(tag, field_index, 1 /* does not match nullptr */);
}

struct index_column_tag {
    using type = non_owning_identifier_without_container;
};

/** Check if the given range is a permutation of the first N integers.
 */
template <typename Rng>
void check_permutation_vector(Rng const& range, std::size_t size) {
    if (range.size() != size) {
        throw std::runtime_error("invalid permutation vector: wrong size");
    }
    std::vector<bool> seen(size, false);
    for (auto val: range) {
        if (!(val >= 0 && val < size)) {
            throw std::runtime_error("invalid permutation vector: value out of range");
        }
        if (seen[val]) {
            throw std::runtime_error("invalid permutation vector: repeated value " +
                                     std::to_string(val));
        }
        seen[val] = true;
    }
}

enum struct may_cause_reallocation { Yes, No };

extern std::vector<void*>* defer_delete_storage;

/**
 * @brief Storage for safe deletion of soa<...> containers.
 *
 * This is intended to prevent deleting an instance of a soa<...>-based container from invalidating
 * any existing data handles, by keeping certain (small) values alive. Deleting these containers is
 * probably not common (e.g. deleting a mechanism type), and only small bookkeeping-related values
 * have to be kept alive. Generally defer_delete_storage is non-null for the lifetime of the top
 * level Model structure, and the Model destructor deallocates (using delete[]) the pointers that
 * are stored inside defer_delete_storage.
 */
template <typename T>
void defer_delete(std::unique_ptr<T[]> data) {
    static_assert(std::is_trivially_destructible_v<T>, "defer_delete does not call destructors");
    if (data && defer_delete_storage) {
        defer_delete_storage->push_back(data.release());
    }
}

template <typename Tag, FieldImplementation impl>
struct field_data {
    static_assert(impl == FieldImplementation::AlwaysSingle ||
                  impl == FieldImplementation::OptionalSingle);
    using data_type = typename Tag::type;
    static_assert(!has_num_variables_v<Tag>);
    field_data(Tag tag)
        : m_tag{std::move(tag)}
        , m_array_dim{get_array_dimension(m_tag)} {
        if constexpr (impl == FieldImplementation::AlwaysSingle) {
            m_data_ptr = std::make_unique<data_type*[]>(1);
        }
    }

    ~field_data() {
        // An unknown number of data_handle<T> in the wild may be holding references to m_data_ptr
        defer_delete(std::move(m_data_ptr));
    }

    /**
     * @brief Return a reference to the tag instance.
     */
    Tag const& tag() const {
        return m_tag;
    }

    template <may_cause_reallocation might_reallocate, typename Callable>
    void for_all_vectors(Callable const& callable) {
        if constexpr (impl == FieldImplementation::OptionalSingle) {
            if (!m_data_ptr) {
                // inactive, optional field
                return;
            }
        }
        callable(m_tag, m_storage, -1, m_array_dim);
        if constexpr (might_reallocate == may_cause_reallocation::Yes) {
            m_data_ptr[0] = m_storage.data();
        }
    }

    template <typename Callable>
    void for_all_vectors(Callable const& callable) const {
        if constexpr (impl == FieldImplementation::OptionalSingle) {
            if (!m_data_ptr) {
                // inactive, optional field
                return;
            }
        }
        callable(m_tag, m_storage, -1, m_array_dim);
    }

    [[nodiscard]] bool active() const {
        static_assert(impl == FieldImplementation::OptionalSingle);
        return bool{m_data_ptr};
    }

    void set_active(bool enable, std::size_t size) {
        static_assert(impl == FieldImplementation::OptionalSingle);
        if (enable == active()) {
            return;
        }
        if (enable) {
            // make sure the storage is allocated + the right size + full of default values
            assert(m_storage.empty());  // it should be starting off empty
            if constexpr (has_default_value_v<Tag>) {
                m_storage.resize(size * m_array_dim, m_tag.default_value());
            } else {
                m_storage.resize(size * m_array_dim);
            }
            m_data_ptr = std::make_unique<data_type*[]>(1);
            m_data_ptr[0] = m_storage.data();
        } else {
            // clear + free the storage
            m_storage.clear();
            m_storage.shrink_to_fit();
            // data handles may be holding pointers to m_data_ptr (which is the reason for the
            // deferred deletion); signal to them that they are no longer valid by writing nullptr
            // here
            m_data_ptr[0] = nullptr;
            defer_delete(std::move(m_data_ptr));
        }
    }

    [[nodiscard]] data_type* const* data_ptrs() const {
        return m_data_ptr.get();
    }

    [[nodiscard]] int const* array_dims() const {
        return &m_array_dim;
    }

    [[nodiscard]] int const* array_dim_prefix_sums() const {
        return &m_array_dim;
    }

  private:
    /**
     * @brief Tag type instance.
     *
     * An instance of @c soa contains an instance of @c field_data for each tag type in its @c
     * Tags... pack. The instance of the tag type contains the metadata about the field it
     * represents, and @c field_data adds the actual data for that field. For example, with @c Tag =
     * @c Node::field::Voltage, which represents the voltage in a given Node, @c m_tag is just an
     * empty type that defines the @c data_type and default value of voltages.
     */
    Tag m_tag;

    /**
     * @brief Storage for the data associated with @c Tag.
     *
     * This is one of the "large" data arrays holding the model data. Because this specialisation of
     * @c field_data is for @c Tag types that @b don't have @c num_variables() members, such as @c
     * Node::field::Voltage, there is exactly one vector per instance of @c field_data. Because the
     * fields in @c Node::storage all have array dimension 1, in that case the size of this vector
     * is the number of Node instances in the program.
     */
    std::vector<data_type> m_storage;

    /**
     * @brief Storage where we maintain an up-to-date cache of @c m_storage.data().
     * @invariant @c m_storage.data() is equal to @c m_data_ptr.
     * @see field_data<Tag, true>::m_data_ptrs %for the motivation.
     *
     * This is declared as an array (of size 1) to simplify the implementation of defer_delete.
     * For FieldImplementation::OptionalSingle then whether or not this is null encodes whether
     * or not the field is active. For FieldImplementation::AlwaysSingle it is never null.
     */
    std::unique_ptr<data_type*[]> m_data_ptr;

    /**
     * @brief Array dimension of the data associated with @c Tag.
     * @invariant @c m_array_dim is equal to @c m_tag.array_dimension(), if that function exists,
     *            or 1.
     * @see field_data<Tag, true>::m_array_dims %for the motivation.
     */
    int m_array_dim;
};

/**
 * @brief Storage manager for a tag type that implements num_variables().
 *
 * An illustrative example is that this is responsible for the storage associated with floating
 * point mechanism data, where the number of fields is set at runtime via num_variables.
 *
 * As well as owning the actual storage containers, this type maintains two spans of values that
 * can be used by other types, in particular neuron::cache::MechanismRange:
 * - array_dims() returns a pointer to the first element of a num_variables()-sized range holding
 *   the array dimensions of the variables.
 * - array_dim_prefix_sums() returns a pointer to the first element of a num_variables()-sized
 *   range holding the prefix sum over the array dimensions (i.e. if array_dims() returns [1, 2, 1]
 *   then array_dim_prefix_sums() returns [1, 3, 4]).
 * - data_ptrs() returns a pointer to the first element of a num_variables()-sized range holding
 *   pointers to the start of the storage associated with each variable (i.e. the result of calling
 *   data() on the underlying vector).
 *
 * This is a helper type for use by neuron::container::soa and it should not be used directly.
 */
template <typename Tag>
struct field_data<Tag, FieldImplementation::RuntimeVariable> {
    using data_type = typename Tag::type;
    static_assert(has_num_variables_v<Tag>);
    field_data(Tag tag)
        : m_tag{std::move(tag)}
        , m_storage{m_tag.num_variables()}
        , m_data_ptrs{std::make_unique<data_type*[]>(m_tag.num_variables())} {
        update_data_ptr_storage();
        auto const num = m_tag.num_variables();
        m_array_dims.reserve(num);
        m_array_dim_prefix_sums.reserve(num);
        for (auto i = 0; i < m_tag.num_variables(); ++i) {
            m_array_dims.push_back(get_array_dimension(m_tag, i));
            m_array_dim_prefix_sums.push_back(
                (m_array_dim_prefix_sums.empty() ? 0 : m_array_dim_prefix_sums.back()) +
                m_array_dims.back());
        }
    }

    ~field_data() {
        // An unknown number of data_handle<T> in the wild may be holding references to m_data_ptrs
        defer_delete(std::move(m_data_ptrs));
    }

    /**
     * @brief Return a reference to the tag instance.
     */
    Tag const& tag() const {
        return m_tag;
    }

    /**
     * @brief Return a pointer to an array of array dimensions for this tag.
     *
     * This avoids indirection via the tag type instances. Because array dimensions are not
     * permitted to change, this is guaranteed to remain valid as long as the underlying soa<...>
     * container does. This is mainly intended for use in neuron::cache::MechanismRange and friends.
     */
    [[nodiscard]] int const* array_dims() const {
        return m_array_dims.data();
    }

    /**
     * @brief Return a pointer to an array of the prefix sum of array dimensions for this tag.
     */
    [[nodiscard]] int const* array_dim_prefix_sums() const {
        return m_array_dim_prefix_sums.data();
    }

    [[nodiscard]] int check_array_dim(int field_index, int array_index) const {
        assert(field_index >= 0);
        assert(array_index >= 0);
        if (auto const num_fields = m_tag.num_variables(); field_index >= num_fields) {
            throw std::runtime_error(get_name(m_tag, field_index) + "/" +
                                     std::to_string(num_fields) + ": out of range");
        }
        auto const array_dim = m_array_dims[field_index];
        if (array_index >= array_dim) {
            throw std::runtime_error(get_name(m_tag, field_index) + ": index " +
                                     std::to_string(array_index) + " out of range");
        }
        return array_dim;
    }

    /**
     * @brief Return a pointer to an array of data pointers for this tag.
     *
     * This array is guaranteed to be kept up to date when the actual storage is re-allocated.
     * This is mainly intended for use in neuron::cache::MechanismRange and friends.
     */
    [[nodiscard]] data_type* const* data_ptrs() const {
        return m_data_ptrs.get();
    }

    /**
     * @brief Invoke the given callable for each vector.
     *
     * @tparam might_reallocate Might the callable cause reallocation of the vector it is given?
     * @param callable Callable to invoke.
     */
    template <may_cause_reallocation might_reallocate, typename Callable>
    void for_all_vectors(Callable const& callable) {
        for (auto i = 0; i < m_storage.size(); ++i) {
            callable(m_tag, m_storage[i], i, m_array_dims[i]);
        }
        if constexpr (might_reallocate == may_cause_reallocation::Yes) {
            update_data_ptr_storage();
        }
    }

    template <typename Callable>
    void for_all_vectors(Callable const& callable) const {
        for (auto i = 0; i < m_storage.size(); ++i) {
            callable(m_tag, m_storage[i], i, m_array_dims[i]);
        }
    }

    // TODO actually use this
    // TODO use array_dim_prefix_sums
    [[nodiscard]] field_index translate_legacy_index(int legacy_index) const {
        int total{};
        auto const num_fields = m_tag.num_variables();
        for (auto field = 0; field < num_fields; ++field) {
            auto const array_dim = m_array_dims[field];
            if (legacy_index < total + array_dim) {
                auto const array_index = legacy_index - total;
                return {field, array_index};
            }
            total += array_dim;
        }
        throw std::runtime_error("could not translate legacy index " +
                                 std::to_string(legacy_index));
    }

  private:
    void update_data_ptr_storage() {
        std::transform(m_storage.begin(), m_storage.end(), m_data_ptrs.get(), [](auto& vec) {
            return vec.data();
        });
    }
    /**
     * @brief Tag type instance.
     *
     * An instance of @c soa contains an instance of @c field_data for each tag type in its @c
     * Tags... pack. The instance of the tag type contains the metadata about the field it
     * represents, and @c field_data adds the actual data for that field. For example, with @c Tag =
     * @c Mechanism::field::FloatingPoint, which represents RANGE variables in mechanisms, @c m_tag
     * holds the names and array dimensions of the RANGE variables.
     */
    Tag m_tag;

    /**
     * @brief Storage for the data associated with @c Tag.
     *
     * These are the "large" data arrays holding the model data. Because this specialisation of @c
     * field_data is for @c Tag types that @b do have @c num_variables() members, such as @c
     * Mechanism::field::FloatingPoint, there is an outer vector with this dimension.
     *
     * @invariant @c m_storage.size() is equal to @c m_tag.num_variables()
     *
     * For Mechanism data, this size is equal to the number of RANGE variables, while
     * @c m_storage[i].size() is (assuming an array dimension of 1) the number of instances (in this
     * case of the given Mechanism type) that exist in the program.
     */
    std::vector<std::vector<data_type>> m_storage;

    /**
     * @brief Storage where we maintain an up-to-date cache of .data() pointers from m_storage.
     * @invariant @c m_data_ptrs contains @c m_storage.size() elements
     * @invariant @c m_storage[i].data() is equal to @c m_data_ptrs[i] for all @c i.
     *
     * This is useful because it allows @c data_handle<T> to store something like @c T** instead of
     * having to store something like @c std::vector<T>*, which avoids hardcoding unnecessary
     * details about the allocator and so on, and allows @c cache::MechanismRange to similarly have
     * a C-like interface. Because @c data_handle<T> remembers references to this, we cannot free
     * it when the container is destroyed (e.g. when a mechanism type is deleted).
     */
    std::unique_ptr<data_type*[]> m_data_ptrs;

    /**
     * @brief Array dimensions of the data associated with @c Tag.
     * @invariant @c m_storage.size() is equal to @c m_array_dims.size()
     * @invariant @c m_array_dims[i] is equal to @c m_tag.array_dimension(i), if that function
     *            exists, or otherwise 1, for all @c i
     *
     * Similar to @c m_data_ptrs, this allows the array dimensions to be communicated simply across
     * a C-like interface.
     */
    std::vector<int> m_array_dims;

    /**
     * @brief Prefix sum over array dimensions for the data associated with @c Tag.
     * @invariant @c m_storage.size() is equal to @c m_array_dim_prefix_sums.size()
     * @invariant @c m_array_dim_prefix_sums[i] is equal to the sum of @c m_array_dims[0] .. @c
     *            m_array_dims[i] for all @c i.
     * @todo This could be used to more efficiently convert legacy indices.
     *
     * This is mainly useful for logic that aids the transition from AoS to SoAoS format in NEURON.
     * For example, the size of the old @c _p vectors in NEURON was @c
     * m_array_dim_prefix_sums.back(), the sum over all array dimensions, which is generally larger
     * than @c m_tag.num_variables().
     */
    std::vector<int> m_array_dim_prefix_sums;
};

struct storage_info_impl: utils::storage_info {
    std::string_view container() const override {
        return m_container;
    }
    std::string_view field() const override {
        return m_field;
    }
    std::size_t size() const override {
        return m_size;
    }
    std::string m_container{}, m_field{};
    std::size_t m_size{};
};
}  // namespace detail

/** @brief Token whose lifetime manages frozen/sorted state of a container.
 */
template <typename Container>
struct state_token {
    constexpr state_token() = default;
    constexpr state_token(state_token&& other)
        : m_container{std::exchange(other.m_container, nullptr)} {}
    constexpr state_token(state_token const&) = delete;
    constexpr state_token& operator=(state_token&& other) {
        m_container = std::exchange(other.m_container, nullptr);
        return *this;
    }
    constexpr state_token& operator=(state_token const&) = delete;
    ~state_token() {
        if (m_container) {
            m_container->decrease_frozen_count();
        }
    }

  private:
    template <typename, typename...>
    friend struct soa;
    constexpr state_token(Container& container)
        : m_container{&container} {}
    Container* m_container{};
};

/**
 * @brief Utility for generating SOA data structures.
 * @headerfile neuron/container/soa_container.hpp
 * @tparam Storage    Name of the actual storage type derived from soa<...>.
 * @tparam Tags       Parameter pack of tag types that define the columns
 *                    included in the container. Types may not be repeated.
 *
 * This CRTP base class is used to implement the ~global SOA storage structs
 * that hold (so far) Node and Mechanism data. Ownership of rows in these
 * structs is managed via instances of the owning identifier type @ref
 * neuron::container::owning_identifier instantiated with Storage, and
 * non-owning reference to rows in the data structures are managed via instances
 * of the @ref neuron::container::non_owning_identifier template instantiated
 * with Storage. These identifiers are typically wrapped in a
 * data-structure-specific (i.e. Node- or Mechanism-specific) interface type
 * that provides data-structure-specific accessors and methods to obtain actual
 * data values and more generic handle types such as @ref
 * neuron::container::data_handle<T> and @ref
 * neuron::container::generic_data_handle.
 */
template <typename Storage, typename... Tags>
struct soa {
    /**
     * @brief Construct with default-constructed tag type instances.
     */
    soa()
        : soa(Tags{}...) {}

    /**
     * @brief Construct with specific tag instances.
     *
     * This is useful if the tag types are not empty, for example if the number
     * of times a column is duplicated is a runtime value.
     */
    soa(Tags... tag_instances)
        : m_data{std::move(tag_instances)...} {}

    /**
     * @brief @ref soa is not movable
     *
     * This is to make it harder to accidentally invalidate pointers-to-storage
     * in handles.
     */
    soa(soa&&) = delete;

    /**
     * @brief @ref soa is not copiable
     *
     * This is partly to make it harder to accidentally invalidate
     * pointers-to-storage in handles, and partly because it could be very
     * expensive so it might be better to be more explicit.
     */
    soa(soa const&) = delete;

    /**
     * @brief @ref soa is not move assignable
     *
     * For the same reason it isn't movable.
     */
    soa& operator=(soa&&) = delete;

    /**
     * @brief @ref soa is not copy assignable
     *
     * For the same reasons it isn't copy constructible
     */
    soa& operator=(soa const&) = delete;

    /**
     * @brief Get the size of the container.
     */
    [[nodiscard]] std::size_t size() const {
        // Check our various std::vector members are still the same size as each
        // other. This check could be omitted in release builds...
        auto const check_size = m_indices.size();
        for_all_vectors(
            [check_size](auto const& tag, auto const& vec, int field_index, int array_dim) {
                auto const size = vec.size();
                assert(size % array_dim == 0);
                assert(size / array_dim == check_size);
            });
        return check_size;
    }

    /**
     * @brief Test if the container is empty.
     */
    [[nodiscard]] bool empty() const {
        auto const result = m_indices.empty();
        for_all_vectors([result](auto const& tag, auto const& vec, int field_index, int array_dim) {
            assert(vec.empty() == result);
        });
        return result;
    }

  private:
    /**
     * @brief Remove the @f$i^{\text{th}}@f$ row from the container.
     *
     * This is currently implemented by swapping the last element into position
     * @f$i@f$ (if those are not the same element) and reducing the size by one.
     * Iterators to the last element and the deleted element will be
     * invalidated.
     */
    void erase(std::size_t i) {
        if (m_frozen_count) {
            throw_error("erase() called on a frozen structure");
        }
        mark_as_unsorted_impl<true>();
        auto const old_size = size();
        assert(i < old_size);
        if (i != old_size - 1) {
            // Swap ranges of size array_dim at logical positions `i` and `old_size - 1` in each
            // vector
            for_all_vectors<detail::may_cause_reallocation::No>(
                [i](auto const& tag, auto& vec, int field_index, int array_dim) {
                    ::std::swap_ranges(::std::next(vec.begin(), i * array_dim),
                                       ::std::next(vec.begin(), (i + 1) * array_dim),
                                       ::std::prev(vec.end(), array_dim));
                });
            // Tell the new entry at `i` that its index is `i` now.
            m_indices[i].set_current_row(i);
        }
        for_all_vectors<detail::may_cause_reallocation::No>(
            [new_size = old_size - 1](auto const& tag, auto& vec, int field_index, int array_dim) {
                vec.resize(new_size * array_dim);
            });
    }

    friend struct state_token<Storage>;
    friend struct owning_identifier<Storage>;

    static_assert(detail::are_types_unique_v<Tags...>, "All tag types should be unique");
    template <typename Tag>
    static constexpr std::size_t tag_index_v = detail::index_of_type_v<Tag, Tags...>;

    /**
     * @brief Apply the given function to non-const versions of all vectors.
     *
     * @tparam might_reallocate Might the callable trigger reallocation of the vectors?
     * @param callable Callable to invoke on each vector.
     *
     * If might_allocate is true then the "cached" values of .data() for each vector will be
     * updated.
     */
    template <detail::may_cause_reallocation might_reallocate, typename Callable>
    void for_all_vectors(Callable const& callable) {
        // might_reallocate is not relevant for m_indices because we do not expose the location of
        // its storage, so it doesn't matter whether or not this triggers reallocation
        callable(detail::index_column_tag{}, m_indices, -1, 1);
        (std::get<tag_index_v<Tags>>(m_data).template for_all_vectors<might_reallocate>(callable),
         ...);
    }

    /**
     * @brief Apply the given function to const-qualified versions of all vectors.
     *
     * Because of the const qualification this cannot cause reallocation and trigger updates of
     * pointers inside m_data, so no might_reallocate parameter is needed.
     */
    template <typename Callable>
    void for_all_vectors(Callable const& callable) const {
        callable(detail::index_column_tag{}, m_indices, -1, 1);
        (std::get<tag_index_v<Tags>>(m_data).for_all_vectors(callable), ...);
    }

    /**
     * @brief Flag that the storage is no longer frozen.
     *
     * This is called from the destructor of state_token.
     */
    void decrease_frozen_count() {
        assert(m_frozen_count);
        --m_frozen_count;
    }

  public:
    /**
     * @brief Return type of get_sorted_token()
     */
    using sorted_token_type = state_token<Storage>;

    /**
     * @brief Mark the container as sorted and return a token guaranteeing that.
     *
     * Is is user-defined precisely what "sorted" means, but the soa<...> class
     * makes some guarantees:
     * - if the container is frozen, no pointers to elements in the underlying
     *   storage will be invalidated -- attempts to do so will throw or abort.
     * - if the container is not frozen, it will remain flagged as sorted until
     *   a potentially-pointer-invalidating operation (insertion, deletion,
     *   permutation) occurs, or mark_as_unsorted() is called.
     *
     * The container will be frozen for the lifetime of the token returned from
     * this function, and therefore also sorted for at least that time. This
     * token has the semantics of a unique_ptr, i.e. it cannot be copied but
     * can be moved, and destroying a moved-from token has no effect.
     *
     * The tokens returned by this function are reference counted; the
     * container will be frozen for as long as any token is alive.
     *
     * Note that "frozen" refers to the storage layout, not to the stored value,
     * meaning that values inside a frozen container can still be modified --
     * "frozen" is not "runtime const".
     *
     * @todo A future extension could be to preserve the sorted flag until
     *       pointers are actually, not potentially, invalidated.
     */
    [[nodiscard]] sorted_token_type get_sorted_token() {
        // Increment the reference count, marking the container as frozen.
        ++m_frozen_count;
        // Mark the container as sorted
        m_sorted = true;
        // Return a token that calls decrease_frozen_count() at the end of its lifetime
        return sorted_token_type{static_cast<Storage&>(*this)};
    }

    /**
     * @brief Tell the container it is no longer sorted.
     *
     * The meaning of being sorted is externally defined, and it is possible
     * that some external change to an input of the (external) algorithm
     * defining the sort order can mean that the data are no longer considered
     * sorted, even if nothing has actually changed inside this container.
     */
    void mark_as_unsorted() {
        mark_as_unsorted_impl<false>();
    }

    /**
     * @brief Set the callback that is invoked when the container becomes unsorted.
     *
     * This is invoked by mark_as_unsorted() and when a container operation
     * (insertion, permutation, deletion) causes the container to transition
     * from being sorted to being unsorted.
     */
    void set_unsorted_callback(std::function<void()> unsorted_callback) {
        m_unsorted_callback = std::move(unsorted_callback);
    }

    /**
     * @brief Query if the underlying vectors are still "sorted".
     *
     * See the documentation of get_sorted_token() for an explanation of what
     * this means.
     */
    [[nodiscard]] bool is_sorted() const {
        return m_sorted;
    }

    /** @brief Permute the SOA-format data using an arbitrary vector.
     */
    template <typename Range>
    void apply_reverse_permutation(Range permutation) {
        // Check that the given vector is a valid permutation of length size().
        std::size_t const my_size{size()};
        detail::check_permutation_vector(permutation, my_size);
        // Applying a permutation in general invalidates indices, so it is forbidden if the
        // structure is frozen, and it leaves the structure unsorted.
        if (m_frozen_count) {
            throw_error("apply_reverse_permutation() called on a frozen structure");
        }
        mark_as_unsorted_impl<true>();
        // Now we apply the reverse permutation in `permutation` to all of the columns in the
        // container. This is the algorithm from boost::algorithm::apply_reverse_permutation.
        for (std::size_t i = 0; i < my_size; ++i) {
            while (i != permutation[i]) {
                using ::std::swap;
                auto const next = permutation[i];
                for_all_vectors<detail::may_cause_reallocation::No>(
                    [i, next](auto const& tag, auto& vec, auto field_index, auto array_dim) {
                        // swap the i-th and next-th array_dim-sized sub-ranges of vec
                        ::std::swap_ranges(::std::next(vec.begin(), i * array_dim),
                                           ::std::next(vec.begin(), (i + 1) * array_dim),
                                           ::std::next(vec.begin(), next * array_dim));
                    });
                swap(permutation[i], permutation[next]);
            }
        }
        // update the indices in the container
        for (auto i = 0ul; i < my_size; ++i) {
            m_indices[i].set_current_row(i);
        }
    }


  private:
    template <bool internal>
    void mark_as_unsorted_impl() {
        if (m_frozen_count) {
            // Currently you can only obtain a frozen container by calling
            // get_sorted_token(), which explicitly guarantees that the
            // container will remain sorted for the lifetime of the returned
            // token.
            throw_error("mark_as_unsorted() called on a frozen structure");
        }
        // Only execute the callback if we're transitioning from sorted to
        // or if this was an explicit mark_as_unsorted() call
        bool const execute_callback{m_sorted || !internal};
        m_sorted = false;
        if (execute_callback && m_unsorted_callback) {
            m_unsorted_callback();
        }
    }

    /**
     * @brief Create a new entry and return an identifier that owns it.
     *
     * Calling this method increases size() by one. Destroying (modulo move
     * operations) the returned identifier, which has the semantics of a
     * unique_ptr, decreases size() by one.
     *
     * Note that this has different semantics to standard library container
     * methods such as emplace_back(), push_back(), insert() and so on. Because
     * the returned identifier manages the lifetime of the newly-created entry,
     * discarding the return value will cause the new entry to immediately be
     * deleted.
     *
     * This is a low-level call that is useful for the implementation of the
     * owning_identifier template. The returned owning identifier is typically
     * wrapped inside an owning handle type that adds data-structure-specific
     * methods (e.g. v(), v_handle() for a Node).
     */
    [[nodiscard]] owning_identifier<Storage> acquire_owning_identifier() {
        if (m_frozen_count) {
            throw_error("acquire_owning_identifier() called on a frozen structure");
        }
        // The .emplace_back() methods we are about to call can trigger
        // reallocation and, therefore, invalidation of pointers. At present,
        // "sorted" is defined to mean that pointers have not been invalidated.
        // There are two reasonable changes that could be made here:
        //  - possibly for release builds, we could only mark unsorted if a
        //    reallocation *actually* happens
        //  - "sorted" could be defined to mean that indices have not been
        //    invalidated -- adding a new entry to the end of the container
        //    never invalidates indices
        mark_as_unsorted_impl<true>();
        // Append to all of the vectors
        auto const old_size = size();
        for_all_vectors<detail::may_cause_reallocation::Yes>(
            [](auto const& tag, auto& vec, auto field_index, auto array_dim) {
                using Tag = ::std::decay_t<decltype(tag)>;
                if constexpr (detail::has_default_value_v<Tag>) {
                    vec.insert(vec.end(), array_dim, tag.default_value());
                } else {
                    vec.insert(vec.end(), array_dim, {});
                }
            });
        // Important that this comes after the m_frozen_count check
        owning_identifier<Storage> index{static_cast<Storage&>(*this), old_size};
        // Update the pointer-to-row-number in m_indices so it refers to the
        // same thing as index
        m_indices.back() = static_cast<non_owning_identifier_without_container>(index);
        return index;
    }

  public:
    /**
     * @brief Get a non-owning identifier to the offset-th entry.
     */
    [[nodiscard]] non_owning_identifier<Storage> at(std::size_t offset) const {
        return {const_cast<Storage*>(static_cast<Storage const*>(this)), m_indices[offset]};
    }

    /**
     * @brief Get the instance of the given tag type.
     * @tparam Tag The tag type, which must be a member of the @c Tags... pack.
     * @return Const reference to the given tag type instance.
     *
     * For example, if this is called on the @c Node::storage then @c Tag would be something like @c
     * Node::field::Area, @c Node::field::RHS or @c Node::field::Voltage, which are empty types that
     * serve to define the default values and types of those quantities.
     *
     * At the time of writing the other possibility is that this is called on an instance of @c
     * Mechanism::storage, in which case @c Tag must (currently) be @c
     * Mechanism::field::FloatingPoint. This stores the names and array dimensions of the RANGE
     * variables in the mechanism (MOD file), which are only known at runtime.
     */
    template <typename Tag>
    [[nodiscard]] constexpr Tag const& get_tag() const {
        return std::get<tag_index_v<Tag>>(m_data).tag();
    }

    template <typename Tag>
    static constexpr bool has_tag_v = detail::type_in_pack_v<Tag, Tags...>;

    /**
     * @brief Get the offset-th element of the column named by Tag.
     *
     * Because this is returning a single value, it is permitted even in
     * read-only mode. The container being in read only mode means that
     * operations that would invalidate iterators/pointers are forbidden, not
     * that actual data values cannot change.
     */
    template <typename Tag>
    [[nodiscard]] typename Tag::type& get(std::size_t offset) {
        static_assert(has_tag_v<Tag>);
        static_assert(!detail::has_num_variables_v<Tag>);
        auto& field_data = std::get<tag_index_v<Tag>>(m_data);
        if constexpr (detail::field_impl_v<Tag> == detail::FieldImplementation::OptionalSingle) {
            if (!field_data.active()) {
                throw_error("get(offset) called for a disabled optional field");
            }
        }
        return field_data.data_ptrs()[0][offset];
    }

    /**
     * @brief Get the offset-th element of the column named by Tag.
     */
    template <typename Tag>
    [[nodiscard]] typename Tag::type const& get(std::size_t offset) const {
        static_assert(has_tag_v<Tag>);
        static_assert(!detail::has_num_variables_v<Tag>);
        auto const& field_data = std::get<tag_index_v<Tag>>(m_data);
        if constexpr (detail::field_impl_v<Tag> == detail::FieldImplementation::OptionalSingle) {
            if (!field_data.active()) {
                throw_error("get(offset) const called for a disabled optional field");
            }
        }
        return field_data.data_ptrs()[0][offset];
    }

    /**
     * @brief Get a handle to the given element of the column named by Tag.
     */
    template <typename Tag>
    [[nodiscard]] data_handle<typename Tag::type> get_handle(
        non_owning_identifier_without_container id,
        int array_index = 0) const {
        static_assert(has_tag_v<Tag>);
        static_assert(!detail::has_num_variables_v<Tag>);
        auto const& field_data = std::get<tag_index_v<Tag>>(m_data);
        // If Tag is an optional field and that field is disabled, return a null handle.
        if constexpr (detail::optional_v<Tag>) {
            if (!field_data.active()) {
                return {};
            }
        }
        auto const array_dim = field_data.array_dims()[0];
        assert(array_dim > 0);
        assert(array_index >= 0);
        assert(array_index < array_dim);
        return {std::move(id), field_data.data_ptrs(), array_dim, array_index};
    }

    /**
     * @brief Get a handle to the given element of the field_index-th column named by Tag.
     */
    template <typename Tag>
    [[nodiscard]] data_handle<typename Tag::type> get_field_instance_handle(
        non_owning_identifier_without_container id,
        int field_index,
        int array_index = 0) const {
        static_assert(has_tag_v<Tag>);
        static_assert(detail::has_num_variables_v<Tag>);
        auto const array_dim = std::get<tag_index_v<Tag>>(m_data).check_array_dim(field_index,
                                                                                  array_index);
        return {std::move(id),
                std::get<tag_index_v<Tag>>(m_data).data_ptrs() + field_index,
                array_dim,
                array_index};
    }

    /**
     * @brief Get the offset-th element of the field_index-th instance of the column named by Tag.
     *
     * Put differently:
     *  - offset: index of a mechanism instance
     *  - field_index: index of a RANGE variable inside a mechanism
     *  - array_index: offset inside an array RANGE variable
     */
    template <typename Tag>
    typename Tag::type& get_field_instance(std::size_t offset,
                                           int field_index,
                                           int array_index = 0) {
        auto const array_dim = std::get<tag_index_v<Tag>>(m_data).check_array_dim(field_index,
                                                                                  array_index);
        return std::get<tag_index_v<Tag>>(m_data)
            .data_ptrs()[field_index][offset * array_dim + array_index];
    }

    /**
     * @brief Get the offset-th element of the field_index-th instance of the column named by Tag.
     */
    template <typename Tag>
    typename Tag::type const& get_field_instance(std::size_t offset,
                                                 int field_index,
                                                 int array_index = 0) const {
        auto const array_dim = std::get<tag_index_v<Tag>>(m_data).check_array_dim(field_index,
                                                                                  array_index);
        return std::get<tag_index_v<Tag>>(m_data)
            .data_ptrs()[field_index][offset * array_dim + array_index];
    }

    [[nodiscard]] non_owning_identifier_without_container get_identifier(std::size_t offset) const {
        return m_indices.at(offset);
    }

    /**
     * @brief Return a permutation-stable handle if ptr is inside us.
     * @todo Check const-correctness. Presumably a const version would return
     *       data_handle<T const>, which would hold a pointer-to-const for the
     *       container?
     */
    [[nodiscard]] neuron::container::generic_data_handle find_data_handle(
        neuron::container::generic_data_handle input_handle) const {
        bool done{false};
        neuron::container::generic_data_handle handle{};
        for_all_vectors([this, &done, &handle, &input_handle](
                            auto const& tag, auto const& vec, int field_index, int array_dim) {
            using Tag = ::std::decay_t<decltype(tag)>;
            if constexpr (!std::is_same_v<Tag, detail::index_column_tag>) {
                using Data = typename Tag::type;
                if (done) {
                    // Short circuit
                    return;
                }
                if (vec.empty()) {
                    // input_handle can't point into an empty vector
                    return;
                }
                if (!input_handle.holds<Data*>()) {
                    // input_handle can't point into a vector of the wrong type
                    return;
                }
                auto* const ptr = input_handle.get<Data*>();
                if (ptr < vec.data() || ptr >= std::next(vec.data(), vec.size())) {
                    return;
                }
                auto const physical_row = ptr - vec.data();
                assert(physical_row < vec.size());
                // This pointer seems to live inside this container. This is all a bit fragile.
                int const array_index = physical_row % array_dim;
                int const logical_row = physical_row / array_dim;
                handle = neuron::container::data_handle<Data>{
                    at(logical_row),
                    std::get<tag_index_v<Tag>>(m_data).data_ptrs() + std::max(field_index, 0),
                    array_dim,
                    array_index};
                assert(handle.refers_to_a_modern_data_structure());
                done = true;  // generic_data_handle doesn't convert to bool
            }
        });
        return handle;
    }

    /**
     * @brief Query whether the given pointer-to-vector is the one associated to Tag.
     *
     * This is used so that one can ask a data_handle<T> if it refers to a particular field in a
     * particular container.
     */
    template <typename Tag>
    [[nodiscard]] bool is_storage_pointer(typename Tag::type const* ptr) const {
        static_assert(has_tag_v<Tag>);
        static_assert(!detail::has_num_variables_v<Tag>);
        auto* const data_ptrs = std::get<tag_index_v<Tag>>(m_data).data_ptrs();
        if constexpr (detail::optional_v<Tag>) {
            // if the field is optional and disabled, data_ptrs is null
            if (!data_ptrs) {
                return false;
            }
        }
        return ptr == data_ptrs[0];
    }

    [[nodiscard]] std::unique_ptr<utils::storage_info> find_container_info(void const* cont) const {
        std::unique_ptr<utils::storage_info> opt_info;
        if (!cont) {
            return opt_info;
        }
        for_all_vectors([cont,
                         &opt_info,
                         this](auto const& tag, auto const& vec, int field_index, int array_dim) {
            if (opt_info) {
                // Short-circuit
                return;
            }
            if (vec.data() != cont) {
                // This isn't the right vector
                return;
            }
            // We found the right container/tag combination! Populate the
            // information struct.
            auto impl_ptr = std::make_unique<detail::storage_info_impl>();
            auto& info = *impl_ptr;
            info.m_container = static_cast<Storage const&>(*this).name();
            info.m_field = detail::get_name(tag, field_index);
            info.m_size = vec.size();
            assert(info.m_size % array_dim == 0);
            opt_info = std::move(impl_ptr);
        });
        return opt_info;
    }

    /**
     * @brief Get a pointer to a range of pointers that always point to the start of the contiguous
     * storage.
     */
    template <typename Tag>
    [[nodiscard]] typename Tag::type* const* get_data_ptrs() const {
        return std::get<tag_index_v<Tag>>(m_data).data_ptrs();
    }

    /**
     * @brief Get a pointer to an array holding the array dimensions of the fields associated with
     * this tag.
     */
    template <typename Tag>
    [[nodiscard]] int const* get_array_dims() const {
        return std::get<tag_index_v<Tag>>(m_data).array_dims();
    }

    /**
     * @brief Get a pointer to an array holding the prefix sum of array dimensions for this tag.
     */
    template <typename Tag>
    [[nodiscard]] int const* get_array_dim_prefix_sums() const {
        return std::get<tag_index_v<Tag>>(m_data).array_dim_prefix_sums();
    }

    template <typename Tag>
    [[nodiscard]] field_index translate_legacy_index(int legacy_index) const {
        return std::get<tag_index_v<Tag>>(m_data).translate_legacy_index(legacy_index);
    }

    /**
     * @brief Query whether the field associated with the given tag is active.
     */
    template <typename Tag>
    [[nodiscard]] bool field_active() const {
        static_assert(detail::optional_v<Tag>,
                      "field_active can only be called with tag types for optional fields");
        return std::get<tag_index_v<Tag>>(m_data).active();
    }

    /**
     * @brief Enable/disable the fields associated with the given tags.
     */
    template <typename... TagsToChange>
    void set_field_status(bool enabled) {
        static_assert((detail::optional_v<TagsToChange> && ...),
                      "set_field_status can only be called with tag types for optional fields");
        auto const size = m_indices.size();
        (std::get<tag_index_v<TagsToChange>>(m_data).set_active(enabled, size), ...);
    }

  private:
    [[noreturn]] void throw_error(std::string_view message) const {
        std::ostringstream oss;
        oss << cxx_demangle(typeid(Storage).name()) << "[frozen_count=" << m_frozen_count
            << ",sorted=" << std::boolalpha << m_sorted << "]: " << message;
        throw std::runtime_error(std::move(oss).str());
    }

    /**
     * @brief Flag for get_sorted_token(), mark_as_unsorted() and is_sorted().
     */
    bool m_sorted{false};

    /**
     * @brief Reference count for tokens guaranteeing the container is in frozen mode.
     *
     * In principle this would be called before multiple worker threads spin up,
     * but in practice as a transition measure then handles are acquired inside
     * worker threads.
     */
    std::atomic<std::size_t> m_frozen_count{};

    /**
     * @brief Pointers to identifiers that record the current physical row.
     */
    std::vector<non_owning_identifier_without_container> m_indices{};

    /**
     * @brief Collection of data columns.
     *
     * If the tag implements a num_variables() method then it is duplicated a
     * runtime-determined number of times and get_field_instance<Tag>(i) returns
     * the i-th element of the outer vector (of length num_variables()) of
     * vectors. If is no num_variables() method then the outer vector can be
     * omitted and get<Tag>() returns a vector of values.
     */
    std::tuple<detail::field_data<Tags, detail::field_impl_v<Tags>>...> m_data{};

    /**
     * @brief Callback that is invoked when the container becomes unsorted.
     */
    std::function<void()> m_unsorted_callback{};
};
}  // namespace neuron::container

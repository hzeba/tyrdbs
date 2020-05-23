#include <common/branch_prediction.h>
#include <common/exception.h>
#include <tyrdbs/slice.h>
#include <tyrdbs/cache.h>
#include <tyrdbs/location.h>

#include <crc32c.h>

#include <cassert>


namespace tyrtech::tyrdbs {


thread_local uint64_t __cache_id{0};


class slice_iterator : public iterator
{
public:
    bool next() override;

    std::string_view key() const override;
    std::string_view value() const override;
    bool eor() const override;
    bool deleted() const override;
    uint64_t tid() const override;

public:
    slice_iterator(slice* slice, std::shared_ptr<node> node, uint16_t ndx);

private:
    slice* m_slice{nullptr};

    std::shared_ptr<node> m_node;
    uint16_t m_ndx{static_cast<uint16_t>(-1)};

    const data_attributes* m_attrs{nullptr};

private:
    bool load_next();
};

bool slice_iterator::next()
{
    if (m_slice == nullptr)
    {
        return false;
    }

    if (m_attrs == nullptr)
    {
        m_attrs = m_node->attributes_at<data_attributes>(m_ndx);
        return true;
    }

    if (m_ndx == m_node->key_count() - 1)
    {
        if (load_next() == false)
        {
            m_slice = nullptr;
            m_node.reset();

            return false;
        }

        m_ndx = static_cast<uint16_t>(-1);
    }

    m_ndx++;

    m_attrs = m_node->attributes_at<data_attributes>(m_ndx);

    return true;
}

std::string_view slice_iterator::key() const
{
    return m_node->key_at(m_ndx);
}

std::string_view slice_iterator::value() const
{
    return m_node->value_at(m_ndx);
}

bool slice_iterator::eor() const
{
    return m_node->eor_at(m_ndx);
}

bool slice_iterator::deleted() const
{
    return m_node->deleted_at(m_ndx);
}

uint64_t slice_iterator::tid() const
{
    if (m_attrs->tid == 0)
    {
        assert(m_slice->m_tid != 0);
        return m_slice->m_tid;
    }

    return m_attrs->tid;
}

slice_iterator::slice_iterator(slice* slice, cache::node_ptr node, uint16_t ndx)
  : m_slice(slice)
  , m_node(std::move(node))
  , m_ndx(ndx)
{
    assert(likely(ndx < m_node->key_count()));
}

bool slice_iterator::load_next()
{
    while (true)
    {
        uint64_t location = m_node->get_next();

        if (location::is_valid(location) == false)
        {
            return false;
        }

        m_node = m_slice->load(location);

        if (location::is_leaf_from(location) == true)
        {
            break;
        }
    }

    return true;
}

std::unique_ptr<iterator> slice::range(const std::string_view& min_key, const std::string_view& max_key)
{
    if (unlikely(key_count() == 0))
    {
        return nullptr;
    }

    assert(likely(min_key.compare(max_key) <= 0));

    uint64_t location = find_node_for(m_root, min_key, max_key);

    if (location::is_valid(location) == false)
    {
        return nullptr;
    }

    auto&& node = load(location);
    uint16_t ndx = node->lower_bound(min_key);

    return std::make_unique<slice_iterator>(this, std::move(node), ndx);
}

std::unique_ptr<iterator> slice::begin()
{
    if (unlikely(key_count() == 0))
    {
        return nullptr;
    }

    uint64_t location = location::location(0, m_first_node_size);
    auto&& node = load(location);

    return std::make_unique<slice_iterator>(this, std::move(node), 0);
}

void slice::unlink()
{
    assert(likely(m_unlink == false));
    m_unlink = true;
}

uint64_t slice::key_count() const
{
    return m_key_count;
}

void slice::set_tid(uint64_t tid)
{
    assert(m_tid == 0);
    m_tid = tid;
}

slice::slice(uint64_t size, uint64_t cache_id, std::shared_ptr<reader> reader)
  : m_cache_id(cache_id)
  , m_reader(std::move(reader))
{
    if (m_cache_id == static_cast<uint64_t>(-1))
    {
        m_cache_id = __cache_id++;
    }

    header h;

    m_reader->pread(size - sizeof(h),
                    reinterpret_cast<char*>(&h),
                    sizeof(h));

    if (h.signature != signature)
    {
        throw runtime_error_exception("invalid slice signature");
    }

    m_key_count = h.stats.key_count;

    m_root = h.root;
    m_first_node_size = h.first_node_size;
}

slice::~slice()
{
    if (m_unlink == true)
    {
        m_reader->unlink();
    }
}

cache::node_ptr slice::load(uint64_t location) const
{
    return cache::get(m_reader.get(), m_cache_id, location);
}

uint64_t slice::find_node_for(uint64_t location,
                               const std::string_view& min_key,
                               const std::string_view& max_key) const
{
    while (true)
    {
        auto&& node = load(location);
        uint16_t ndx = node->lower_bound(min_key);

        if (ndx != node->key_count())
        {
            int32_t cmp = min_key.compare(node->key_at(ndx));
            assert(likely(cmp <= 0));

            if (cmp < 0 && ndx != 0)
            {
                ndx--;
            }
        }
        else
        {
            ndx--;
        }

        auto index_min_key = node->key_at(ndx);
        auto index_max_key = node->value_at(ndx);

        if (min_key.compare(index_max_key) > 0)
        {
            return static_cast<uint64_t>(-1);
        }

        if (max_key.compare(index_min_key) < 0)
        {
            return static_cast<uint64_t>(-1);
        }

        location = node->template attributes_at<index_attributes>(ndx)->location;

        if (location::is_leaf_from(location) == true)
        {
            break;
        }
    }

    return location;
}

}

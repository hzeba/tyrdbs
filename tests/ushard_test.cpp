#include <common/cmd_line.h>
#include <common/cpu_sched.h>
#include <common/uuid.h>
#include <gt/engine.h>
#include <gt/async.h>
#include <gt/condition.h>
#include <io/engine.h>
#include <tyrdbs/cache.h>
#include <tyrdbs/slice_writer.h>
#include <tyrdbs/overwrite_iterator.h>

#include <tests/stats.h>

#include <crc32c.h>


using namespace tyrtech;


struct thread_data
{
    using ushard_t =
            std::vector<tyrdbs::slices_t>;

    std::vector<uint8_t> merge_requests;
    gt::condition merge_cond;

    ushard_t ushard;

    bool compact{false};
    bool exit_merge{false};

    bool merge_done{false};
    gt::condition done_cond;

    uint32_t thread_id{0};

    uint64_t duration{0};

    tests::stats vs_stats;
    tests::stats vr_stats;

    thread_data(uint32_t thread_id)
      : thread_id(thread_id)
    {
        for (uint32_t i = 0; i < 16; i++)
        {
            ushard.emplace_back(tyrdbs::slices_t());
        }
    }
};


uint32_t tid{1};
std::string string_storage;


using string_t =
        std::pair<uint64_t, uint16_t>;

using data_set_t =
        std::vector<string_t>;

using data_sets_t =
        std::vector<data_set_t>;


uint32_t tier_of(const tyrdbs::slice_ptr& slice)
{
    return (64 - __builtin_clzll(slice->key_count())) >> 2;
}

void insert(const data_set_t& data, thread_data* td)
{
    char buff[37];
    tyrdbs::slice_writer w("{}.dat", uuid().str(buff, sizeof(buff)));

    auto t1 = clock::now();

    for (auto&& it : data)
    {
        std::string_view key(string_storage.data() + it.first, it.second);
        w.add(key, key, true, false);
    }

    w.flush();

    auto slice = std::make_shared<tyrdbs::slice>(w.commit(), "{}", w.path());
    slice->set_tid(tid++);

    auto tier = tier_of(slice);

    td->ushard[tier].push_back(std::move(slice));

    if (td->ushard[tier].size() > 4)
    {
        td->merge_requests.push_back(tier);
        td->merge_cond.signal();
    }

    auto t2 = clock::now();

    uint64_t duration = t2 - t1;

    logger::notice("added {} keys in {:.6f} s, {:.2f} keys/s",
                   data.size(),
                   duration / 1000000000.,
                   data.size() * 1000000000. / duration);
}

void verify_sequential(const data_set_t& data, const thread_data::ushard_t& ushard, tests::stats* s)
{
    tyrdbs::slices_t slices;

    for (auto& slice_list : ushard)
    {
        std::copy(slice_list.begin(), slice_list.end(), std::back_inserter(slices));
    }

    auto db_it = tyrdbs::overwrite_iterator(slices);
    auto data_it = data.begin();

    std::string value;

    uint32_t output_size = 0;

    while (data_it != data.end())
    {
        std::string_view key(string_storage.data() + data_it->first,
                             data_it->second);

        {
            auto sw = s->stopwatch();
            assert(db_it.next() == true);
        }

        while (true)
        {
            assert(db_it.key().compare(key) == 0);
            assert(db_it.deleted() == false);
            assert(db_it.tid() != 0);

            auto value_part = db_it.value();
            value.append(value_part.data(), value_part.size());

            if (db_it.eor() == true)
            {
                break;
            }

            assert(db_it.next() == true);
        }

        assert(key.compare(value) == 0);

        value.clear();

        ++data_it;

        output_size += key.size() + value.size();

        if (output_size > 8192)
        {
            output_size = 0;
            gt::yield();
        }
    }

    assert(db_it.next() == false);
}

void verify_range(const data_set_t& data, const thread_data::ushard_t& ushard, tests::stats* s)
{
    auto data_it = data.begin();

    std::string value;

    while (data_it != data.end())
    {
        std::string_view key(string_storage.data() + data_it->first,
                             data_it->second);

        tyrdbs::slices_t slices;

        for (auto& slice_list : ushard)
        {
            std::copy(slice_list.begin(), slice_list.end(), std::back_inserter(slices));
        }

        auto t1 = clock::now();
        auto db_it = tyrdbs::overwrite_iterator(key, key, slices);

        s->add(clock::now() - t1);

        assert(db_it.next() == true);

        while (true)
        {
            assert(db_it.key().compare(key) == 0);
            assert(db_it.deleted() == false);
            assert(db_it.tid() != 0);

            auto value_part = db_it.value();
            value.append(value_part.data(), value_part.size());

            if (db_it.eor() == true)
            {
                break;
            }

            assert(db_it.next() == true);
        }

        assert(key.compare(value) == 0);

        value.clear();

        ++data_it;

        gt::yield();
    }
}

void merge(thread_data* td, uint8_t tier)
{
    bool compact{tier == static_cast<uint8_t>(-1)};

    tyrdbs::slices_t slices;

    if (compact == true)
    {
        for (auto& slice_list : td->ushard)
        {
            std::copy(slice_list.begin(), slice_list.end(), std::back_inserter(slices));
        }

        if (slices.size() == 1)
        {
            return;
        }
    }
    else
    {
        slices = td->ushard[tier];

        if (slices.size() < 4)
        {
            return;
        }
    }

    char buff[37];
    tyrdbs::slice_writer w("{}.dat", uuid().str(buff, sizeof(buff)));

    auto t1 = clock::now();

    auto db_it = tyrdbs::overwrite_iterator(slices);

    w.add(&db_it, compact);
    w.flush();

    auto slice = std::make_shared<tyrdbs::slice>(w.commit(), "{}", w.path());

    if (compact == true)
    {
        for (auto& slice_list : td->ushard)
        {
            slice_list.clear();
        }
    }
    else
    {
        td->ushard[tier].erase(td->ushard[tier].begin(),
                               td->ushard[tier].begin() + slices.size());
    }

    auto merged_keys = slice->key_count();
    auto new_tier = tier_of(slice);

    td->ushard[new_tier].push_back(std::move(slice));

    if (td->ushard[new_tier].size() > 4)
    {
        td->merge_requests.push_back(new_tier);
        td->merge_cond.signal();
    }

    for (auto& slice : slices)
    {
        slice->unlink();
    }

    auto t2 = clock::now();

    if (merged_keys != 0)
    {
        uint64_t duration = t2 - t1;

        logger::notice("merged {} keys in {:.6f} s, {:.2f} keys/s",
                       merged_keys,
                       duration / 1000000000.,
                       merged_keys * 1000000000. / duration);
    }
}

void merge_thread(thread_data* td)
{
    while (true)
    {
        if (td->merge_requests.size() != 0)
        {
            uint32_t tier = td->merge_requests[0];
            td->merge_requests.erase(td->merge_requests.begin());

            merge(td, tier);

            gt::yield();
        }
        else
        {
            if (td->exit_merge == true)
            {
                break;
            }
            else
            {
                td->merge_cond.wait();
            }
        }
    }

    if (td->compact == true)
    {
        merge(td, static_cast<uint8_t>(-1));
    }

    td->merge_done = true;
    td->done_cond.signal();
}


void test(const data_sets_t* data,
          const data_set_t* test_data,
          thread_data* td,
          bool compact)
{
    auto t1 = clock::now();

    gt::create_thread(merge_thread, td);

    for (auto&& set : *data)
    {
        insert(set, td);
    }

    td->compact = compact;

    td->exit_merge = true;
    td->merge_cond.signal();

    while (td->merge_done == false)
    {
        td->done_cond.wait();
    }

    verify_sequential(*test_data, td->ushard, &td->vs_stats);
    verify_range(*test_data, td->ushard, &td->vr_stats);

    auto t2 = clock::now();

    td->duration = t2 - t1;

    //td.ushard->drop();
}

data_set_t load_data(FILE* fp)
{
    data_set_t data;

    char key[4096];

    while (fscanf(fp, "%s", key) == 1)
    {
        bool batch_done = true;

        batch_done &= key[0] == '=';
        batch_done &= key[1] == '=';
        batch_done &= key[2] == '\0';

        if (batch_done == true)
        {
            break;
        }

        uint64_t offset = string_storage.size();
        uint16_t size = strlen(key);

        string_storage.append(key, size);

        data.push_back(string_t(offset, size));
    }

    return data;
}

data_sets_t load_data(const std::string_view& path)
{
    data_sets_t data;

    FILE* fp = fopen(path.data(), "r");
    assert(fp != nullptr);

    while (true)
    {
        data_set_t set = load_data(fp);

        if (set.size() == 0)
        {
            break;
        }

        data.push_back(std::move(set));
    }

    fclose(fp);

    return data;
}

data_set_t load_test_data(const std::string_view& path)
{
    FILE* fp = fopen(path.data(), "r");
    assert(fp != nullptr);

    data_set_t set = load_data(fp);

    fclose(fp);

    return set;
}

void report(thread_data* t)
{
    logger::notice("");
    logger::notice("thread #{} took \033[1m{:.6f} s\033[0m",
                   t->thread_id,
                   t->duration / 1000000000.);

    logger::notice("");
    logger::notice("sequential read [ns]:");
    t->vs_stats.report();

    logger::notice("");
    logger::notice("random read [ns]:");
    t->vr_stats.report();
}

int main(int argc, const char* argv[])
{
    cmd_line cmd(argv[0], "ushard layer test.", nullptr);

    cmd.add_param("storage-queue-depth",
                  nullptr,
                  "storage-queue-depth",
                  "num",
                  "32",
                  {"storage queue depth to use (default is 32)"});

    cmd.add_param("cpu",
                  nullptr,
                  "cpu",
                  "index",
                  "0",
                  {"cpu index to run the program on (default is 0)"});

    cmd.add_param("threads",
                  nullptr,
                  "threads",
                  "num",
                  "4",
                  {"number of threads to use (default is 4)"});

    cmd.add_param("cache-bits",
                  nullptr,
                  "cache-bits",
                  "bits",
                  "12",
                  {"cache size expressed as 2^bits (default is 12)"});

    cmd.add_flag("compact",
                 nullptr,
                 "compact",
                 {"compact ushard before reading"});

    cmd.add_param("input-data",
                  nullptr,
                  "input-data",
                  "file",
                  "input.data",
                  {"input data file (default input.data)"});

    cmd.add_param("test-data",
                  nullptr,
                  "test-data",
                  "file",
                  "test.data",
                  {"test data file (default test.data)"});

    cmd.parse(argc, argv);

    set_cpu(cmd.get<uint32_t>("cpu"));

    auto&& data = load_data(cmd.get<std::string_view>("input-data"));
    auto&& test_data = load_test_data(cmd.get<std::string_view>("test-data"));

    assert(crc32c_initialize() == true);

    gt::initialize();
    gt::async::initialize();
    io::initialize(4096);
    io::file::initialize(cmd.get<uint32_t>("storage-queue-depth"));

    tyrdbs::cache::initialize(cmd.get<uint32_t>("cache-bits"));

    std::vector<thread_data> td;

    for (uint32_t i = 0; i < cmd.get<uint32_t>("threads"); i++)
    {
        td.push_back(thread_data(i));
    }

    for (uint32_t i = 0; i < td.size(); i++)
    {
        gt::create_thread(test, &data, &test_data, &td[i], cmd.flag("compact"));
    }

    auto t1 = clock::now();

    gt::run();

    auto t2 = clock::now();

    logger::notice("");
    logger::notice("execution took \033[1m{:.6f} s\033[0m",
                   (t2 - t1) / 1000000000.);

    for (auto&& t : td)
    {
        report(&t);
    }

    return 0;
}

#include <common/cmd_line.h>
#include <common/cpu_sched.h>
#include <common/clock.h>
#include <common/logger.h>
#include <gt/engine.h>
#include <io/engine.h>
#include <net/rpc_request.h>
#include <net/uri.h>

#include <tests/db_server_service.json.h>
#include <tests/data.json.h>

#include <random>


using namespace tyrtech;


struct reader
{
    std::vector<uint64_t>::iterator iterator;
    std::vector<uint64_t>::iterator end;
    uint64_t key{0};
    uint64_t value{0};
    std::string value_part;
};


bool fill_entries(reader* r,
                  message::builder* builder,
                  uint32_t group_bits,
                  uint32_t ushard_bits)
{
    uint16_t data_flags = 0;

    auto data = tests::data_builder(builder);

    auto&& dbs = data.add_collections();
    auto&& db = dbs.add_value();
    auto&& entries = db.add_entries();

    while (true)
    {
        uint64_t key = __builtin_bswap64(r->key);
        std::string_view key_str(reinterpret_cast<const char*>(&key), sizeof(key));

        uint8_t entry_flags = 0;

        entry_flags |= 0x01;
        //flags |= 0x02;

        uint16_t bytes_required = 0;

        bytes_required += tests::entry_builder::bytes_required();
        bytes_required += tests::entry_builder::key_bytes_required();
        bytes_required += tests::entry_builder::value_bytes_required();
        bytes_required += tests::entry_builder::ushard_bytes_required();
        bytes_required += sizeof(key);

        if (builder->available_space() <= bytes_required)
        {
            break;
        }

        uint16_t part_size = builder->available_space();
        part_size -= bytes_required;

        part_size = std::min(part_size, static_cast<uint16_t>(r->value_part.size()));

        if (part_size != r->value_part.size())
        {
            entry_flags &= ~0x01;
        }

        uint32_t ushard = (__builtin_bswap64(key) >> group_bits) & ((1UL << ushard_bits) - 1);

        auto&& entry = entries.add_value();

        entry.set_flags(entry_flags);
        entry.add_key(key_str);
        entry.add_value(r->value_part.substr(0, part_size));
        entry.add_ushard(ushard);

        if (part_size != r->value_part.size())
        {
            r->value_part = r->value_part.substr(part_size);

            break;
        }
        else
        {
            r->key++;
        }

        if (++r->iterator == r->end)
        {
            data_flags |= 0x01;
            break;
        }

        r->value = *r->iterator;
        r->value_part = std::string_view(reinterpret_cast<const char*>(&r->value),
                                         sizeof(r->value));
    }

    data.set_flags(data_flags);

    return (data_flags & 0x01) == 0x01;
}


void update_thread(const std::string_view& uri,
                   uint32_t seed,
                   uint32_t iterations,
                   uint32_t keys,
                   uint32_t group_bits,
                   uint32_t ushard_bits,
                   uint32_t target_rate,
                   FILE* stats_fd)
{
    net::socket_channel channel(net::uri::connect(uri, 0), 0);

    std::mt19937 generator(seed);
    std::uniform_int_distribution<uint32_t> distribution(0, static_cast<uint32_t>(-1));

    auto t0 = clock::now();

    uint64_t inserted_keys = 0;

    for (uint32_t i = 0; i < iterations; i++)
    {
        std::vector<uint64_t> value_set;

        while (value_set.size() != keys)
        {
            value_set.push_back(distribution(generator));
        }

        reader r;
        r.iterator = value_set.begin();
        r.end = value_set.end();
        r.key = static_cast<uint64_t>(i) * keys;

        r.value = *r.iterator;
        r.value_part = std::string_view(reinterpret_cast<const char*>(&r.value),
                                        sizeof(r.value));

        uint64_t handle = 0;

        auto t1 = clock::now();

        while (true)
        {
            net::rpc_request<tests::collections::update_data> request(&channel);
            auto message = request.add_message();

            if (handle != 0)
            {
                message.add_handle(handle);
            }

            bool done = fill_entries(&r, message.add_data(), group_bits, ushard_bits);

            request.execute();
            auto response = request.wait();

            if (handle == 0)
            {
                assert(response.has_handle() == true);
                handle = response.handle();
            }
            else
            {
                assert(response.has_handle() == false);
            }

            if (done == true)
            {
                break;
            }

            assert(handle != 0);
        }

        net::rpc_request<tests::collections::commit_update> request(&channel);
        auto message = request.add_message();

        message.add_handle(handle);

        request.execute();
        request.wait();

        auto t2 = clock::now();

        inserted_keys += keys;

        uint64_t iter_t = t2 - t1;
        uint64_t total_t = t2 - t0;

        logger::notice("#{} updated {} keys ({} groups) in {:.2f} ms, {:.2f} keys/s",
                       i,
                       keys,
                       keys >> group_bits,
                       iter_t / 1000000.,
                       inserted_keys * 1000000000. / total_t);

        if (stats_fd != nullptr)
        {
            fmt::print(stats_fd,
                       "{},{},{},{}\n",
                       t2 / 1000,
                       keys,
                       group_bits,
                       iter_t);

            fflush(stats_fd);
        }

        if (target_rate != 0)
        {
            uint64_t target_t = inserted_keys * 1000000000 / target_rate;

            if (target_t > total_t)
            {
                uint64_t delay = target_t - total_t;
                gt::sleep(delay / 1000000);
            }
        }
    }
}


int main(int argc, const char* argv[])
{
    cmd_line cmd(argv[0], "Network update client demo.", nullptr);

    cmd.add_param("seed",
                  nullptr,
                  "seed",
                  "num",
                  "0",
                  {"pseudo random seed to use (default is 0)"});

    cmd.add_param("network-queue-depth",
                  nullptr,
                  "network-queue-depth",
                  "num",
                  "512",
                  {"network queue depth to use (default is 512)"});

    cmd.add_param("cpu",
                  nullptr,
                  "cpu",
                  "index",
                  "0",
                  {"cpu index to run the program on (default is 0)"});

    cmd.add_param("iterations",
                  nullptr,
                  "iterations",
                  "num",
                  "10000",
                  {"iterations to run (default is 10000)"});

    cmd.add_param("keys",
                  nullptr,
                  "keys",
                  "num",
                  "10000",
                  {"keys to update in one iteration (default: 10000)"});

    cmd.add_param("group-bits",
                  nullptr,
                  "group-bits",
                  "num",
                  "9",
                  {"number of bits to group values with (default: 9)"});

    cmd.add_param("ushard-bits",
                  nullptr,
                  "ushard-bits",
                  "num",
                  "5",
                  {"number of bits to use for usharding (default: 5)"});

    cmd.add_param("update-rate",
                  nullptr,
                  "update-rate",
                  "num",
                  "0",
                  {"update rate limit in keys per second (default: unlimited)"});

    cmd.add_param("stats-output",
                  nullptr,
                  "stats-output",
                  "file",
                  nullptr,
                  {"output csv formatted stats to file"});

    cmd.add_param("uri",
                  "<uri>",
                  {"uri to connect to"});

    cmd.parse(argc, argv);

    set_cpu(cmd.get<uint32_t>("cpu"));

    gt::initialize();
    io::initialize(4096);
    io::socket::initialize(cmd.get<uint32_t>("network-queue-depth"));

    FILE* stats_fd = nullptr;

    if (cmd.has("stats-output") == true)
    {
        stats_fd = fopen(cmd.get<std::string_view>("stats-output").data(), "w");
        assert(stats_fd != nullptr);

        fmt::print(stats_fd, "#ts[us],keys,group_bits,duration[ns]\n");
    }

    gt::create_thread(update_thread,
                      cmd.get<std::string_view>("uri"),
                      cmd.get<uint32_t>("seed"),
                      cmd.get<uint32_t>("iterations"),
                      cmd.get<uint32_t>("keys"),
                      cmd.get<uint32_t>("group-bits"),
                      cmd.get<uint32_t>("ushard-bits"),
                      cmd.get<uint32_t>("update-rate"),
                      stats_fd);

    gt::run();

    if (stats_fd != nullptr)
    {
        fclose(stats_fd);
    }

    return 0;
}

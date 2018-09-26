#define BOOST_TEST_MODULE btree
#include <boost/asio/io_service.hpp>
#include <boost/test/included/unit_test.hpp>
#include <boost/optional.hpp>

#include <cache/btree.h>
#include <namespaces.h>
#include <iostream>

#include "or_throw.h"

BOOST_AUTO_TEST_SUITE(btree)

using namespace std;
using namespace ouinet;

using boost::optional;

BOOST_AUTO_TEST_CASE(test_1)
{
    BTree db;
    
    asio::io_service ios;

    asio::spawn(ios, [&](asio::yield_context yield) {
        sys::error_code ec;
        db.insert("key", "value", yield[ec]);
        BOOST_REQUIRE(!ec);
        optional<string> v = db.find("key", yield[ec]);
        BOOST_REQUIRE(!ec);
        BOOST_REQUIRE(v);
        BOOST_REQUIRE_EQUAL(*v, "value");
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_2)
{
    srand(time(NULL));

    BTree db(nullptr, nullptr, nullptr, 256);

    set<string> inserted;

    asio::io_service ios;

    asio::spawn(ios, [&](asio::yield_context yield) {
        sys::error_code ec;

        for (int i = 0; i < 3000; ++i) {
            int k = rand();
            stringstream ss;
            ss << k;
            db.insert(ss.str(), ss.str(), yield[ec]);
            inserted.insert(ss.str());
            BOOST_REQUIRE(!ec);
        }

        BOOST_REQUIRE(db.check_invariants());

        for (auto& key : inserted) {
            auto val = db.find(key, yield[ec]);
            BOOST_REQUIRE(!ec);
            BOOST_REQUIRE_EQUAL(key, val);
        }
    });

    ios.run();
}

void random_wait( unsigned range
                , asio::io_service& ios
                , asio::yield_context yield)
{
    if (range == 0) return;
    auto cnt = rand() % range;
    for (unsigned i = 0; i < cnt; ++i) ios.post(yield);
}

struct MockStorage : public std::map<BTree::Hash, BTree::Value> {
    using Map = std::map<BTree::Hash, BTree::Value>;

    MockStorage(asio::io_service& ios, unsigned async_deviation = 0)
        : _ios(ios)
        , _async_deviation(async_deviation) {}

    BTree::CatOp cat_op() {
        return [this] (BTree::Hash hash, asio::yield_context yield) {
            random_wait(_async_deviation, _ios, yield);

            auto i = Map::find(hash);
            if (i == Map::end()) {
                return or_throw<BTree::Value>(yield, asio::error::not_found);
            }

            return i->second;
        };
    }

    BTree::AddOp add_op() {
        return [this] (BTree::Value value , asio::yield_context yield) {
            random_wait(_async_deviation, _ios, yield);

            std::stringstream ss;
            ss << next_id++;
            auto id = ss.str();
            Map::operator[](id) = std::move(value);

            return id;
        };
    }

    BTree::RemoveOp remove_op() {
        return [this] (const BTree::Hash& h, asio::yield_context yield) {
            random_wait(_async_deviation, _ios, yield);
            Map::erase(h);
        };
    }

private:
    size_t next_id = 0;;
    asio::io_service& _ios;
    unsigned _async_deviation;
};

string random_key(unsigned len) {
    stringstream ss;
    for (unsigned i = 0; i < len; ++i) ss << (rand() % 10);
    return ss.str();
}

BOOST_AUTO_TEST_CASE(test_3)
{
    srand(time(NULL));

    set<string> inserted;

    asio::io_service ios;

    MockStorage storage(ios);

    BTree db(storage.cat_op(), storage.add_op(), storage.remove_op(), 2);

    asio::spawn(ios, [&](asio::yield_context yield) {
        sys::error_code ec;

        string root_hash;

        for (int i = 0; i < 100; ++i) {
            auto k = random_key(5);
            db.insert(k, "v" + k, yield[ec]);
            inserted.insert(k);
            BOOST_REQUIRE(!ec);
        }

        BOOST_REQUIRE(db.check_invariants());

        BOOST_REQUIRE_EQUAL(storage.size(), db.local_node_count());

        for (auto& key : inserted) {
            auto val = db.find(key, yield[ec]);
            BOOST_REQUIRE(!ec);
            BOOST_REQUIRE_EQUAL("v" + key, val);
        }

        BTree db2(storage.cat_op(), storage.add_op(), storage.remove_op(), 2);

        db2.load(db.root_hash(), yield[ec]);
        BOOST_REQUIRE(!ec);

        for (auto& key : inserted) {
            auto val = db2.find(key, yield[ec]);
            BOOST_REQUIRE(!ec);
            BOOST_REQUIRE_EQUAL("v" + key, val);
        }

        ec = sys::error_code();

        auto i = db.begin(yield[ec]);

        BOOST_REQUIRE(!ec);

        while (!i.is_end()) {
            BOOST_REQUIRE(!inserted.empty());
            BOOST_REQUIRE_EQUAL("v" + *inserted.begin(), i.value());

            i.advance(yield[ec]);

            BOOST_REQUIRE(!ec);
            inserted.erase(inserted.begin());
        }
    });

    ios.run();
}

// Test that doing BTree::load while BTree::find doesn't crash the app.
BOOST_AUTO_TEST_CASE(test_4)
{
    srand(time(NULL));

    asio::io_service ios;

    MockStorage storage(ios, 10);

    /* We can't let it remove items from MockStorage because MockStorage
     * doesn't currently keep refcount per value */
    BTree db1(storage.cat_op(), storage.add_op(), nullptr, 2);
    BTree db2(storage.cat_op(), storage.add_op(), nullptr, 2);
    BTree db3(storage.cat_op(), storage.add_op(), nullptr, 2);

    auto int_to_string = [](int i) {
        assert(i < 1000);
        stringstream ss;
        ss << i;
        auto k = ss.str();
        return string(3-k.size(), '0') + k;
    };

    auto fill_db = [&](BTree& db, asio::yield_context yield) {
        for (int i = 0; i < 1000; ++i) {
            string k = int_to_string(i);
            sys::error_code ec;
            db.insert(k, "v" + k, yield[ec]);
            BOOST_REQUIRE(!ec);
        }
    };

    asio::spawn(ios, [&](asio::yield_context yield) {
        sys::error_code ec;

        fill_db(db2, yield);
        fill_db(db3, yield);

        db1.load(db2.root_hash(), yield);

        auto done = false;

        asio::spawn(ios, [&](asio::yield_context yield) {
            for (int i = 0; i < 1000; ++i) {
                sys::error_code ec;
                auto k = random_key(3);
                db1.find(k, yield[ec]);
                BOOST_REQUIRE(!ec);
                ios.post(yield);
            }
            done = true;
        });

        // This was experimentally chosen so that a database is switched evenly
        // during and between consecutive calls to BTree::find.
        const unsigned WAIT_RANGE = 20;

        // Note that we intentionally don't do the following DB switching in
        // the above loop because we want them to happen *while db1.find is
        // running* (not only before or after).
        while (!done) {
            if (db1.root_hash() == db2.root_hash()) {
                db1.load(db3.root_hash(), yield);
            }
            else {
                db1.load(db2.root_hash(), yield);
            }
            random_wait(WAIT_RANGE, ios, yield);
        }
    });

    ios.run();
}

BOOST_AUTO_TEST_SUITE_END()

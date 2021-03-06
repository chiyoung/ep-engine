/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include <string.h>
#include <cstdlib>
#include <cctype>
#include <algorithm>

#include "sqlite-kvstore.hh"
#include "sqlite-pst.hh"
#include "vbucket.hh"
#include "ep_engine.h"
#include "warmup.hh"

#define STATWRITER_NAMESPACE sqlite_engine
#include "statwriter.hh"
#undef STATWRITER_NAMESPACE

StrategicSqlite3::StrategicSqlite3(EPStats &st, shared_ptr<SqliteStrategy> s,
                                   bool read_only) :
    KVStore(read_only), stats(st), strategy(s), intransaction(false) {
    open();
}

StrategicSqlite3::StrategicSqlite3(const StrategicSqlite3 &from) :
    KVStore(from), stats(from.stats),
    strategy(from.strategy), intransaction(false) {
    open();
}

int64_t StrategicSqlite3::lastRowId() {
    assert(db);
    return static_cast<int64_t>(sqlite3_last_insert_rowid(db));
}

void StrategicSqlite3::insert(const Item &itm,
                              Callback<mutation_result> &cb) {
    assert(itm.getId() <= 0);

    PreparedStatement *ins_stmt = strategy->getStatements(itm.getVBucketId(),
                                                          itm.getKey())->ins();
    ins_stmt->bind(1, itm.getKey());
    ins_stmt->bind(2, const_cast<Item&>(itm).getData(), itm.getNBytes());
    ins_stmt->bind(3, itm.getFlags());
    ins_stmt->bind(4, itm.getExptime());
    ins_stmt->bind64(5, itm.getCas());
    ins_stmt->bind(6, itm.getVBucketId());

    ++stats.io_num_write;
    stats.io_write_bytes += itm.getKey().length() + itm.getNBytes();

    int rv = ins_stmt->execute();
    if (rv < 0) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "Fatal sqlite error in inserting key '%s' !!! "
                         "Reopen the database...\n",
                         itm.getKey().c_str());
        reopen();
    }

    int64_t newId = lastRowId();

    std::pair<int, int64_t> p(rv, newId);
    cb.callback(p);
    ins_stmt->reset();
}

void StrategicSqlite3::update(const Item &itm,
                              Callback<mutation_result> &cb) {
    assert(itm.getId() > 0);

    PreparedStatement *upd_stmt = strategy->getStatements(itm.getVBucketId(),
                                                          itm.getKey())->upd();

    upd_stmt->bind(1, itm.getKey());
    upd_stmt->bind(2, const_cast<Item&>(itm).getData(), itm.getNBytes());
    upd_stmt->bind(3, itm.getFlags());
    upd_stmt->bind(4, itm.getExptime());
    upd_stmt->bind64(5, itm.getCas());
    upd_stmt->bind64(6, itm.getId());

    int rv = upd_stmt->execute();
    if (rv < 0) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "Fatal sqlite error in updating key '%s' !!! "
                         "Reopen the database...\n",
                         itm.getKey().c_str());
        reopen();
    }
    ++stats.io_num_write;
    stats.io_write_bytes += itm.getKey().length() + itm.getNBytes();

    std::pair<int, int64_t> p(rv, 0);
    cb.callback(p);
    upd_stmt->reset();
}

vbucket_map_t StrategicSqlite3::listPersistedVbuckets() {
    std::map<uint16_t, vbucket_state> rv;

    PreparedStatement *st = strategy->getGetVBucketStateST();

    while (st->fetch()) {
        ++stats.io_num_read;
        uint16_t vbid = st->column_int(0);
        vbucket_state vb_state;
        vb_state.state = (vbucket_state_t)st->column_int(1);
        vb_state.checkpointId = st->column_int64(2);
        vb_state.maxDeletedSeqno = 0;
        rv[vbid] = vb_state;
    }

    st->reset();

    return rv;
}

void StrategicSqlite3::set(const Item &itm,
                           Callback<mutation_result> &cb) {
    assert(!isReadOnly());
    if (itm.getId() <= 0) {
        insert(itm, cb);
    } else {
        update(itm, cb);
    }
}

void StrategicSqlite3::get(const std::string &key, uint64_t rowid,
                           uint16_t vb, Callback<GetValue> &cb) {
    PreparedStatement *sel_stmt = strategy->getStatements(vb, key)->sel();
    sel_stmt->bind64(1, rowid);

    ++stats.io_num_read;

    if(sel_stmt->fetch()) {
        GetValue rv(new Item(key.data(),
                             static_cast<uint16_t>(key.length()),
                             sel_stmt->column_int(1),
                             sel_stmt->column_int(2),
                             sel_stmt->column_blob(0),
                             sel_stmt->column_bytes(0),
                             sel_stmt->column_int64(3),
                             sel_stmt->column_int64(4),
                             static_cast<uint16_t>(sel_stmt->column_int(5))));
        stats.io_read_bytes += key.length() + rv.getValue()->getNBytes();
        cb.callback(rv);
    } else {
        GetValue rv;
        cb.callback(rv);
    }
    sel_stmt->reset();
}

void StrategicSqlite3::reset() {
    assert(!isReadOnly());
    if (db) {
        rollback();
        close();
        open();
        strategy->destroyTables();
        close();
        open();
        execute("vacuum");
    }
}

void StrategicSqlite3::del(const Item &itm, uint64_t rowid,
                           Callback<int> &cb) {
    assert(!isReadOnly());
    int rv = 0;
    if (rowid <= 0) {
        cb.callback(rv);
        return;
    }

    std::string key = itm.getKey();
    uint16_t vb = itm.getVBucketId();
    PreparedStatement *del_stmt = strategy->getStatements(vb, key)->del();
    del_stmt->bind64(1, rowid);
    rv = del_stmt->execute();
    if (rv < 0) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "Fatal sqlite error in deleting key '%s' !!! "
                         "Reopen the database...\n",
                         key.c_str());
        reopen();
    }
    cb.callback(rv);
    del_stmt->reset();
}

bool StrategicSqlite3::delVBucket(uint16_t vbucket) {
    assert(!isReadOnly());
    assert(strategy->hasEfficientVBDeletion());
    bool rv = true;
    std::stringstream tmp_table_name;
    tmp_table_name << "invalid_kv_" << vbucket << "_" << gethrtime();
    rv = begin();
    if (rv) {
        strategy->renameVBTable(vbucket, tmp_table_name.str());
        strategy->createVBTable(vbucket);
        rv = commit();
    }
    if (!rv) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "Fatal sqlite error in deleting vbucket %d !!! "
                         "Reopen the database...\n",
                         vbucket);
        reopen();
    }
    return rv;
}

bool StrategicSqlite3::snapshotVBuckets(const vbucket_map_t &m) {
    assert(!isReadOnly());
    bool rv = storeMap(strategy->getClearVBucketStateST(),
                       strategy->getInsVBucketStateST(), m);
    if (!rv) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "Fatal sqlite error in snapshot vbucket states!!! "
                         "Reopen the database...\n");
        reopen();
    }
    return rv;
}

bool StrategicSqlite3::snapshotStats(const std::map<std::string, std::string> &m) {
    assert(!isReadOnly());
    bool rv = storeMap(strategy->getClearStatsST(), strategy->getInsStatST(), m);
    if (!rv) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "Fatal sqlite error in snapshot engine stats!!! "
                         "Reopen the database...\n");
        reopen();
    }
    return rv;
}

/**
 * Function object to set a series of k,v pairs into a PreparedStatement.
 */
template <typename T1, typename T2>
struct map_setter {

    /**
     * Constructor.
     *
     * @param i the prepared statement to operate on
     * @param o the location of the return value - will set to false upon failure
     */
    map_setter(PreparedStatement *i, bool &o) : insSt(i), output(o) {}

    PreparedStatement *insSt;
    bool &output;

    void operator() (const std::pair<T1, T2> &p) {
        int pos = 1;
        pos += insSt->bind(pos, p.first);
        insSt->bind(pos, p.second);

        bool inserted = insSt->execute() == 1;
        insSt->reset();
        output &= inserted;
    }
};

template <typename T1, typename T2>
bool StrategicSqlite3::storeMap(PreparedStatement *clearSt,
                                PreparedStatement *insSt,
                                const std::map<T1, T2> &m) {
    bool rv(false);
    if (!begin()) {
        return false;
    }
    try {
        bool deleted = clearSt->execute() >= 0;
        rv &= deleted;
        clearSt->reset();

        map_setter<T1, T2> ms(insSt, rv);
        std::for_each(m.begin(), m.end(), ms);

        rv = commit();
    } catch(...) {
        rollback();
    }
    return rv;
}

static void processDumpRow(EPStats &stats,
                           PreparedStatement *st, shared_ptr<Callback<GetValue> > &cb) {
    ++stats.io_num_read;
    GetValue rv(new Item(st->column_blob(0),
                         static_cast<uint16_t>(st->column_bytes(0)),
                         st->column_int(2),
                         st->column_int(3),
                         st->column_blob(1),
                         st->column_bytes(1),
                         st->column_int64(4),
                         st->column_int64(6),
                         static_cast<uint16_t>(st->column_int(5))),
                ENGINE_SUCCESS,
                -1);
    stats.io_read_bytes += rv.getValue()->getKey().length() + rv.getValue()->getNBytes();
    cb->callback(rv);
}

void StrategicSqlite3::dump(shared_ptr<Callback<GetValue> > cb) {
    const std::vector<Statements*> statements = strategy->allStatements();
    std::vector<Statements*>::const_iterator it;
    for (it = statements.begin(); it != statements.end(); ++it) {
        PreparedStatement *st = (*it)->all();
        st->reset();
        while (st->fetch()) {
            processDumpRow(stats, st, cb);
        }

        st->reset();
    }
}

void StrategicSqlite3::dump(uint16_t vb, shared_ptr<Callback<GetValue> > cb) {
    assert(strategy->hasEfficientVBLoad());
    std::vector<PreparedStatement*> loaders(strategy->getVBStatements(vb, select_all));

    std::vector<PreparedStatement*>::iterator it;
    for (it = loaders.begin(); it != loaders.end(); ++it) {
        PreparedStatement *st = *it;
        while (st->fetch()) {
            processDumpRow(stats, st, cb);
        }
    }

    strategy->closeVBStatements(loaders);
}


static char lc(const char i) {
    return std::tolower(i);
}

StorageProperties StrategicSqlite3::getStorageProperties() {
    // Verify we at least compiled in mutexes.
    assert(sqlite3_threadsafe());
    bool allows_concurrency(false);
    {
        PreparedStatement st(db, "pragma journal_mode");
        static const std::string wal_str("wal");
        if (st.fetch()) {
            std::string s(st.column(0));
            std::transform(s.begin(), s.end(), s.begin(), lc);
            getLogger()->log(EXTENSION_LOG_INFO, NULL,
                             "journal-mode:  %s\n", s.c_str());
            allows_concurrency = s == wal_str;
        }
    }
    if (allows_concurrency) {
        PreparedStatement st(db, "pragma read_uncommitted");
        if (st.fetch()) {
            allows_concurrency = st.column_int(0) == 1;
            getLogger()->log(EXTENSION_LOG_INFO, NULL,
                             "read_uncommitted:  %s\n",
                             allows_concurrency ? "yes" : "no");
        }
    }
    size_t concurrency(allows_concurrency ? 10 : 1);
    StorageProperties rv(concurrency, concurrency - 1, 1,
                         strategy->hasEfficientVBLoad(),
                         strategy->hasEfficientVBDeletion(),
                         strategy->hasPersistedDeletions(), false);
    return rv;
}

void StrategicSqlite3::addStats(const std::string &prefix,
                             ADD_STAT add_stat, const void *c) {
    if (prefix != "rw") {
        return;
    }

    SQLiteStats &st(strategy->sqliteStats);
    add_casted_stat("sector_size", st.sectorSize, add_stat, c);
    add_casted_stat("open", st.numOpen, add_stat, c);
    add_casted_stat("close", st.numClose, add_stat, c);
    add_casted_stat("lock", st.numLocks, add_stat, c);
    add_casted_stat("truncate", st.numTruncates, add_stat, c);
}


void StrategicSqlite3::addTimingStats(const std::string &prefix,
                                    ADD_STAT add_stat, const void *c) {
    if (prefix != "rw") {
        return;
    }

    SQLiteStats &st(strategy->sqliteStats);
    add_casted_stat("delete", st.deleteHisto, add_stat, c);
    add_casted_stat("sync", st.syncTimeHisto, add_stat, c);
    add_casted_stat("readTime", st.readTimeHisto, add_stat, c);
    add_casted_stat("readSeek", st.readSeekHisto, add_stat, c);
    add_casted_stat("readSize", st.readSizeHisto, add_stat, c);
    add_casted_stat("writeTime", st.writeTimeHisto, add_stat, c);
    add_casted_stat("writeSeek", st.writeSeekHisto, add_stat, c);
    add_casted_stat("writeSize", st.writeSizeHisto, add_stat, c);
}

typedef std::map<std::string, std::list<uint64_t> > ShardRowidMap;

struct WarmupCookie {
    WarmupCookie(StrategicSqlite3 *s) :
        store(s)
    { /* EMPTY */ }
    StrategicSqlite3 *store;
    ShardRowidMap objmap;
};

static void warmupCallback(void *arg, uint16_t vb,
                           const std::string &key, uint64_t rowid)
{
    WarmupCookie *cookie = static_cast<WarmupCookie*>(arg);
    cookie->objmap[cookie->store->getKvTableName(key, vb)].push_back(rowid);
}

size_t StrategicSqlite3::warmup(MutationLog &lf,
                                const std::map<uint16_t, vbucket_state> &vbmap,
                                Callback<GetValue> &cb,
                                Callback<size_t> &estimate)
{
    // First build up the various maps...

    MutationLogHarvester harvester(lf);
    std::map<uint16_t, vbucket_state>::const_iterator it;
    for (it = vbmap.begin(); it != vbmap.end(); ++it) {
        harvester.setVBucket(it->first);
    }

    hrtime_t start = gethrtime();
    if (!harvester.load()) {
        return -1;
    }
    hrtime_t end = gethrtime();

    size_t total = harvester.total();
    estimate.callback(total);

    getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                     "Completed log read in %s with %ld entries\n",
                     hrtime2text(end - start).c_str(), total);

    start = gethrtime();
    WarmupCookie cookie(this);
    harvester.apply(&cookie, &warmupCallback);

    // Ok, run through all of the lists and apply each one of them..
    total = 0;
    for (ShardRowidMap::iterator iter = cookie.objmap.begin();
         iter != cookie.objmap.end();
         ++iter) {

        total += warmupSingleShard(iter->first, iter->second, cb);
    }
    end = gethrtime();

    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                     "Warmed up %ld items in %s", (long)total,
                     hrtime2text(end - start).c_str());

    return total;
}

static bool list_compare(uint64_t first, uint64_t second) {
    return first < second;
}

size_t StrategicSqlite3::warmupSingleShard(const std::string &table,
                                           std::list<uint64_t> &ids,
                                           Callback<GetValue> &cb)
{
    using namespace std;

    string prefix("select k, v, flags, exptime, cas, vbucket, rowid from ");
    prefix.append(table);
    prefix.append(" where rowid in (");

    ids.sort(list_compare);

    list<uint64_t>::iterator iter = ids.begin();

    size_t batchSize;
    if (engine) {
        batchSize = engine->getConfiguration().getWarmupBatchSize();
    } else {
        batchSize = 1000;
    }
    size_t ret = 0;
    do {
        stringstream ss;
        size_t num = 0;

        // @todo make this configurable
        while (iter != ids.end() && num < batchSize) {
            ss << *iter << ",";
            ++num; ++iter;
        }

        if (num != 0) {
            // we need to execute the query
            string query = prefix + ss.str();
            query.resize(query.length() - 1);
            query.append(")");

            PreparedStatement st(db, query.c_str());
            while (st.fetch()) {
                ++ret;
                Item *it = new Item(st.column_blob(0),
                                    static_cast<uint16_t>(st.column_bytes(0)),
                                    st.column_int(2), st.column_int(3),
                                    st.column_blob(1), st.column_bytes(1),
                                    st.column_int64(4), st.column_int64(6),
                                    static_cast<uint16_t>(st.column_int(5)));
                GetValue rv(it, ENGINE_SUCCESS, -1);
                cb.callback(rv);
            }
        }
    } while (iter != ids.end());

    return ret;
}

bool StrategicSqlite3::getEstimatedItemCount(size_t &nItems) {
    if (getenv("COUCHBASE_FORCE_SQLITE_ESTIMATE_COUNT") != NULL) {
        size_t num = 0;

        const std::vector<Statements*> statements = strategy->allStatements();
        std::vector<Statements*>::const_iterator it;
        for (it = statements.begin(); it != statements.end(); ++it) {
            PreparedStatement *st = (*it)->count_all();
            st->reset();
            if (st->fetch()) {
                num += static_cast<size_t>(st->column_int(0));
            }
            st->reset();
        }

        nItems = num;
        return true;
    } else {
        return false;
    }
}

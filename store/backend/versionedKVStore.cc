// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
// vim: set ts=4 sw=4:
/***********************************************************************
 *
 * common/versionedKVStore.cc:
 *   Timestamped version store
 *
 **********************************************************************/

#include "common/versionedKVStore.h"

using namespace std;

VersionedKVStore::VersionedKVStore() { }
    
VersionedKVStore::~VersionedKVStore() { }

bool
VersionedKVStore::inStore(const string &key)
{
    return store.find(key) != store.end() && store[key].size() > 0;
}

void
VersionedKVStore::getValue(const string &key, const Timestamp &t, set<VersionedKVStore::VersionedValue>::iterator &it)
{
    VersionedValue v(t);
    it = store[key].upper_bound(v);

    // if there is no valid version at this timestamp
    if (it == store[key].begin()) {
        it = store[key].end();
    } else {
        it--;
    }
}


/* Returns the most recent value and timestamp for given key.
 * Error if key does not exist. */
bool
VersionedKVStore::get(const string &key, pair<Timestamp, string> &value)
{
    // check for existence of key in store
    if (inStore(key)) {
        VersionedValue v = *(store[key].rbegin());
        value = make_pair(v.write, v.value);
        return true;
    }
    return false;
}
    
/* Returns the value valid at given timestamp.
 * Error if key did not exist at the timestamp. */
bool
VersionedKVStore::get(const string &key, const Timestamp &t, pair<Timestamp, string> &value)
{
    if (inStore(key)) {
        set<VersionedValue>::iterator it;
        getValue(key, t, it);
        if (it != store[key].end()) {
            value = make_pair((*it).write, (*it).value);
            return true;
        }
    }
    return false;
}

bool
VersionedKVStore::getRange(const string &key, const Timestamp &t,
			   pair<Timestamp, Timestamp> &range)
{
    if (inStore(key)) {
        set<VersionedValue>::iterator it;
        getValue(key, t, it);

        if (it != store[key].end()) {
            range.first = (*it).write;
            it++;
            if (it != store[key].end()) {
                range.second = (*it).write;
            }
            return true;
        }
    }
    return false;
}

void
VersionedKVStore::put(const string &key, const string &value, const Timestamp &t)
{
    // Key does not exist. Create a list and an entry.
    store[key].insert(VersionedValue(t, value));
}

/*
 * Commit a read by updating the timestamp of the latest read txn for
 * the version of the key that the txn read.
 */
void
VersionedKVStore::commitGet(const string &key, const Timestamp &readTime, const Timestamp &commit)
{
    // Hmm ... could read a key we don't have if we are behind ... do we commit this or wait for the log update?
    if (inStore(key)) {
        set<VersionedValue>::iterator it;
        getValue(key, readTime, it);
        
        if (it != store[key].end()) {
            // figure out if anyone has read this version before
            if (lastReads.find(key) != lastReads.end() &&
                lastReads[key].find((*it).write) != lastReads[key].end()) {
                if (lastReads[key][(*it).write] < commit) {
                    lastReads[key][(*it).write] = commit;
                }
            }
        }
    } // otherwise, ignore the read
}

bool
VersionedKVStore::getLastRead(const string &key, Timestamp &lastRead)
{
    if (inStore(key)) {
        VersionedValue v = *(store[key].rbegin());
        if (lastReads.find(key) != lastReads.end() &&
            lastReads[key].find(v.write) != lastReads[key].end()) {
            lastRead = lastReads[key][v.write];
            return true;
        }
    }
    return false;
}    

/*
 * Get the latest read for the write valid at timestamp t
 */
bool
VersionedKVStore::getLastRead(const string &key, const Timestamp &t, Timestamp &lastRead)
{
    if (inStore(key)) {
        set<VersionedValue>::iterator it;
        getValue(key, t, it);
        ASSERT(it != store[key].end());

        // figure out if anyone has read this version before
        if (lastReads.find(key) != lastReads.end() &&
            lastReads[key].find((*it).write) != lastReads[key].end()) {
            lastRead = lastReads[key][(*it).write];
            return true;
        }
    }
    return false;	
}

// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
// vim: set ts=4 sw=4:
/***********************************************************************
 *
 * spanstore/client.cc:
 *   SpanStore client implementation.
 *
 **********************************************************************/

#include "spanstore/client.h"

using namespace std;

namespace spanstore {

Client::Client(Mode mode, string configPath, int nShards,
                int closestReplica, TrueTime timeServer)
    : transport(0.0, 0.0, 0), mode(mode), timeServer(timeServer)
{
    // Initialize all state here;
    client_id = 0;
    while (client_id == 0) {
        random_device rd;
        mt19937_64 gen(rd());
        uniform_int_distribution<uint64_t> dis;
        client_id = dis(gen);
    }
    t_id = (client_id/10000)*10000;

    nshards = nShards;
    bclient.reserve(nshards);

    Debug("Initializing SpanStore client with id [%lu]", client_id);

    /* Start a client for time stamp server. */
    if (mode == MODE_OCC) {
        string tssConfigPath = configPath + ".tss.config";
        ifstream tssConfigStream(tssConfigPath);
        if (tssConfigStream.fail()) {
            fprintf(stderr, "unable to read configuration file: %s\n",
                    tssConfigPath.c_str());
        }
        specpaxos::Configuration tssConfig(tssConfigStream);
        tss = new specpaxos::vr::VRClient(tssConfig, &transport);
    }

    /* Start a client for each shard. */
    for (int i = 0; i < nShards; i++) {
        string shardConfigPath = configPath + to_string(i) + ".config";
        SpanClient *spanclient = new SpanClient(mode, shardConfigPath,
            &transport, client_id, i, closestReplica);
        bclient[i] = new BufferClient(spanclient);
    }

    /* Run the transport in a new thread. */
    clientTransport = new thread(&Client::run_client, this);

    Debug("SpanStore client [%lu] created!", client_id);
}

Client::~Client()
{
    transport.Stop();
    delete tss;
    for (auto b : bclient) {
        delete b;
    }
    clientTransport->join();
}

/* Runs the transport event loop. */
void
Client::run_client()
{
    transport.Run();
}

/* Begins a transaction. All subsequent operations before a commit() or
 * abort() are part of this transaction.
 *
 * Return a TID for the transaction.
 */
void
Client::Begin()
{
    Debug("BEGIN Transaction");
    t_id++;
    participants.clear();
    commit_sleep = -1;
    for (int i = 0; i < nshards; i++) {
        bclient[i]->Begin(t_id);
    }
}

/* Returns the value corresponding to the supplied key. */
int
Client::Get(const string &key, string &value)
{
    // Contact the appropriate shard to get the value.
    int i = key_to_shard(key, nshards);

    // If needed, add this shard to set of participants and send BEGIN.
    if (participants.find(i) == participants.end()) {
        participants.insert(i);
    }

    // Send the GET operation to appropriate shard.
    Promise promise(GET_TIMEOUT);

    bclient[i]->Get(key, &promise);
    value = promise.GetValue();

    return promise.GetReply();
}

/* Sets the value corresponding to the supplied key. */
int
Client::Put(const string &key, const string &value)
{
    // Contact the appropriate shard to set the value.
    int i = key_to_shard(key, nshards);

    // If needed, add this shard to set of participants and send BEGIN.
    if (participants.find(i) == participants.end()) {
        participants.insert(i);
    }

    Promise promise(PUT_TIMEOUT);

    // Buffering, so no need to wait.
    bclient[i]->Put(key, value, &promise);
    return promise.GetReply();
}

int
Client::Prepare(uint64_t &ts)
{
    int status;

    // 1. Send commit-prepare to all shards.
    Debug("PREPARE Transaction");
    list<Promise *> promises;

    for (auto p : participants) {
        Debug("Sending prepare to shard [%d]", p);
        promises.push_back(new Promise(PREPARE_TIMEOUT));
        bclient[p]->Prepare(Timestamp(),promises.back());
    }

    // In the meantime ... go get a timestamp for OCC
    if (mode == MODE_OCC) {
        // Have to go to timestamp server
        unique_lock<mutex> lk(cv_m);

        Debug("Sending request to TimeStampServer");
        tss->Invoke("", bind(&Client::tssCallback, this,
                             placeholders::_1,
                             placeholders::_2));
        
        Debug("Waiting for TSS reply");
        cv.wait(lk);
        ts = stol(replica_reply, NULL, 10);
        Debug("TSS reply received: %lu", ts);
    }

    // 2. Wait for reply from all shards. (abort on timeout)
    Debug("Waiting for PREPARE replies");

    status = REPLY_OK;
    for (auto p : promises) {
        // If any shard returned false, abort the transaction.
        if (p->GetReply() != REPLY_OK) {
            if (status != REPLY_FAIL) {
                status = p->GetReply();
            }
        }
        // Also, find the max of all prepare timestamp returned.
        if (p->GetTimestamp().getTimestamp() > ts) {
            ts = p->GetTimestamp().getTimestamp();
        }
        delete p;
    }
    return status;
}

/* Attempts to commit the ongoing transaction. */
bool
Client::Commit()
{
    // Implementing 2 Phase Commit
    uint64_t ts = 0;
    int status;

    for (int i = 0; i < COMMIT_RETRIES; i++) {
        status = Prepare(ts);
        if (status == REPLY_OK || status == REPLY_FAIL) {
            break;
        }
    }

    if (status == REPLY_OK) {
        // For Spanner like systems, calculate timestamp.
        if (mode == MODE_SPAN_OCC || mode == MODE_SPAN_LOCK) {
            uint64_t now, err;
            struct timeval t1, t2;

            gettimeofday(&t1, NULL);
            timeServer.GetTimeAndError(now, err);

            if (now > ts) {
                ts = now;
            } else {
                uint64_t diff = ((ts >> 32) - (now >> 32))*1000000 +
                        ((ts & 0xffffffff) - (now & 0xffffffff));
                err += diff;
            }

            commit_sleep = (int)err;

            // how good are we at waking up on time?
            Debug("Commit wait sleep: %lu", err);
            if (err > 1000000)
                Warning("Sleeping for too long! %lu; now,ts: %lu,%lu", err, now, ts);
            if (err > 150) {
                usleep(err-150);
            }
            // fine grained busy-wait
            while (1) {
                gettimeofday(&t2, NULL);
                if ((t2.tv_sec-t1.tv_sec)*1000000 +
                    (t2.tv_usec-t1.tv_usec) > (int64_t)err) {
                    break;
                }
            }
        }

        // Send commits
        Debug("COMMIT Transaction at [%lu]", ts);

        for (auto p : participants) {
            Debug("Sending commit to shard [%d]", p);
            bclient[p]->Commit(ts);
        }
        return true;
    }

    // 4. If not, send abort to all shards.
    Abort();
    return false;
}

/* Aborts the ongoing transaction. */
void
Client::Abort()
{
    Debug("ABORT Transaction");
    for (auto p : participants) {
        bclient[p]->Abort();
    }
}

/* Return statistics of most recent transaction. */
vector<int>
Client::Stats()
{
    vector<int> v;
    return v;
}

/* Callback from a tss replica upon any request. */
void
Client::tssCallback(const string &request, const string &reply)
{
    lock_guard<mutex> lock(cv_m);
    Debug("Received TSS callback [%s]", reply.c_str());

    // Copy reply to "replica_reply".
    replica_reply = reply;
    
    // Wake up thread waiting for the reply.
    cv.notify_all();
}

} // namespace spanstore
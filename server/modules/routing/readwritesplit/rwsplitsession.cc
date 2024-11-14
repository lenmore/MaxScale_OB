/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 * Copyright (c) 2023 MariaDB plc, Finnish Branch
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2026-09-21
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "rwsplitsession.hh"

#include <mysqld_error.h>

#include <maxbase/format.hh>
#include <maxbase/pretty_print.hh>

using namespace maxscale;
using namespace std::chrono;
using mariadb::QueryClassifier;

namespace
{
void log_unexpected_response(MXS_SESSION* session, mxs::RWBackend* backend, const mxs::Reply& reply)
{
    MXB_ERROR("Unexpected response from '%s', closing session: %s",
              backend->name(), reply.describe().c_str());
    session->dump_statements();
    session->dump_session_log();
    session->kill();
    mxb_assert(!true);
}
}

std::vector<RWBackend*> sptr_vec_to_ptr_vec(std::vector<RWBackend>& sVec)
{
    std::vector<RWBackend*> pVec;
    for (auto& smart : sVec)
    {
        pVec.push_back(&smart);
    }
    return pVec;
}

RWSplitSession::RWSplitSession(RWSplit* instance, MXS_SESSION* session, mxs::RWBackends backends)
    : mxs::RouterSession(session)
    , m_backends(std::move(backends))
    , m_raw_backends(sptr_vec_to_ptr_vec(m_backends))
    , m_current_master(nullptr)
    , m_config(instance->config().get_ref())
    , m_expected_responses(0)
    , m_router(instance)
    , m_wait_gtid(NONE)
    , m_next_seq(0)
    , m_qc(parser(), this, session, m_config->use_sql_variables_in)
    , m_retry_duration(0)
    , m_can_replay_trx(true)
{
}

RWSplitSession* RWSplitSession::create(RWSplit* router, MXS_SESSION* session, const Endpoints& endpoints)
{
    RWSplitSession* rses = new RWSplitSession(router, session, RWBackend::from_endpoints(endpoints));

    if (rses->open_connections())
    {
        mxb::atomic::add(&router->stats().n_sessions, 1, mxb::atomic::RELAXED);
    }
    else
    {
        delete rses;
        rses = nullptr;
    }

    return rses;
}

RWSplitSession::~RWSplitSession()
{
    auto t = m_session_timer.split();

    for (auto& backend : m_raw_backends)
    {
        auto& stats = m_router->local_server_stats()[backend->target()];
        stats.update(t, backend->select_timer().total(), backend->num_selects());
    }

    // TODO: Fix this
    // m_router->local_avg_sescmd_sz().add(protocol_data().history().size());
}

bool RWSplitSession::routeQuery(GWBUF&& buffer)
{
    if (buffer.empty())
    {
        MXB_ERROR("MXS-2585: Null buffer passed to routeQuery, closing session");
        mxb_assert(!true);
        return 0;
    }

    if (replaying_trx() || !m_pending_retries.empty() || !m_query_queue.empty())
    {
        MXB_INFO("New %s received while %s is active: %s",
                 mariadb::cmd_to_string(buffer.data()[4]),
                 replaying_trx() ?  "transaction replay" : "query execution",
                 get_sql_string(buffer).c_str());

        m_query_queue.emplace_back(std::move(buffer));
        return true;
    }

    return route_query(std::move(buffer));
}

bool RWSplitSession::route_query(GWBUF&& buffer)
{
    bool rval = false;
    bool trx_was_ending = trx_is_ending();
    m_qc.update_route_info(buffer);
    RoutingPlan res = resolve_route(buffer, route_info());

    if (can_route_query(buffer, res, trx_was_ending))
    {
        if (need_gtid_probe(res))
        {
            m_qc.revert_update();
            m_query_queue.push_front(std::move(buffer));
            std::tie(buffer, res) = start_gtid_probe();
        }

        /** No active or pending queries */
        rval = route_stmt(std::move(buffer), res);
    }
    else
    {
        // Roll back the query classifier state to keep it consistent.
        m_qc.revert_update();

        // Already busy executing a query, put the query in a queue and route it later
        MXB_INFO("Storing query (len: %lu cmd: %0x), expecting %d replies to current command: %s. "
                 "Would route %s to '%s'.",
                 buffer.length(), buffer.data()[4], m_expected_responses,
                 maxbase::show_some(get_sql_string(buffer), 1024).c_str(),
                 route_target_to_string(res.route_target),
                 res.target ? res.target->name() : "<no target>");

        mxb_assert(m_expected_responses >= 1 || !m_query_queue.empty());

        m_query_queue.emplace_back(std::move(buffer));
        rval = true;
        mxb_assert(m_expected_responses >= 1);
    }

    return rval;
}

/**
 * @brief Route a stored query
 *
 * When multiple queries are executed in a pipeline fashion, the readwritesplit
 * stores the extra queries in a queue. This queue is emptied after reading a
 * reply from the backend server.
 *
 * @param rses Router client session
 * @return True if a stored query was routed successfully
 */
bool RWSplitSession::route_stored_query()
{
    if (m_query_queue.empty())
    {
        return true;
    }

    bool rval = true;

    /** Loop over the stored statements as long as the routeQuery call doesn't
     * append more data to the queue. If it appends data to the queue, we need
     * to wait for a response before attempting another reroute */
    MXB_INFO(">>> Routing stored queries");

    while (!m_query_queue.empty())
    {
        auto query = std::move(m_query_queue.front());
        m_query_queue.pop_front();
        mxb_assert_message(query, "Query in query queue unexpectedly empty");

        /** Store the query queue locally for the duration of the routeQuery call.
         * This prevents recursive calls into this function. */
        decltype(m_query_queue) temp_storage;
        temp_storage.swap(m_query_queue);

        if (!routeQuery(std::move(query)))
        {
            rval = false;
            MXB_ERROR("Failed to route queued query.");
        }

        if (m_query_queue.empty())
        {
            /** Query successfully routed and no responses are expected */
            m_query_queue.swap(temp_storage);
        }
        else
        {
            /**
             * Routing was stopped, we need to wait for a response before retrying.
             * temp_storage holds the tail end of the queue and m_query_queue contains the query we attempted
             * to route.
             */
            mxb_assert(m_query_queue.size() == 1);
            temp_storage.push_front(std::move(m_query_queue.front()));
            m_query_queue = std::move(temp_storage);
            break;
        }
    }

    MXB_INFO("<<< Stored queries routed");

    return rval;
}

void RWSplitSession::trx_replay_next_stmt()
{
    mxb_assert(m_state == TRX_REPLAY);

    if (m_replayed_trx.have_stmts())
    {
        const auto& curr_trx = m_trx.checksums();
        const auto& old_trx = m_replayed_trx.checksums();

        if (old_trx[curr_trx.size() - 1] == curr_trx.back())
        {
            // More statements to replay, pop the oldest one and execute it
            GWBUF buf = m_replayed_trx.pop_stmt();
            const char* cmd = mariadb::cmd_to_string(mxs_mysql_get_command(buf));
            MXB_INFO("Replaying %s: %s", cmd, get_sql_string(buf).c_str());
            retry_query(std::move(buf), 0);
        }
        else
        {
            checksum_mismatch();
        }
    }
    else
    {
        // No more statements to execute, return to normal routing mode
        m_state = ROUTING;
        mxb::atomic::add(&m_router->stats().n_trx_replay, 1, mxb::atomic::RELAXED);

        if (!m_replayed_trx.empty())
        {
            // Check that the checksums match.
            if (m_trx.checksums().back() == m_replayed_trx.checksums().back())
            {
                mxb_assert(m_trx.checksums() == m_replayed_trx.checksums());
                MXB_INFO("Checksums match, replay successful. Replay took %ld seconds.",
                         trx_replay_seconds());
                m_num_trx_replays = 0;

                if (m_interrupted_query)
                {
                    m_state = TRX_REPLAY_INTERRUPTED;
                    MXB_INFO("Resuming execution: %s", get_sql_string(m_interrupted_query.buffer).c_str());
                    retry_query(std::move(m_interrupted_query.buffer), 0);
                    m_interrupted_query.buffer.clear();
                }
                else if (!m_query_queue.empty())
                {
                    route_stored_query();
                }
            }
            else
            {
                checksum_mismatch();
            }
        }
        else
        {
            /**
             * The transaction was "empty". This means that the start of the transaction
             * did not finish before we started the replay process.
             *
             * The transaction that is being currently replayed has a result,
             * whereas the original interrupted transaction had none. Due to this,
             * the checksums would not match if they were to be compared.
             */
            mxb_assert_message(!m_interrupted_query, "Interrupted query should be empty");
            m_num_trx_replays = 0;
        }
    }
}

void RWSplitSession::checksum_mismatch()
{
    // Turn the replay flag back on to prevent queries from getting routed before the hangup we
    // just added is processed. For example, this can happen if the error is sent and the client
    // manages to send a COM_QUIT that gets processed before the fake hangup event.
    // This also makes it so that when transaction_replay_retry_on_mismatch is enabled, the replay
    // will eventually stop.
    m_state = TRX_REPLAY;

    if (m_config->trx_retry_on_mismatch && start_trx_replay())
    {
        MXB_INFO("Checksum mismatch, starting transaction replay again.");
    }
    else
    {
        MXB_INFO("Checksum mismatch, transaction replay failed. Closing connection.");
        m_pSession->kill("Transaction checksum mismatch encountered when replaying transaction.");
    }
}

void RWSplitSession::manage_transactions(RWBackend* backend, const GWBUF& writebuf, const mxs::Reply& reply)
{
    if (m_state == OTRX_ROLLBACK)
    {
        /** This is the response to the ROLLBACK. If it fails, we must close
         * the connection. The replaying of the transaction can continue
         * regardless of the ROLLBACK result. */
        mxb_assert(backend == m_prev_plan.target);

        if (!mxs_mysql_is_ok_packet(writebuf))
        {
            m_pSession->kill();
        }
    }
    else if (m_config->transaction_replay && m_can_replay_trx && trx_is_open())
    {
        if (m_wait_gtid != READING_GTID && m_wait_gtid != GTID_READ_DONE)
        {
            m_current_query.buffer.minimize();
            int64_t size = m_trx.size() + m_current_query.buffer.runtime_size();

            // A transaction is open and it is eligible for replaying
            if (size < m_config->trx_max_size)
            {
                /** Transaction size is OK, store the statement for replaying and
                 * update the checksum of the result */

                m_current_query.bytes += writebuf.length();
                m_current_query.checksum.update(writebuf);

                if (reply.is_complete())
                {
                    const char* cmd = mariadb::cmd_to_string(mxs_mysql_get_command(m_current_query.buffer));

                    // Add an empty checksum for any statements which we don't want to checksum. This allows
                    // us to identify which statement it was that caused the checksum mismatch.
                    if (!include_in_checksum(reply))
                    {
                        m_current_query.checksum.reset();
                    }

                    m_current_query.checksum.finalize();
                    m_trx.add_result(m_current_query.checksum.value());

                    MXB_INFO("Adding %s to trx: %s", cmd, get_sql_string(m_current_query.buffer).c_str());

                    // Add the statement to the transaction now that the result is complete.
                    m_trx.add_stmt(backend, std::move(m_current_query.buffer));
                    m_current_query.clear();
                }
            }
            else
            {
                // We leave the transaction open to retain the information where it was being executed. This
                // is needed in case the server where it's being executed on fails.
                MXB_INFO("Transaction is too big (%lu bytes), can't replay if it fails.", size);
                m_can_replay_trx = false;
                mxb::atomic::add(&m_router->stats().n_trx_too_big, 1, mxb::atomic::RELAXED);
            }
        }
    }
    else if (m_wait_gtid == RETRYING_ON_MASTER)
    {
        // We're retrying the query on the master and we need to keep the current query
    }
    else
    {
        /** Normal response, reset the currently active query. This is done before
         * the whole response is complete to prevent it from being retried
         * in case the connection breaks in the middle of a resultset. */
        m_current_query.clear();
    }
}

bool RWSplitSession::lagging_too_much(RWBackend* backend, int max_rlag) const
{
    // Use a hard-coded lower limit of 5 minutes. This should avoid dropping connections early for cases
    // where max_replication_lag is very low.
    return backend->target()->replication_lag() > std::max(max_rlag * 2, 60 * 5);
}

void RWSplitSession::close_stale_connections()
{
    auto current_rank = get_current_rank();
    int max_rlag = get_max_replication_lag();

    for (auto& backend : m_raw_backends)
    {
        if (backend->in_use())
        {
            auto server = backend->target();

            if (!server->is_usable())
            {
                MXB_INFO("Discarding connection to '%s', server in state: %s",
                         backend->name(), backend->target()->status_string().c_str());
                backend->close();
            }
            else if (server->rank() != current_rank)
            {
                MXB_INFO("Discarding connection to '%s': Server has rank %ld and current rank is %ld",
                         backend->name(), backend->target()->rank(), current_rank);
                backend->close();
            }
            else if (max_rlag != mxs::Target::RLAG_UNDEFINED && lagging_too_much(backend, max_rlag))
            {
                mxb_assert(server->replication_lag() != mxs::Target::RLAG_UNDEFINED);
                MXB_INFO("Discarding connection to '%s': Server is lagging behind by %ld seconds",
                         backend->name(), server->replication_lag());
                backend->close();
            }
        }
    }
}

bool is_wsrep_error(const mxs::Error& error)
{
    return error.code() == 1047 && error.sql_state() == "08S01"
           && error.message() == "WSREP has not yet prepared node for application use";
}

bool RWSplitSession::is_ignorable_error(RWBackend* backend, const mxs::Error& error) const
{
    if (m_config->trx_retry_on_deadlock && error.is_rollback())
    {
        // Rollback error and retrying on deadlocks is enabled
        MXB_INFO("Got transaction rollback error: [%s] %u %s",
                 error.sql_state().c_str(), error.code(), error.message().c_str());
        return true;
    }

    if (is_wsrep_error(error))
    {
        // WSREP error from Galera. This means that the server in question is not yet up and
        // is in the process of starting up. This is a transient error that can be ignored
        // and which should trigger a replay.
        MXB_INFO("Got WSREP error: [%s] %u %s",
                 error.sql_state().c_str(), error.code(), error.message().c_str());
        return true;
    }

    if (error.code() == ER_OPTION_PREVENTS_STATEMENT
        && backend == m_current_master      // This is the current master
        && trx_is_open()                    // There's an open transaction
        && !trx_is_read_only()              // The transaction isn't read-only
        && m_config->transaction_replay     // Transaction replay is enabled
        && m_state != TRX_REPLAY)           // Not replaying a transaction
    {
        // TODO: This could be handled inside a replayed transaction but the use of restart_trx_replay() is
        //       not totally safe inside clientReply.
        mxb_assert_message(error.message().find("--read-only") != std::string::npos,
                           "Expected --read-only in error: %s", error.message().c_str());

        // The query was routed to m_current_master while a transaction was open and transaction_replay is
        // enabled. In these situations, the most likely cause of this is that a switchover is taking place
        // and the server was set into read-only mode. To recover from a switchover gracefully, treat this as
        // an ignorable error that can trigger transaction replay.
        MXB_INFO("Got read-only error: [%s] %u %s",
                 error.sql_state().c_str(), error.code(), error.message().c_str());
        return true;
    }

    return false;
}

bool RWSplitSession::handle_ignorable_error(RWBackend* backend, const mxs::Error& error)
{
    mxb_assert(m_expected_responses >= 1);

    bool ok = false;

    MXB_INFO("%s: %s", error.is_rollback() ?
             "Server triggered transaction rollback, replaying transaction" :
             "WSREP not ready, retrying query", error.message().c_str());

    if (trx_is_open())
    {
        ok = start_trx_replay();
    }
    else
    {
        static bool warn_unexpected_rollback = true;

        if (!is_wsrep_error(error) && warn_unexpected_rollback)
        {
            MXB_WARNING("Expected a WSREP error but got a transaction rollback error: %d, %s",
                        error.code(), error.message().c_str());
            warn_unexpected_rollback = false;
        }

        if (m_expected_responses > 1)
        {
            MXB_INFO("Cannot retry the query as multiple queries were in progress");
        }
        else if (!m_current_query)
        {
            MXB_INFO("Cannot retry, reply has been partially delivered to the client.");
        }
        else if (backend == m_current_master)
        {
            if (can_retry_query() && can_recover_master())
            {
                ok = retry_master_query(backend);
            }
        }
        else if (m_config->retry_failed_reads)
        {
            ok = true;
            retry_query(std::move(m_current_query.buffer));
            m_current_query.clear();
        }
    }

    if (ok)
    {
        backend->ack_write();
        m_expected_responses--;
        m_wait_gtid = NONE;
        m_pSession->reset_server_bookkeeping();
        backend->close();
    }

    return ok;
}

void RWSplitSession::finish_transaction(mxs::RWBackend* backend)
{
    // m_trx.target() can be null if the client sends two COMMIT statements in a row. Although unlikely to
    // appear on purpose, we cannot assert this until the transaction state is tracked at the component level
    // in the routing chain.
    MXB_INFO("Transaction complete on '%s', %s of SQL.",
             m_trx.target() ? m_trx.target()->name() : "<no target>",
             mxb::pretty_size(m_trx.size()).c_str());
    m_trx.close();
    m_can_replay_trx = true;
    m_set_trx.clear();
}

bool RWSplitSession::discard_partial_result(GWBUF& buffer, const mxs::Reply& reply)
{
    mxb_assert(m_interrupted_query.bytes >= m_current_query.bytes);
    mxb_assert(m_config->transaction_replay);
    bool discard = m_current_query.bytes + buffer.length() <= m_interrupted_query.bytes;

    if (discard)
    {
        // Discard this part, we have already sent it.
        m_current_query.bytes += buffer.length();
        m_current_query.checksum.update(buffer);
        MXB_INFO("Discarding result, client already has it. %s processed so far.",
                 mxb::pretty_size(m_current_query.bytes).c_str());

        if (reply.is_complete())
        {
            MXB_INFO("Replayed result was shorter than the original one.");
            checksum_mismatch();
        }
    }
    else
    {
        // We've returned some part of this result. Split it into two parts and return the trailing end of the
        // result to the client.
        MXB_INFO("Replay of interrupted query is complete.");
        auto bytes_to_discard = m_interrupted_query.bytes - m_current_query.bytes;
        m_current_query.checksum.update(buffer.data(), bytes_to_discard);
        buffer.consume(bytes_to_discard);
        m_current_query.bytes = m_interrupted_query.bytes;
        m_state = ROUTING;
        m_num_trx_replays = 0;

        if (include_in_checksum(reply))
        {
            auto cksum = m_current_query.checksum;
            cksum.finalize();

            // In case the result wasn't the same, the resultset checksum will not match.
            if (cksum != m_interrupted_query.checksum)
            {
                checksum_mismatch();
                discard = true;
            }
        }

        m_interrupted_query.clear();
    }

    return discard;
}

bool RWSplitSession::clientReply(GWBUF&& writebuf, const mxs::ReplyRoute& down, const mxs::Reply& reply)
{
    RWBackend* backend = static_cast<RWBackend*>(down.endpoint()->get_userdata());

    if (backend->should_ignore_response())
    {
        return ignore_response(backend, reply);
    }

    if (handle_causal_read_reply(writebuf, reply, backend))
    {
        return 1;   // Nothing to route, return
    }

    if (m_state == TRX_REPLAY_INTERRUPTED && discard_partial_result(writebuf, reply))
    {
        return true;    // Discard this chunk, the client already has it
    }

    const auto& error = reply.error();

    if (error.is_unexpected_error())
    {
        // All unexpected errors are related to server shutdown.
        MXB_SINFO("Server '" << backend->name() << "' is shutting down");

        // The server sent an error that we either didn't expect or we don't want. If retrying is going to
        // take place, it'll be done in handleError.
        if (!backend->is_waiting_result() || !reply.has_started())
        {
            // The buffer contains either an ERR packet, in which case the resultset hasn't started yet, or a
            // resultset with a trailing ERR packet. The full resultset can be discarded as the client hasn't
            // received it yet. In theory we could return this to the client but we don't know if it was
            // interrupted or not so the safer option is to retry it.
            return 1;
        }
    }

    if (is_ignorable_error(backend, error) && handle_ignorable_error(backend, error))
    {
        // We can ignore this error and treat it as if the connection to the server was broken.
        return 1;
    }

    if (m_wait_gtid != GTID_READ_DONE)
    {
        m_qc.update_from_reply(reply);
    }

    // TODO: Do this in the client protocol, it seems to be a pretty logical place for it as it already
    // assigns the prepared statement IDs.
    if (m_config->reuse_ps && reply.command() == MXS_COM_STMT_PREPARE)
    {
        if (m_current_query)
        {
            const auto& current_sql = get_sql_string(m_current_query.buffer);
            m_ps_cache[current_sql].append(writebuf.shallow_clone());
        }
    }

    // Track transaction contents and handle ROLLBACK with aggressive transaction load balancing
    manage_transactions(backend, writebuf, reply);

    if (reply.is_complete())
    {
        if (backend->is_idle())
        {
            log_unexpected_response(m_pSession, backend, reply);
            return false;
        }

        MXB_INFO("Reply complete from '%s' (%s)", backend->name(), reply.describe().c_str());
        /** Got a complete reply, decrement expected response count */
        m_expected_responses--;
        mxb_assert(m_expected_responses >= 0);

        track_tx_isolation(reply);

        if (reply.command() == MXS_COM_STMT_PREPARE && reply.is_ok())
        {
            m_qc.ps_store_response(reply.generated_id(), reply.param_count());
        }

        if (m_state == OTRX_ROLLBACK)
        {
            // Transaction rolled back, start replaying it on the master
            m_state = ROUTING;
            start_trx_replay();
            m_pSession->reset_server_bookkeeping();
            return 1;
        }

        backend->ack_write();
        backend->select_finished();
        mxb_assert(m_expected_responses >= 0);

        if (continue_causal_read())
        {
            // GTID sync part of causal reads is complete, continue with the actual reading part. This must be
            // done after the ack_write() call to make sure things are correctly marked as done. It must also
            // be done only if we didn't ignore a response: there can be multiple pending queries ongoing
            // during the GTID sync and only the response which isn't discarded is the correct one.
            return 1;
        }
    }
    else
    {
        MXB_INFO("Reply not yet complete. Waiting for %d replies, got one from %s",
                 m_expected_responses, backend->name());
    }

    mxb_assert(writebuf);

    if (m_state == TRX_REPLAY)
    {
        mxb_assert(m_config->transaction_replay);

        if (m_expected_responses == 0)
        {
            // Current statement is complete, continue with the next one
            trx_replay_next_stmt();
        }

        /**
         * If the start of the transaction was interrupted, we need to return
         * the result to the client.
         *
         * This retrying of START TRANSACTION is done with the transaction replay
         * mechanism instead of the normal query retry mechanism because the safeguards
         * in the routing logic prevent retrying of individual queries inside transactions.
         *
         * If the transaction was not empty and some results have already been
         * sent to the client, we must discard all responses that the client already has.
         */

        if (!m_replayed_trx.empty())
        {
            // Client already has this response, discard it
            return 1;
        }
    }
    else if (trx_is_open() && trx_is_ending() && m_expected_responses == 0)
    {
        finish_transaction(backend);
    }

    mxb_assert_message(backend->in_use(), "Backend should be in use when routing reply");
    /** Write reply to client DCB */
    auto rc = RouterSession::clientReply(std::move(writebuf), down, reply);

    if (reply.is_complete() && m_expected_responses == 0 && m_state != TRX_REPLAY)
    {
        route_stored_query();
    }

    if (m_check_stale && m_expected_responses == 0 && !trx_is_open())
    {
        /**
         * Close stale connections to servers in maintenance. Done here to avoid closing the connections
         * before all responses have been received. Must not be done inside a transaction.
         */
        close_stale_connections();
        m_check_stale = false;
    }

    return rc;
}

bool RWSplitSession::ignore_response(mxs::RWBackend* backend, const mxs::Reply& reply)
{
    if (reply.is_complete())
    {
        if (backend->is_idle())
        {
            log_unexpected_response(m_pSession, backend, reply);
            return false;
        }

        backend->ack_write();
        backend->select_finished();
        mxb_assert(m_expected_responses >= 0);

        MXB_INFO("Reply complete from '%s', discarding it: %s", backend->name(), reply.describe().c_str());
    }
    else
    {
        MXB_INFO("Reply not yet complete from '%s', discarding partial result.", backend->name());
    }

    return true;
}

bool RWSplitSession::can_start_trx_replay() const
{
    bool can_replay = false;

    if (m_can_replay_trx)
    {
        if (m_config->trx_timeout > 0s)
        {
            // m_trx_replay_timer is only set when the first replay starts, this is why we must check how many
            // attempts we've made.
            if (m_num_trx_replays == 0 || m_trx_replay_timer.split() < m_config->trx_timeout)
            {
                can_replay = true;
            }
            else
            {
                MXB_INFO("Transaction replay time limit of %ld seconds exceeded, not attempting replay",
                         m_config->trx_timeout.count());
            }
        }
        else
        {
            if (m_num_trx_replays < m_config->trx_max_attempts)
            {
                can_replay = true;
            }
            else
            {
                mxb_assert(m_num_trx_replays == m_config->trx_max_attempts);
                MXB_INFO("Transaction replay attempt cap of %ld exceeded, not attempting replay",
                         m_config->trx_max_attempts);
            }
        }
    }

    return can_replay;
}

bool RWSplitSession::start_trx_replay()
{
    bool rval = false;

    if (m_config->transaction_replay && can_start_trx_replay())
    {
        ++m_num_trx_replays;

        if (!replaying_trx())
        {
            // This is the first time we're retrying this transaction, store it and the interrupted query
            m_orig_trx = m_trx;
            m_orig_stmt = m_current_query.shallow_clone();
            m_trx_replay_timer.restart();
        }
        else
        {
            // If there are pending retries while the state is TRX_REPLAY, the transaction replay
            // was started again before the previous queries were routed. In this case the currently
            // queued up delay_routing() calls would have to be canceled but this is not currently
            // possible. As a workaround, a second counter of "discarded" queries must be used to
            // indicate the number of queries to discard. This effectively cancels out the pending
            // delay_routing() calls.
            mxb_assert(m_canceled_retries <= m_pending_retries.size());
            m_canceled_retries = m_pending_retries.size();

            // Not the first time, copy the original
            m_replayed_trx.close();
            m_trx.close();
            m_trx = m_orig_trx;
            m_current_query = m_orig_stmt.shallow_clone();
        }

        if (m_trx.have_stmts() || m_current_query)
        {
            // Stash any interrupted queries while we replay the transaction
            m_interrupted_query = std::move(m_current_query);
            m_interrupted_query.checksum.finalize();
            m_current_query.clear();

            MXB_INFO("Starting transaction replay %ld. Replay has been ongoing for %ld seconds.",
                     m_num_trx_replays, trx_replay_seconds());
            m_state = TRX_REPLAY;

            /**
             * Copy the transaction for replaying. The current transaction
             * is closed as the replaying opens a new transaction.
             */
            m_replayed_trx = m_trx;
            m_trx.close();

            if (m_replayed_trx.have_stmts())
            {
                // Pop the first statement and start replaying the transaction
                GWBUF buf = m_replayed_trx.pop_stmt();
                const char* cmd = mariadb::cmd_to_string(mxs_mysql_get_command(buf));
                MXB_INFO("Replaying %s: %s", cmd, get_sql_string(buf).c_str());
                retry_query(std::move(buf), 1);
            }
            else
            {
                /**
                 * The transaction was only opened and no queries have been
                 * executed. The buffer should contain a query that starts
                 * or ends a transaction or autocommit should be disabled.
                 */
                MXB_AT_DEBUG(uint32_t type_mask = parser().get_trx_type_mask(m_interrupted_query.buffer));
                mxb_assert_message((type_mask & (sql::TYPE_BEGIN_TRX | sql::TYPE_COMMIT))
                                   || !route_info().trx().is_autocommit(),
                                   "The current query (%s) should start or stop a transaction "
                                   "or autocommit should be disabled",
                                   get_sql_string(m_interrupted_query.buffer).c_str());

                m_state = TRX_REPLAY_INTERRUPTED;
                MXB_INFO("Retrying interrupted query: %s",
                         get_sql_string(m_interrupted_query.buffer).c_str());
                retry_query(std::move(m_interrupted_query.buffer), 1);
                m_interrupted_query.buffer.clear();
            }
        }
        else
        {
            mxb_assert_message(route_info().trx().is_autocommit() || trx_is_ending(),
                               "Session should have autocommit disabled or transaction just ended if the "
                               "transaction had no statements and no query was interrupted");
        }

        rval = true;
    }

    return rval;
}

bool RWSplitSession::retry_master_query(RWBackend* backend)
{
    bool can_continue = false;

    if (m_current_query)
    {
        // A query was in progress, try to route it again
        mxb_assert(m_prev_plan.target == backend || m_prev_plan.route_target == TARGET_ALL);
        retry_query(std::move(m_current_query.buffer));
        m_current_query.clear();
        can_continue = true;
    }
    else
    {
        // This should never happen
        mxb_assert_message(!true, "m_current_query is empty");
        MXB_ERROR("Current query unexpectedly empty when trying to retry query on primary");
    }

    return can_continue;
}

bool RWSplitSession::handleError(mxs::ErrorType type, const std::string& message,
                                 mxs::Endpoint* endpoint, const mxs::Reply& reply)
{
    RWBackend* backend = static_cast<RWBackend*>(endpoint->get_userdata());
    mxb_assert(backend && backend->in_use());
    std::string errmsg;
    bool is_expected = backend->is_expected_response();

    if (is_expected && route_info().multi_part_packet())
    {
        errmsg = mxb::string_printf("Server '%s' was lost in the middle of a large multi-packet query,"
                                    " cannot continue the session: %s", backend->name(), message.c_str());
        return mxs::RouterSession::handleError(type, errmsg, endpoint, reply);
    }
    else if (is_expected && reply.has_started() && (!m_config->transaction_replay || !trx_is_open()))
    {
        errmsg = mxb::string_printf("Server '%s' was lost in the middle of a resultset,"
                                    " cannot continue the session: %s", backend->name(), message.c_str());
        return mxs::RouterSession::handleError(type, errmsg, endpoint, reply);
    }
    else if (m_pSession->killed_by_query())
    {
        errmsg = "Connection was killed by a KILL query, closing session: " + message;
        return mxs::RouterSession::handleError(type, errmsg, endpoint, reply);
    }

    auto failure_type = type == mxs::ErrorType::PERMANENT ? RWBackend::CLOSE_FATAL : RWBackend::CLOSE_NORMAL;

    bool can_continue = false;

    if (m_current_master && m_current_master->in_use() && m_current_master == backend)
    {
        MXB_INFO("Primary '%s' failed: %s", backend->name(), message.c_str());
        /** The connection to the master has failed */

        if (mxs_mysql_is_binlog_dump(reply.command()) || reply.command() == MXS_COM_REGISTER_SLAVE)
        {
            MXB_INFO("Session is a replication client, closing connection immediately.");
            m_pSession->kill();     // Not sending an error causes the replication client to connect again
            return false;
        }

        auto old_wait_gtid = m_wait_gtid;
        bool expected_response = backend->is_waiting_result();

        if (!expected_response)
        {
            // We have to use Backend::is_waiting_result as the check since it's updated immediately after a
            // write to the backend is done. The mxs::Reply is updated only when the backend protocol
            // processes the query which can be out of sync when handleError is called if the disconnection
            // happens before authentication completes.
            mxb_assert(reply.is_complete() || backend->should_ignore_response());

            /** The failure of a master is not considered a critical
             * failure as partial functionality still remains. If
             * master_failure_mode is not set to fail_instantly, reads
             * are allowed as long as slave servers are available
             * and writes will cause an error to be returned.
             *
             * If we were waiting for a response from the master, we
             * can't be sure whether it was executed or not. In this
             * case the safest thing to do is to close the client
             * connection. */
            errmsg += " Lost connection to primary server while connection was idle.";
            if (m_config->master_failure_mode != RW_FAIL_INSTANTLY)
            {
                can_continue = true;
            }
        }
        else
        {
            // We were expecting a response but we aren't going to get one
            mxb_assert(m_expected_responses >= 1);

            errmsg += " Lost connection to primary server while waiting for a result.";

            if (m_expected_responses > 1)
            {
                can_continue = false;
                errmsg += " Cannot retry query as multiple queries were in progress.";
            }
            else if (m_wait_gtid == READING_GTID)
            {
                m_current_query.buffer = reset_gtid_probe();

                if (!trx_is_open() && can_recover_master())
                {
                    // Not inside a transaction, we can retry the original query
                    retry_query(std::move(m_current_query.buffer), 0);
                    m_current_query.clear();
                    can_continue = true;
                }
            }
            else if (m_config->retry_failed_reads && m_prev_plan.route_target != TARGET_MASTER
                     && !trx_is_open() && can_recover_master())
            {
                // This was not a write but it just ended up being routed to the current master. It can be
                // safely retried if a transaction is not open.
                can_continue = retry_master_query(backend);
            }
            else if (m_config->master_failure_mode == RW_ERROR_ON_WRITE)
            {
                /** In error_on_write mode, the session can continue even
                 * if the master is lost. Send a read-only error to
                 * the client to let it know that the query failed. */
                can_continue = true;
                send_readonly_error();
            }
        }

        if (trx_is_open() && !in_optimistic_trx()
            && (!m_trx.target() || m_trx.target() == backend || old_wait_gtid == READING_GTID))
        {
            can_continue = start_trx_replay();
            errmsg += " A transaction is active and cannot be replayed.";
        }

        if (m_qc.have_tmp_tables())
        {
            if (m_config->strict_tmp_tables)
            {
                can_continue = false;
                errmsg += " Temporary tables were lost when the connection was lost.";
            }
            else
            {
                MXB_INFO("Temporary tables have been created and they "
                         "are now lost if a reconnection takes place.");
            }
        }

        if (!m_unsafe_reconnect_reason.empty())
        {
            can_continue = false;
            errmsg += " Unsafe to reconnect: " + m_unsafe_reconnect_reason + ".";
        }

        if (!can_continue)
        {
            auto diff = maxbase::Clock::now(maxbase::NowType::EPollTick) - backend->last_write();
            int idle = duration_cast<seconds>(diff).count();
            errmsg = mxb::string_printf(
                "Lost connection to the primary server, closing session.%s "
                "Connection from %s has been idle for %d seconds. Error caused by: %s. "
                "Last error: %s", errmsg.c_str(), m_pSession->user_and_host().c_str(),
                idle, message.c_str(), reply.error().message().c_str());
        }

        // Decrement the expected response count only if we know we can continue the sesssion.
        // This keeps the internal logic sound even if another query is routed before the session
        // is closed.
        if (can_continue && expected_response)
        {
            m_expected_responses--;
        }

        backend->close(failure_type);
        MXB_SINFO("Primary connection failed: " << message);
    }
    else
    {
        MXB_INFO("Replica '%s' failed: %s", backend->name(), message.c_str());

        if (backend->is_waiting_result())
        {
            // Slaves should never have more than one response waiting
            mxb_assert(m_expected_responses == 1);
            m_expected_responses--;

            mxb_assert_message(m_wait_gtid != READING_GTID, "Should not be in READING_GTID state");
            // Reset causal read state so that the next read starts from the correct one.
            m_wait_gtid = NONE;
        }

        // If a GTID probe is ongoing and the target of the transaction failed, the replay cannot be started
        // until the GTID probe either ends or the current master server fails at which point the replay will
        // be started.
        if (trx_is_read_only() && m_trx.target() == backend && m_wait_gtid != READING_GTID)
        {
            // Try to replay the transaction on another node
            can_continue = start_trx_replay();
            backend->close(failure_type);
            MXB_SINFO("Read-only trx failed: " << message);

            if (!can_continue)
            {
                errmsg = mxb::string_printf("Connection to server %s failed while executing "
                                            "a read-only transaction", backend->name());
            }
        }
        else if (in_optimistic_trx())
        {
            /**
             * The connection was closed mid-transaction or while we were
             * executing the ROLLBACK. In both cases the transaction will
             * be closed. We can safely start retrying the transaction
             * on the master.
             */

            mxb_assert(trx_is_open());
            can_continue = start_trx_replay();
            backend->close(failure_type);
            MXB_SINFO("Optimistic trx failed: " << message);
        }
        else
        {
            can_continue = handle_error_new_connection(backend, message, failure_type);

            if (!can_continue)
            {
                errmsg = mxb::string_printf("Unable to continue session as all connections have failed and "
                                            "new connections cannot be created. Last server to fail was '%s'.",
                                            backend->name());
            }
        }
    }

    mxb_assert_message(can_continue || !errmsg.empty(), "We should always return a custom error");

    return can_continue || mxs::RouterSession::handleError(
        type, errmsg.empty() ? message : errmsg, endpoint, reply);
}

void RWSplitSession::endpointConnReleased(mxs::Endpoint* down)
{
    auto* backend = static_cast<RWBackend*>(down->get_userdata());
    if (can_recover_servers() && (!backend->is_master() || m_config->master_reconnection))
    {
        backend->close(RWBackend::CLOSE_NORMAL);
        MXB_INFO("Backend pooled");
    }
}

/**
 * Check if there is backend reference pointing at failed DCB, and reset its
 * flags. Then clear DCB's callback and finally : try to find replacement(s)
 * for failed slave(s).
 *
 * This must be called with router lock.
 *
 * @param inst      router instance
 * @param rses      router client session
 * @param dcb       failed DCB
 * @param errmsg    error message which is sent to client if it is waiting
 *
 * @return true if there are enough backend connections to continue, false if
 * not
 */
bool RWSplitSession::handle_error_new_connection(RWBackend* backend, const std::string& errmsg,
                                                 RWBackend::close_type failure_type)
{
    bool route_stored = false;
    bool can_be_fixed = true;

    if (backend->is_waiting_result())
    {
        // The backend was busy executing command and the client is expecting a response.
        if (m_current_query && m_config->retry_failed_reads)
        {
            if (!m_config->delayed_retry && is_last_backend(backend))
            {
                can_be_fixed = false;
                MXB_INFO("Cannot retry failed read as there are no candidates to "
                         "try it on and delayed_retry is not enabled");
            }
            else
            {

                MXB_INFO("Re-routing failed read after server '%s' failed", backend->name());
                route_stored = false;
                retry_query(std::move(m_current_query.buffer));
                m_current_query.clear();
            }
        }
        else
        {
            can_be_fixed = false;
        }
    }

    /** Close the current connection. This needs to be done before routing any
     * of the stored queries. If we route a stored query before the connection
     * is closed, it's possible that the routing logic will pick the failed
     * server as the target. */
    backend->close(failure_type);
    MXB_SINFO("Replica connection failed: " << errmsg);

    if (can_be_fixed && route_stored)
    {
        route_stored_query();
    }

    return can_be_fixed && (can_recover_servers() || have_open_connections());
}

bool RWSplitSession::lock_to_master()
{
    if (m_config->strict_multi_stmt || m_config->strict_sp_calls)
    {
        MXB_INFO("Multi-statement query or stored procedure call, routing "
                 "all future queries to primary.");
        m_locked_to_master = true;
    }

    return m_current_master && m_current_master->in_use();
}

bool RWSplitSession::is_locked_to_master() const
{
    return m_locked_to_master || m_set_trx;
}

bool RWSplitSession::supports_hint(Hint::Type hint_type) const
{
    using Type = Hint::Type;
    bool rv = true;

    switch (hint_type)
    {
    case Type::ROUTE_TO_MASTER:
    case Type::ROUTE_TO_SLAVE:
    case Type::ROUTE_TO_NAMED_SERVER:
    case Type::ROUTE_TO_LAST_USED:
    case Type::PARAMETER:
        // Ignore hints inside transactions if transaction replay or causal reads is enabled. This prevents
        // all sorts of problems (e.g. MXS-4260) that happen when the contents of the transaction are spread
        // across multiple servers.
        if (trx_is_open() && (m_config->transaction_replay || m_config->causal_reads != CausalReads::NONE))
        {
            rv = false;
        }
        break;

    case Type::ROUTE_TO_UPTODATE_SERVER:
    case Type::ROUTE_TO_ALL:
        rv = false;
        break;

    default:
        mxb_assert(!true);
        rv = false;
    }

    return rv;
}

void RWSplitSession::unsafe_to_reconnect(std::string_view why)
{
    m_unsafe_reconnect_reason.assign(why);
    MXB_INFO("Unsafe SQL (%s), disabling reconnection.",
             m_unsafe_reconnect_reason.c_str());
}

bool RWSplitSession::is_valid_for_master(const mxs::RWBackend* master)
{
    bool rval = false;

    if (master->in_use()
        || (m_config->master_reconnection && master->can_connect() && can_recover_servers()))
    {
        rval = master->target()->is_master()
            || (master->in_use() && master->target()->is_in_maint() && trx_is_open());
    }

    return rval;
}

bool RWSplitSession::need_gtid_probe(const RoutingPlan& plan) const
{
    uint8_t cmd = route_info().command();
    const auto cr = m_config->causal_reads;
    return (cr == CausalReads::UNIVERSAL || cr == CausalReads::FAST_UNIVERSAL)
           && plan.route_target == TARGET_SLAVE
           && m_wait_gtid == NONE
           && (cmd == MXS_COM_QUERY || cmd == MXS_COM_STMT_EXECUTE)
           && (route_info().type_mask() & (sql::TYPE_COMMIT | sql::TYPE_ROLLBACK)) == 0;
}

void RWSplitSession::track_tx_isolation(const mxs::Reply& reply)
{
    constexpr const char* LEVEL = "SERIALIZABLE";
    bool was_serializable = m_locked_to_master;
    std::string_view value;

    if (auto trx_char = reply.get_variable("trx_characteristics"); !trx_char.empty())
    {
        m_locked_to_master = trx_char.find(LEVEL) != std::string_view::npos;
        value = trx_char;
    }

    auto tx_isolation = reply.get_variable("transaction_isolation");

    if (tx_isolation.empty())
    {
        tx_isolation = reply.get_variable("tx_isolation");
    }

    if (!tx_isolation.empty())
    {
        m_locked_to_master = tx_isolation.find(LEVEL) != std::string_view::npos;
        value = tx_isolation;
    }

    if (was_serializable != m_locked_to_master)
    {
        MXB_INFO("Transaction isolation level set to '%s', %s", std::string(value).c_str(),
                 m_locked_to_master ? "locking session to primary" : "returning to normal routing");
    }
}

std::string RWSplitSession::get_delayed_retry_failure_reason() const
{
    std::string extra;
    auto backends = m_raw_backends;

    auto end = std::partition(backends.begin(), backends.end(), [](const auto* b){
        return b->is_master();
    });

    bool only_failed_masters = std::all_of(backends.begin(), end, [](const auto* b){
        return b->has_failed();
    });

    if (only_failed_masters)
    {
        extra = ". Found servers with the 'Master' status but the connections "
                "have been marked as broken due to fatal errors";
    }

    return "'delayed_retry_timeout' exceeded before a server with the 'Master' status could be found"
           + extra;
}

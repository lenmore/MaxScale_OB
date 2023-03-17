/*
 * Copyright (c) 2023 MariaDB plc
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2027-02-21
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "pgprotocolmodule.hh"
#include "pgauthenticatormodule.hh"
#include "pgclientconnection.hh"
#include "pgbackendconnection.hh"
#include "pgprotocoldata.hh"
#include "postgresprotocol.hh"
#include "pgusermanager.hh"

#include <maxscale/listener.hh>
#include <maxbase/pretty_print.hh>

PgProtocolModule::PgProtocolModule(std::string name, SERVICE* pService)
    : m_config(name, this)
    , m_service(*pService)
{
}

// static
PgProtocolModule* PgProtocolModule::create(const std::string& name, mxs::Listener* pListener)
{
    return new PgProtocolModule(name, pListener->service());
}

std::unique_ptr<mxs::ClientConnection>
PgProtocolModule::create_client_protocol(MXS_SESSION* pSession, mxs::Component* pComponent)
{
    auto sProtocol_data = std::make_unique<PgProtocolData>();

    pSession->set_protocol_data(std::move(sProtocol_data));

    auto sClient_connection = std::make_unique<PgClientConnection>(pSession, pComponent);

    return sClient_connection;
}

std::unique_ptr<mxs::BackendConnection>
PgProtocolModule::create_backend_protocol(MXS_SESSION* session, SERVER* server, mxs::Component* component)
{
    return std::make_unique<PgBackendConnection>(session, server, component);
}

std::string PgProtocolModule::auth_default() const
{
    MXB_ALERT("Not implemented yet: %s", __func__);
    mxb_assert(!true);

    return "";
}

GWBUF PgProtocolModule::make_error(int errnum, const std::string& sqlstate, const std::string& msg) const
{
    // The field type explanations are here
    // https://www.postgresql.org/docs/current/protocol-error-fields.html
    auto old_severity = mxb::cat("S", "ERROR");
    auto new_severity = mxb::cat("V", "ERROR");
    auto code = mxb::cat("C", sqlstate);
    auto message = mxb::cat("M", msg);

    GWBUF buf{pg::HEADER_LEN
              + old_severity.size() + 1
              + new_severity.size() + 1
              + code.size() + 1
              + message.size() + 1};
    auto ptr = buf.data();

    *ptr++ = 'E';
    ptr += pg::set_uint32(ptr, buf.length() - 1);
    ptr += pg::set_string(ptr, old_severity);
    ptr += pg::set_string(ptr, new_severity);
    ptr += pg::set_string(ptr, code);
    ptr += pg::set_string(ptr, message);

    return buf;
}

std::string_view PgProtocolModule::get_sql(const GWBUF& packet) const
{
    return pg::get_sql(packet);
}

std::string PgProtocolModule::describe(const GWBUF& packet, int max_len) const
{
    std::ostringstream ss;
    const uint8_t* ptr = packet.data();

    char cmd = *ptr++;
    uint32_t len = pg::get_uint32(ptr);
    ptr += 4;
    ss << pg::client_command_to_str(cmd) << " (" << mxb::pretty_size(len) << ")";

    switch (cmd)
    {
    case pg::QUERY:
        ss << " stmt: " << pg::get_string(ptr).substr(0, max_len);
        break;

    case pg::PARSE:
        {
            auto id = pg::get_string(ptr);
            ptr += id.size() + 1;
            ss << " id: '" << id << "' stmt: " << pg::get_string(ptr).substr(0, max_len);
        }
        break;

    case pg::CLOSE:
    case pg::DESCRIBE:
        {
            char type = *ptr++;
            ss << " type: '" << type << "' id: '" << pg::get_string(ptr) << "'";
        }
        break;

    case pg::EXECUTE:
        ss << " id: '" << pg::get_string(ptr) << "'";
        break;

    case pg::BIND:
        {
            auto portal = pg::get_string(ptr);
            ptr += portal.size() + 1;
            ss << " portal: '" << portal << "' id: '" << pg::get_string(ptr) << "'";
        }
        break;

    default:
        break;
    }

    return ss.str();
}

GWBUF PgProtocolModule::make_query(std::string_view sql) const
{
    return pg::create_query_packet(sql);
}

uint64_t PgProtocolModule::capabilities() const
{
    return mxs::ProtocolModule::CAP_BACKEND | mxs::ProtocolModule::CAP_AUTHDATA;
}

std::string PgProtocolModule::name() const
{
    return MXB_MODULE_NAME;
}

std::string PgProtocolModule::protocol_name() const
{
    return MXS_POSTGRESQL_PROTOCOL_NAME;
}

std::unique_ptr<mxs::UserAccountManager> PgProtocolModule::create_user_data_manager()
{
    return std::make_unique<PgUserManager>();
}

PgProtocolModule::AuthenticatorList
PgProtocolModule::create_authenticators(const mxs::ConfigParameters& params)
{
    MXB_ALERT("Not implemented yet: %s", __func__);

    AuthenticatorList authenticators;

    std::unique_ptr<PgAuthenticatorModule> sAuthenticator(new PgAuthenticatorModule);

    authenticators.push_back(std::move(sAuthenticator));

    return authenticators;
}

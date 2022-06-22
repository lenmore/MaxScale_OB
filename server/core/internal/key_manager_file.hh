/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2026-05-03
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#pragma once

#include <maxscale/ccdefs.hh>
#include <maxscale/key_manager.hh>
#include <maxscale/config2.hh>

namespace cfg
{
using Opt = mxs::config::ParamPath::Options;

static mxs::config::Specification spec {"key_manager_file", mxs::config::Specification::GLOBAL};
static mxs::config::ParamPath keyfile {&cfg::spec, "keyfile", "Path to the encryption key", Opt::R};
}

class FileKey : public mxs::KeyManager::MasterKey
{
public:
    static std::unique_ptr<mxs::KeyManager::MasterKey> create(const mxs::ConfigParameters& options)
    {
        Config config;
        std::unique_ptr<mxs::KeyManager::MasterKey> rv;

        if (config.specification().validate(options) && config.configure(options))
        {
            if (auto key = load_key_file(config); !key.empty())
            {
                rv.reset(new FileKey(std::move(key)));
            }
        }

        return rv;
    }

    std::tuple<bool, uint32_t, std::vector<uint8_t>>
    get_key(const std::string& id, uint32_t version) const override final
    {
        std::vector<uint8_t> key;
        auto it = m_keys.find(id);

        if (it != m_keys.end())
        {
            key = it->second;
        }

        return {it != m_keys.end(), MasterKey::NO_VERSIONING, key};
    }

private:

    class Config : public mxs::config::Configuration
    {
    public:
        Config()
            : mxs::config::Configuration(cfg::spec.module(), &cfg::spec)
        {
            add_native(&Config::keyfile, &cfg::keyfile);
        }

        std::string keyfile;
        int64_t     id;
    };

    FileKey(std::map<std::string, std::vector<uint8_t>> keys)
        : m_keys(std::move(keys))
    {
    }

    static bool is_hex_key(const std::string& key)
    {
        switch (key.size())
        {
        case 16 * 2:
        case 24 * 2:
        case 32 * 2:
            return true;
        }

        return false;
    }

    static std::map<std::string, std::vector<uint8_t>> load_key_file(const Config& config)
    {
        std::map<std::string, std::vector<uint8_t>> rval;
        auto [str, err] = mxb::load_file<std::string>(config.keyfile);

        if (!str.empty())
        {
            mxb::trim(str);
            std::string id = std::to_string(config.id);

            for (auto line : mxb::strtok(str, "\n"))
            {
                mxb::trim(line);

                if (auto tok = mxb::strtok(line, ";"); tok.size() == 2)
                {
                    mxb::trim(tok[0]);
                    mxb::trim(tok[1]);
                    char* end;


                    if (is_hex_key(tok[1]) && strtoul(tok[0].c_str(), &end, 10) && *end == '\0')
                    {
                        if (auto key = mxs::from_hex(tok[1]); !key.empty())
                        {
                            rval.emplace(std::move(tok[0]), std::move(key));
                        }
                    }
                }
            }

            if (rval.empty())
            {
                MXB_ERROR("File '%s' does not contain a valid encryption key.", config.keyfile.c_str());
            }
        }
        else if (!err.empty())
        {
            MXB_ERROR("%s", err.c_str());
        }

        return rval;
    }

    std::map<std::string, std::vector<uint8_t>> m_keys;
};

/*
    Copyright (c) 2011-2012 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2012 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>. 
*/

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>

#include "cocaine/app.hpp"

#include "cocaine/archive.hpp"
#include "cocaine/context.hpp"
#include "cocaine/engine.hpp"
#include "cocaine/logging.hpp"
#include "cocaine/manifest.hpp"
#include "cocaine/profile.hpp"
#include "cocaine/rpc.hpp"

#include "cocaine/traits/json.hpp"

using namespace cocaine;
using namespace cocaine::engine;

namespace fs = boost::filesystem;

app_t::app_t(context_t& context, const std::string& name, const std::string& profile):
    m_context(context),
    m_log(context.log(
        (boost::format("app/%1%")
            % name
        ).str()
    )),
    m_manifest(new manifest_t(context, name)),
    m_profile(new profile_t(context, profile))
{
    fs::path path(fs::path(m_context.config.spool_path) / name);
    
    if(!fs::exists(path)) {
        deploy(name, path.string());
    }

    m_control.reset(new io::channel_t(context, ZMQ_PAIR));

    std::string endpoint(
        (boost::format("inproc://%s")
            % m_manifest->name
        ).str()
    );

    try { 
        m_control->bind(endpoint);
    } catch(const zmq::error_t& e) {
        boost::format message("unable to bind the engine control channel - %s");
        throw configuration_error_t((message % e.what()).str());
    }

    // NOTE: The event loop is not started here yet.
    m_engine.reset(
        new engine_t(
            m_context,
            *m_manifest,
            *m_profile
        )
    );
}

app_t::~app_t() {
    // NOTE: Stop the engine first, so that there won't be any
    // new events during the drivers shutdown process.
    stop();
}

void
app_t::start() {
    if(m_thread) {
        return;
    }

    COCAINE_LOG_INFO(m_log, "starting the engine");

    if(!m_manifest->drivers.empty()) {
        COCAINE_LOG_INFO(
            m_log,
            "starting %llu %s",
            m_manifest->drivers.size(),
            m_manifest->drivers.size() == 1 ? "driver" : "drivers"
        );

        boost::format format("%s/%s");

        for(config_t::component_map_t::const_iterator it = m_manifest->drivers.begin();
            it != m_manifest->drivers.end();
            ++it)
        {
            try {
                format % m_manifest->name % it->first;

                m_drivers.emplace(
                    it->first,
                    m_context.get<api::driver_t>(
                        it->second.type,
                        api::category_traits<api::driver_t>::args_type(
                            *m_engine,
                            format.str(),
                            it->second.args
                        )
                    )
                );
            } catch(const std::exception& e) {
                COCAINE_LOG_ERROR(
                    m_log,
                    "unable to initialize the '%s' driver - %s",
                    format.str(),
                    e.what()
                );

                throw configuration_error_t("unable to initialize the drivers");
            }

            format.clear();
        }
    }

    m_thread.reset(
        new boost::thread(
            boost::bind(
                &engine_t::run,
                boost::ref(*m_engine)
            )
        )
    );
}

void
app_t::stop() {
    if(!m_thread) {
        return;
    }

    COCAINE_LOG_INFO(m_log, "stopping the engine");
    
    m_control->send_message(
        io::message<control::terminate>()
    );

    m_thread->join();
    m_thread.reset();

    // NOTE: Stop the drivers, so that there won't be any open
    // sockets and so on while the engine is stopped.
    m_drivers.clear();
}

Json::Value
app_t::info() const {
    Json::Value info(Json::objectValue);

    if(!m_thread) {
        info["error"] = "engine is not active";
        return info;
    }

    m_control->send_message(
        io::message<control::status>()
    );

    {
        io::scoped_option<
            io::options::receive_timeout
        > option(*m_control, defaults::control_timeout);

        if(!m_control->recv(info)) {
            info["error"] = "engine is unresponsive";
            return info;
        }
    }

    info["profile"] = m_profile->name;

    for(driver_map_t::const_iterator it = m_drivers.begin();
        it != m_drivers.end();
        ++it) 
    {
        info["drivers"][it->first] = it->second->info();
    }

    return info;
}

bool
app_t::enqueue(const boost::shared_ptr<job_t>& job,
               mode::value mode)
{
    return m_engine->enqueue(job, mode);
}

void
app_t::deploy(const std::string& name, 
              const std::string& path)
{
    std::string blob;

    COCAINE_LOG_INFO(m_log, "deploying the app to '%s'", path);
    
    api::category_traits<api::storage_t>::ptr_type storage(
        m_context.get<api::storage_t>("storage/core")
    );
    
    try {
        blob = storage->get<std::string>("apps", name);
    } catch(const storage_error_t& e) {
        COCAINE_LOG_ERROR(m_log, "unable to fetch the app from the storage - %s", e.what());
        boost::format message("the '%s' app is not available");
        throw configuration_error_t((message % name).str());
    }
    
    try {
        archive_t archive(m_context, blob);
        archive.deploy(path);
    } catch(const archive_error_t& e) {
        COCAINE_LOG_ERROR(m_log, "unable to extract the app files - %s", e.what());
        boost::format message("the '%s' app is not available");
        throw configuration_error_t((message % name).str());
    }
}

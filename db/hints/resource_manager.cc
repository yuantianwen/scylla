/*
 * Copyright (C) 2018 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "resource_manager.hh"
#include "manager.hh"
#include "log.hh"
#include <boost/range/algorithm/for_each.hpp>
#include <boost/range/adaptor/map.hpp>
#include "lister.hh"
#include "disk-error-handler.hh"
#include "seastarx.hh"

namespace db {
namespace hints {

static logging::logger resource_manager_logger("hints_resource_manager");

future<semaphore_units<semaphore_default_exception_factory>> resource_manager::get_send_units_for(size_t buf_size) {
    // Let's approximate the memory size the mutation is going to consume by the size of its serialized form
    size_t hint_memory_budget = std::max(_min_send_hint_budget, buf_size);
    // Allow a very big mutation to be sent out by consuming the whole shard budget
    hint_memory_budget = std::min(hint_memory_budget, _max_send_in_flight_memory);
    resource_manager_logger.trace("memory budget: need {} have {}", hint_memory_budget, _send_limiter.available_units());
    return get_units(_send_limiter, hint_memory_budget);
}

const std::chrono::seconds space_watchdog::_watchdog_period = std::chrono::seconds(1);

space_watchdog::space_watchdog(shard_managers_set& managers)
    : _shard_managers(managers)
    , _timer([this] { on_timer(); })
{}

void space_watchdog::start() {
    _timer.arm(timer_clock_type::now());
}

future<> space_watchdog::stop() noexcept {
    try {
        return _gate.close().finally([this] { _timer.cancel(); });
    } catch (...) {
        return make_exception_future<>(std::current_exception());
    }
}

future<> space_watchdog::scan_one_ep_dir(boost::filesystem::path path, manager& shard_manager, ep_key_type ep_key) {
    return lister::scan_dir(path, { directory_entry_type::regular }, [this, ep_key, &shard_manager] (lister::path dir, directory_entry de) {
        // Put the current end point ID to state.eps_with_pending_hints when we see the second hints file in its directory
        if (_files_count == 1) {
            shard_manager.add_ep_with_pending_hints(ep_key);
        }
        ++_files_count;

        return io_check(file_size, (dir / de.name.c_str()).c_str()).then([this] (uint64_t fsize) {
            _total_size += fsize;
        });
    });
}

size_t space_watchdog::end_point_managers_count() const {
    return boost::accumulate(_shard_managers, 0, [] (size_t sum, manager& shard_manager) {
        return sum + shard_manager.ep_managers_size();
    });
}

void space_watchdog::on_timer() {
    with_gate(_gate, [this] {
        return futurize_apply([this] {
            _total_size = 0;

            return do_for_each(_shard_managers, [this] (manager& shard_manager) {
                shard_manager.clear_eps_with_pending_hints();

                // The hints directories are organized as follows:
                // <hints root>
                //    |- <shard1 ID>
                //    |  |- <EP1 address>
                //    |     |- <hints file1>
                //    |     |- <hints file2>
                //    |     |- ...
                //    |  |- <EP2 address>
                //    |     |- ...
                //    |  |-...
                //    |- <shard2 ID>
                //    |  |- ...
                //    ...
                //    |- <shardN ID>
                //    |  |- ...
                //
                return lister::scan_dir(shard_manager.hints_dir(), {directory_entry_type::directory}, [this, &shard_manager] (lister::path dir, directory_entry de) {
                    _files_count = 0;
                    // Let's scan per-end-point directories and enumerate hints files...
                    //
                    // Let's check if there is a corresponding end point manager (may not exist if the corresponding DC is
                    // not hintable).
                    // If exists - let's take a file update lock so that files are not changed under our feet. Otherwise, simply
                    // continue to enumeration - there is no one to change them.
                    auto it = shard_manager.find_ep_manager(de.name);
                    if (it != shard_manager.ep_managers_end()) {
                        return with_lock(it->second.file_update_mutex(), [this, &shard_manager, dir = std::move(dir), ep_name = std::move(de.name)]() mutable {
                             return scan_one_ep_dir(dir / ep_name.c_str(), shard_manager, ep_key_type(ep_name));
                        });
                    } else {
                        return scan_one_ep_dir(dir / de.name.c_str(), shard_manager, ep_key_type(de.name));
                    }
                });
            }).then([this] {
                // Adjust the quota to take into account the space we guarantee to every end point manager
                size_t adjusted_quota = 0;
                size_t delta = end_point_managers_count() * resource_manager::hint_segment_size_in_mb * 1024 * 1024;
                if (resource_manager::max_shard_disk_space_size > delta) {
                    adjusted_quota = resource_manager::max_shard_disk_space_size - delta;
                }

                bool can_hint = _total_size < adjusted_quota;
                resource_manager_logger.trace("space_watchdog: total_size ({}) {} max_shard_disk_space_size ({})", _total_size, can_hint ? "<" : ">=", adjusted_quota);

                if (!can_hint) {
                    for (manager& shard_manager : _shard_managers) {
                        shard_manager.forbid_hints_for_eps_with_pending_hints();
                    }
                } else {
                    for (manager& shard_manager : _shard_managers) {
                        shard_manager.allow_hints();
                    }
                }
            });
        }).handle_exception([this] (auto eptr) {
            resource_manager_logger.trace("space_watchdog: unexpected exception - stop all hints generators");
            // Stop all hint generators if space_watchdog callback failed
            for (manager& shard_manager : _shard_managers) {
                shard_manager.forbid_hints();
            }
        }).finally([this] {
            _timer.arm(_watchdog_period);
        });
    });
}

future<> resource_manager::start(shared_ptr<service::storage_proxy> proxy_ptr, shared_ptr<gms::gossiper> gossiper_ptr, shared_ptr<service::storage_service> ss_ptr) {
    return parallel_for_each(_shard_managers, [proxy_ptr, gossiper_ptr, ss_ptr](manager& m) {
        return m.start(proxy_ptr, gossiper_ptr, ss_ptr);
    }).then([this]() {
        return _space_watchdog.start();
    });
}

future<> resource_manager::stop() noexcept {
    return parallel_for_each(_shard_managers, [](manager& m) {
        return m.stop();
    }).finally([this]() {
        return _space_watchdog.stop();
    });
}

void resource_manager::register_manager(manager& m) {
    _shard_managers.insert(m);
}

}
}

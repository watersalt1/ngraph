//*****************************************************************************
// Copyright 2017-2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#pragma once

#include <map>
#include <mutex>

#include <onnxifi.h>

#include "backend_manager.hpp"
#include "event.hpp"
#include "exceptions.hpp"

namespace ngraph
{
    namespace onnxifi
    {
        /// \brief ONNXIFI event manager
        class EventManager
        {
        public:
            EventManager(const EventManager&) = delete;
            EventManager& operator=(const EventManager&) = delete;

            EventManager(EventManager&& other) noexcept = delete;
            EventManager& operator=(EventManager&& other) noexcept = delete;

            static void init_event(::onnxBackend handle, ::onnxEvent* event);

            static void release_event(::onnxEvent event) { instance()._release_event(event); }
        private:
            mutable std::mutex m_mutex{};
            std::map<::onnxEvent, std::unique_ptr<Event>> m_registered_events{};

            EventManager() = default;

            static EventManager& instance()
            {
                static EventManager event_manager;
                return event_manager;
            }

            ::onnxEvent _init_event(const Backend& backend);
            void _release_event(::onnxEvent event);
        };

    } // namespace onnxifi

} // namespace ngraph

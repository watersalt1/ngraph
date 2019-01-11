//*****************************************************************************
// Copyright 2017-2019 Intel Corporation
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

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ngraph/function.hpp"
#include "ngraph/runtime/nvidiagpu/nvidiagpu_backend.hpp"
#include "ngraph/runtime/nvidiagpu/nvidiagpu_call_frame.hpp"
#include "ngraph/runtime/nvidiagpu/nvidiagpu_tensor_wrapper.hpp"

namespace ngraph
{
    namespace runtime
    {
        namespace nvidiagpu
        {
            class CallFrame;
            class NVRuntimeConstructor
            {
            public:
                using op_runtime_t =
                    std::function<void(CallFrame& call_frame, RuntimeContext* ctx)>;
                using op_order_t =
                    std::unordered_map<std::shared_ptr<Function>, std::list<std::shared_ptr<Node>>>;

                NVRuntimeConstructor(const op_order_t& ordered_ops);
                void add(const std::string& name, const op_runtime_t& step);
                void add_call(const std::string& caller,
                              const std::string& callee,
                              const std::vector<runtime::nvidiagpu::TensorWrapper>& args,
                              const std::vector<runtime::nvidiagpu::TensorWrapper>& out);
                EntryPoint build(const std::string& function, CallFrame& call_frame);

            private:
                std::unordered_map<std::string, std::vector<op_runtime_t>> m_runtime;
            };
        }
    }
}
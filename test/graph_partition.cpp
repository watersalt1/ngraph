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

#include <memory>
#include <sstream>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <vector>

#include "gtest/gtest.h"

#include "ngraph/graph_util.hpp"
#include "ngraph/ngraph.hpp"
#include "ngraph/pass/assign_placement.hpp"
#include "ngraph/pass/manager.hpp"
#include "ngraph/runtime/host_tensor.hpp"
#include "ngraph/util.hpp"
#include "util/ndarray.hpp"
#include "util/test_tools.hpp"

using namespace std;
using namespace ngraph;

TEST(graph_partition, placement_all_cpu_policy)
{
    Shape shape = Shape{2, 2};
    shared_ptr<op::Parameter> A = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<op::Parameter> B = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<op::Parameter> C = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<Node> AplusB = A + B;
    shared_ptr<Node> AplusBtimesC = AplusB * C;
    shared_ptr<Function> f = make_shared<Function>(AplusBtimesC, ParameterVector{A, B, C});

    for (auto node : f->get_ordered_ops())
    {
        EXPECT_EQ(node->get_placement(), Placement::DEFAULT);
    }

    pass::Manager pass_manager;
    pass_manager.register_pass<pass::AssignPlacement>(
        [](shared_ptr<Node> node) { return Placement::CPU; });
    pass_manager.run_passes(f);

    for (auto node : f->get_ordered_ops())
    {
        EXPECT_EQ(node->get_placement(), Placement::CPU);
    }
}

#ifdef NGRAPH_CPU_ENABLE

// Perform all operations on INTERPRETER and fallback Multiply to CPU
static function<Placement(shared_ptr<Node>)> int_with_cpu_mul_policy = [](shared_ptr<Node> node) {
    Placement placement;
    string node_op = node->description();
    if (node_op == "Multiply")
    {
        placement = Placement::CPU;
    }
    else
    {
        placement = Placement::INTERPRETER;
    }
    return placement;
};

// HybridCallFrame servers 2 purposes:
// 1. HybridBackend's main use case is to test device placement and graph partition routines.
// 2. It also shows how glued-hybrid runtime can be built by combining different runtimes.
//
// By default, HybridBackend operates on INTERPRETER (for example, the tensor view is
// INTERPRETER tensor view). It falls back to CPU when requested by placement.
class HybridExecutable;
class HybridBackend : public runtime::Backend
{
public:
    HybridBackend(const function<Placement(shared_ptr<Node>)>& placement_policy)
        : m_placement_policy(placement_policy)
    {
    }

    ~HybridBackend() {}
    shared_ptr<runtime::Tensor> create_tensor(const element::Type& element_type,
                                              const Shape& shape) override
    {
        return get_cached_backend(Placement::INTERPRETER)->create_tensor(element_type, shape);
    }

    shared_ptr<runtime::Tensor> create_tensor(const element::Type& element_type,
                                              const Shape& shape,
                                              void* memory_pointer) override
    {
        return get_cached_backend(Placement::INTERPRETER)
            ->create_tensor(element_type, shape, memory_pointer);
    }

    std::unique_ptr<runtime::Executable>
        compile(shared_ptr<Function> function, bool enable_performance_collection = false) override;

    shared_ptr<runtime::Backend> get_cached_backend(Placement placement)
    {
        if (m_cached_backends.find(placement) == m_cached_backends.end())
        {
            m_cached_backends[placement] = runtime::Backend::create(placement_to_string(placement));
        }
        return m_cached_backends.at(placement);
    }

    map<Placement, shared_ptr<runtime::Backend>> m_cached_backends;
    function<Placement(shared_ptr<Node>)> m_placement_policy;
};

class HybridExecutable : public runtime::Executable
{
public:
    HybridExecutable(HybridBackend* backend,
                     shared_ptr<Function> func,
                     bool enable_performance_collection)
        : m_hybrid_backend(backend)
    {
        // Clone function
        m_function = clone_function(*func);

        // Run placement pass
        pass::Manager pass_manager;
        pass_manager.register_pass<pass::AssignPlacement>(int_with_cpu_mul_policy);
        pass_manager.run_passes(m_function);

        // Split function to sub_functions
        tie(m_sub_functions, m_map_parameter_to_result) = split_function_by_placement(m_function);

        // Compile subfunctions in corresponding backends
        for (shared_ptr<Function>& sub_function : m_sub_functions)
        {
            Placement placement = get_colocated_function_placement(sub_function);
            auto be = m_hybrid_backend->get_cached_backend(placement);
            shared_ptr<runtime::Executable> h = be->compile(sub_function);
            m_handle_map[sub_function] = h;
        }
        set_parameters_and_results(*m_function);
    }

    bool execute(const vector<shared_ptr<runtime::Tensor>>& outputs,
                 const vector<shared_ptr<runtime::Tensor>>& inputs)
    {
        bool rc = true;

        // Parameter and result node in sub_function maps to one Tensor
        unordered_map<shared_ptr<Node>, shared_ptr<runtime::Tensor>> map_node_to_tensor_view;
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            map_node_to_tensor_view[m_function->get_parameters()[i]] = inputs[i];
        }
        for (size_t i = 0; i < outputs.size(); ++i)
        {
            map_node_to_tensor_view[m_function->get_results()[i]] = outputs[i];
        }

        // Call subfunctions
        vector<shared_ptr<runtime::Tensor>> live_list;
        for (shared_ptr<Function>& sub_function : m_sub_functions)
        {
            // Init backend
            Placement placement = get_colocated_function_placement(sub_function);
            auto backend = m_hybrid_backend->get_cached_backend(placement);

            // Prepare parameter TensorViews
            vector<shared_ptr<runtime::Tensor>> parameter_tvs;
            for (auto parameter_node : sub_function->get_parameters())
            {
                if (map_node_to_tensor_view.find(parameter_node) != map_node_to_tensor_view.end())
                {
                    parameter_tvs.push_back(map_node_to_tensor_view.at(parameter_node));
                }
                else
                {
                    auto result_node = m_map_parameter_to_result.at(parameter_node);
                    auto result_tv = map_node_to_tensor_view.at(result_node);
                    auto parameter_tv = backend->create_tensor(parameter_node->get_element_type(),
                                                               parameter_node->get_shape());
                    live_list.push_back(parameter_tv);
                    copy_data(parameter_tv, read_vector<float>(result_tv));
                    map_node_to_tensor_view[parameter_node] = parameter_tv;
                    parameter_tvs.push_back(parameter_tv);
                }
            }

            // Prepare result TensorViews
            vector<shared_ptr<runtime::Tensor>> result_tvs;
            for (auto result_node : sub_function->get_results())
            {
                if (map_node_to_tensor_view.find(result_node) != map_node_to_tensor_view.end())
                {
                    result_tvs.push_back(map_node_to_tensor_view.at(result_node));
                }
                else
                {
                    auto result_tv = backend->create_tensor(result_node->get_element_type(),
                                                            result_node->get_shape());
                    live_list.push_back(result_tv);
                    map_node_to_tensor_view[result_node] = result_tv;
                    result_tvs.push_back(result_tv);
                }
            }

            // Call
            m_handle_map.at(sub_function)->validate_and_execute(result_tvs, parameter_tvs);
        }
        return rc;
    }

private:
    HybridBackend* m_hybrid_backend;
    shared_ptr<Function> m_function;
    vector<shared_ptr<Function>> m_sub_functions;
    unordered_map<shared_ptr<op::Parameter>, shared_ptr<op::Result>> m_map_parameter_to_result;
    unordered_map<shared_ptr<Function>, shared_ptr<runtime::Executable>> m_handle_map;
};

unique_ptr<runtime::Executable> HybridBackend::compile(shared_ptr<Function> function,
                                                       bool enable_performance_collection)
{
    unique_ptr<HybridExecutable> exec{
        new HybridExecutable(this, function, enable_performance_collection)};

    return exec;
}

TEST(graph_partition, placement_int_with_cpu_mul_policy)
{
    Shape shape = Shape{2, 2};
    shared_ptr<op::Parameter> A = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<op::Parameter> B = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<op::Parameter> C = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<Node> AplusB = A + B;
    shared_ptr<Node> AplusBtimesC = AplusB * C;
    shared_ptr<Function> f = make_shared<Function>(AplusBtimesC, ParameterVector{A, B, C});

    for (auto node : f->get_ordered_ops())
    {
        EXPECT_EQ(node->get_placement(), Placement::DEFAULT);
    }

    pass::Manager pass_manager;
    pass_manager.register_pass<pass::AssignPlacement>(int_with_cpu_mul_policy);
    pass_manager.run_passes(f);

    for (auto node : f->get_ordered_ops())
    {
        string node_op = node->description();
        if (node_op == "Multiply")
        {
            EXPECT_EQ(node->get_placement(), Placement::CPU);
        }
        else
        {
            EXPECT_EQ(node->get_placement(), Placement::INTERPRETER);
        }
    }
}

TEST(graph_partition, hybrid_abc_manual)
{
    // A   B   C    A   B     C
    //  \ /   /      \ /     /
    //   +D  /        +D    /
    //    \ /         |    /
    //     *E         R0  R1  f0(INT)
    //     |       ------------------
    //     R          P0  P1
    //                 \ /
    //                  *E
    //                  |
    //                  R2    f1(CPU)
    //             ------------------
    //                  P2
    //                  |
    //                  R     f2(INT)
    //             ------------------
    Shape shape = Shape{2, 2};
    auto A = make_shared<op::Parameter>(element::f32, shape);
    auto B = make_shared<op::Parameter>(element::f32, shape);
    auto C = make_shared<op::Parameter>(element::f32, shape);
    auto D = A + B;
    auto E = D * C;
    auto R = make_shared<op::Result>(E);
    auto f = make_shared<Function>(ResultVector{R}, ParameterVector{A, B, C});

    pass::Manager pass_manager;
    pass_manager.register_pass<pass::AssignPlacement>(int_with_cpu_mul_policy);
    pass_manager.run_passes(f);

    // Insert parameter
    auto RP0 = insert_result_parameter_split(D, E);
    shared_ptr<op::Result> R0 = RP0.first;
    shared_ptr<op::Parameter> P0 = RP0.second;
    auto RP1 = insert_result_parameter_split(C, E);
    shared_ptr<op::Result> R1 = RP1.first;
    shared_ptr<op::Parameter> P1 = RP1.second;
    auto RP2 = insert_result_parameter_split(E, R);
    shared_ptr<op::Result> R2 = RP2.first;
    shared_ptr<op::Parameter> P2 = RP2.second;

    // Backends
    auto int_backend = runtime::Backend::create(placement_to_string(Placement::INTERPRETER));
    auto cpu_backend = runtime::Backend::create(placement_to_string(Placement::CPU));

    // f0 on INT
    auto a = int_backend->create_tensor(element::f32, shape);
    auto b = int_backend->create_tensor(element::f32, shape);
    auto c = int_backend->create_tensor(element::f32, shape);
    auto r0 = int_backend->create_tensor(element::f32, shape);
    auto r1 = int_backend->create_tensor(element::f32, shape);
    copy_data(a, test::NDArray<float, 2>({{1, 2}, {3, 4}}).get_vector());
    copy_data(b, test::NDArray<float, 2>({{5, 6}, {7, 8}}).get_vector());
    copy_data(c, test::NDArray<float, 2>({{9, 10}, {11, 12}}).get_vector());

    auto f0 = make_shared<Function>(ResultVector{R0, R1}, ParameterVector{A, B, C});
    auto int_handle = int_backend->compile(f0);
    int_handle->validate_and_execute({r0, r1}, {a, b, c});

    // f1 on CPU
    auto p0 = cpu_backend->create_tensor(element::f32, shape);
    auto p1 = cpu_backend->create_tensor(element::f32, shape);
    auto r2 = cpu_backend->create_tensor(element::f32, shape);
    copy_data(p0, read_vector<float>(r0));
    copy_data(p1, read_vector<float>(r1));

    auto f1 = make_shared<Function>(ResultVector{R2}, ParameterVector{P0, P1});
    auto cpu_handle = cpu_backend->compile(f1);
    cpu_handle->validate_and_execute({r2}, {p0, p1});

    // f2 on INT
    auto p2 = int_backend->create_tensor(element::f32, shape);
    auto r = int_backend->create_tensor(element::f32, shape);
    copy_data(p2, read_vector<float>(r2));

    auto f2 = make_shared<Function>(ResultVector{R}, ParameterVector{P2});
    auto int_handle2 = int_backend->compile(f2);
    int_handle2->validate_and_execute({r}, {p2});

    // Check final result on INT
    EXPECT_EQ(read_vector<float>(r),
              (test::NDArray<float, 2>({{54, 80}, {110, 144}})).get_vector());
}

TEST(graph_partition, hybrid_abc)
{
    // Same as hybrid_abc_manual, but using the test hybrid transformer
    //
    // A   B   C    A   B     C
    //  \ /   /      \ /     /
    //   +D  /        +D    /
    //    \ /         |    /
    //     *E         R0  R1  f0(INT)
    //     |       ------------------
    //     R          P0  P1
    //                 \ /
    //                  *E
    //                  |
    //                  R2    f1(CPU)
    //             ------------------
    //                  P2
    //                  |
    //                  R     f2(INT)
    //             ------------------
    Shape shape = Shape{2, 2};
    auto A = make_shared<op::Parameter>(element::f32, shape);
    auto B = make_shared<op::Parameter>(element::f32, shape);
    auto C = make_shared<op::Parameter>(element::f32, shape);
    auto D = A + B;
    auto E = D * C;
    auto R = make_shared<op::Result>(E);
    auto f = make_shared<Function>(ResultVector{R}, ParameterVector{A, B, C});

    auto backend = make_shared<HybridBackend>(int_with_cpu_mul_policy);
    shared_ptr<runtime::Tensor> a = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> b = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> c = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> r = backend->create_tensor(element::f32, shape);

    copy_data(a, test::NDArray<float, 2>({{1, 2}, {3, 4}}).get_vector());
    copy_data(b, test::NDArray<float, 2>({{5, 6}, {7, 8}}).get_vector());
    copy_data(c, test::NDArray<float, 2>({{9, 10}, {11, 12}}).get_vector());

    auto handle = backend->compile(f);
    handle->validate_and_execute({r}, {a, b, c});
    EXPECT_EQ(read_vector<float>(r),
              (test::NDArray<float, 2>({{54, 80}, {110, 144}})).get_vector());
}

TEST(graph_partition, hybrid_abcd)
{
    //   A   B
    //    \ /
    // C  E*   D
    //  \ / \ /
    //  F+  G+
    //    \ /
    //    H+
    Shape shape = Shape{2, 2};
    shared_ptr<op::Parameter> A = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<op::Parameter> B = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<op::Parameter> C = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<op::Parameter> D = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<Node> E = A * B;
    shared_ptr<Node> F = C + E;
    shared_ptr<Node> G = E + D;
    shared_ptr<Node> H = F + G;
    shared_ptr<Function> f = make_shared<Function>(H, ParameterVector{A, B, C, D});

    auto backend = make_shared<HybridBackend>(int_with_cpu_mul_policy);
    auto handle = backend->compile(f);

    shared_ptr<runtime::Tensor> a = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> b = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> c = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> d = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> r = backend->create_tensor(element::f32, shape);

    copy_data(a, test::NDArray<float, 2>({{1, 2}, {3, 4}}).get_vector());
    copy_data(b, test::NDArray<float, 2>({{5, 6}, {7, 8}}).get_vector());
    copy_data(c, test::NDArray<float, 2>({{9, 10}, {11, 12}}).get_vector());
    copy_data(d, test::NDArray<float, 2>({{13, 14}, {15, 16}}).get_vector());

    handle->validate_and_execute({r}, {a, b, c, d});
    EXPECT_EQ(read_vector<float>(r), (test::NDArray<float, 2>({{32, 48}, {68, 92}})).get_vector());
}

TEST(graph_partition, hybrid_back_and_forth)
{
    // A   B
    //  \ / \
    //  D*   |
    //    \ /
    //    E+   C
    //      \ /
    //      F*
    Shape shape = Shape{2, 2};
    shared_ptr<op::Parameter> A = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<op::Parameter> B = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<op::Parameter> C = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<Node> D = A * B;
    shared_ptr<Node> E = D + B;
    shared_ptr<Node> F = E * C;
    shared_ptr<Function> f = make_shared<Function>(F, ParameterVector{A, B, C});

    auto backend = make_shared<HybridBackend>(int_with_cpu_mul_policy);
    auto handle = backend->compile(f);

    shared_ptr<runtime::Tensor> a = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> b = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> c = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> r = backend->create_tensor(element::f32, shape);

    copy_data(a, test::NDArray<float, 2>({{1, 2}, {3, 4}}).get_vector());
    copy_data(b, test::NDArray<float, 2>({{5, 6}, {7, 8}}).get_vector());
    copy_data(c, test::NDArray<float, 2>({{9, 10}, {11, 12}}).get_vector());

    handle->validate_and_execute({r}, {a, b, c});
    EXPECT_EQ(read_vector<float>(r),
              (test::NDArray<float, 2>({{90, 180}, {308, 480}})).get_vector());
}

TEST(graph_partition, hybrid_multi_middle_nodes)
{
    // A   B   C
    //  \ / \ / \
    //  D+  E+  |
    //    \ / \ /
    //    F*  G*
    //      \ /
    //      H+
    Shape shape = Shape{2, 2};
    shared_ptr<op::Parameter> A = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<op::Parameter> B = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<op::Parameter> C = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<Node> D = A + B;
    shared_ptr<Node> E = B + C;
    shared_ptr<Node> F = D * E;
    shared_ptr<Node> G = E * C;
    shared_ptr<Node> H = F + G;
    shared_ptr<Function> f = make_shared<Function>(H, ParameterVector{A, B, C});

    auto backend = make_shared<HybridBackend>(int_with_cpu_mul_policy);
    auto handle = backend->compile(f);

    shared_ptr<runtime::Tensor> a = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> b = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> c = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> r = backend->create_tensor(element::f32, shape);

    copy_data(a, test::NDArray<float, 2>({{1, 2}, {3, 4}}).get_vector());
    copy_data(b, test::NDArray<float, 2>({{5, 6}, {7, 8}}).get_vector());
    copy_data(c, test::NDArray<float, 2>({{9, 10}, {11, 12}}).get_vector());

    handle->validate_and_execute({r}, {a, b, c});
    EXPECT_EQ(read_vector<float>(r),
              (test::NDArray<float, 2>({{210, 288}, {378, 480}})).get_vector());
}

TEST(graph_partition, hybrid_no_split)
{
    // A   B
    //  \ /
    //   +
    Shape shape = Shape{2, 2};
    shared_ptr<op::Parameter> A = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<op::Parameter> B = make_shared<op::Parameter>(element::f32, shape);
    shared_ptr<Node> C = A + B;
    shared_ptr<Function> f = make_shared<Function>(C, ParameterVector{A, B});

    auto backend = make_shared<HybridBackend>(int_with_cpu_mul_policy);
    auto handle = backend->compile(f);

    shared_ptr<runtime::Tensor> a = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> b = backend->create_tensor(element::f32, shape);
    shared_ptr<runtime::Tensor> c = backend->create_tensor(element::f32, shape);

    copy_data(a, test::NDArray<float, 2>({{1, 2}, {3, 4}}).get_vector());
    copy_data(b, test::NDArray<float, 2>({{5, 6}, {7, 8}}).get_vector());

    handle->validate_and_execute({c}, {a, b});
    EXPECT_EQ(read_vector<float>(c), (test::NDArray<float, 2>({{6, 8}, {10, 12}})).get_vector());
}

#endif

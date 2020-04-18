//*****************************************************************************
// Copyright 2017-2020 Intel Corporation
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

#include <algorithm>
#include <vector>

#include "ngraph/evaluator_tensor.hpp"
#include "ngraph/op/concat.hpp"
#include "ngraph/op/constant.hpp"
#include "ngraph/op/gather.hpp"
#include "ngraph/op/shape_of.hpp"
#include "ngraph/pass/constant_folding.hpp"
#include "ngraph/runtime/reference/shape_of.hpp"
#include "ngraph/type/element_type_traits.hpp"

using namespace std;
using namespace ngraph;

constexpr NodeTypeInfo op::v3::ShapeOf::type_info;

op::v3::ShapeOf::ShapeOf(const Output<Node>& arg, element::Type output_type)
    : Op({arg})
    , m_output_type(output_type)
{
    constructor_validate_and_infer_types();
}

void op::v3::ShapeOf::validate_and_infer_types()
{
    NODE_VALIDATION_CHECK(this,
                          m_output_type == element::i64 || m_output_type == element::i32,
                          "Output type must be i32 or i64");
    set_input_is_relevant_to_value(0, false);
    set_output_type(0, m_output_type, PartialShape{get_input_partial_shape(0).rank()});
}

bool ngraph::op::v3::ShapeOf::visit_attributes(AttributeVisitor& visitor)
{
    visitor.on_attribute("output_type", m_output_type);
    return true;
}

shared_ptr<Node> op::v3::ShapeOf::clone_with_new_inputs(const OutputVector& new_args) const
{
    check_new_args_count(this, new_args);
    auto new_shape_of = make_shared<op::v3::ShapeOf>(new_args.at(0), m_output_type);
    new_shape_of->set_is_foldable(m_is_foldable);
    return new_shape_of;
}

namespace
{
    template <element::Type_t ET>
    inline bool kernel_selector(const element::Type_t& element_type,
                                const Shape& shape,
                                const EvaluatorTensorPtr& output_value)
    {
        if (element_type == ET)
        {
            runtime::reference::shape_of(shape, output_value->get_ptr<ET>());
            return true;
        }
        else
        {
            return false;
        }
    }

    void evaluate_shape_of(const element::Type_t& element_type,
                           const EvaluatorTensorPtr& output_value,
                           const EvaluatorTensorPtr& input_value)
    {
        Shape shape = input_value->get_shape();
        if (!(kernel_selector<element::Type_t::i32>(element_type, shape, output_value) ||
              kernel_selector<element::Type_t::i64>(element_type, shape, output_value)))
        {
            NGRAPH_CHECK(false, "Invalid element type: ", element_type);
        }
    }

    bool constant_fold_shape_of(Node* shape_of_node,
                                Output<Node>& replacement,
                                const Output<Node>& shape_of_input,
                                bool is_foldable)
    {
        auto partial_shape = shape_of_input.get_partial_shape();
        auto output_type = shape_of_node->get_output_element_type(0);
        if (partial_shape.is_static())
        {
            NGRAPH_CHECK(pass::revalidate_and_ensure_static(shape_of_node->shared_from_this()));
            auto arg_shape = shape_of_input.get_shape();
            auto constant =
                make_shared<op::v0::Constant>(output_type, shape_of_node->get_output_shape(0));
            constant->allocate_buffer();
            evaluate_shape_of(output_type,
                              constant->get_evaluator_tensor(0),
                              shape_of_input.get_evaluator_tensor());
            replacement = constant;
            return true;
        }
        else if (partial_shape.rank().is_static() && is_foldable)
        {
            auto shape_of = shape_of_node->copy_with_new_inputs({shape_of_input});
            // Ugly
            if (auto ps = as_type_ptr<op::v0::ShapeOf>(shape_of))
            {
                ps->set_is_foldable(false);
            }
            else if (auto ps = as_type_ptr<op::v3::ShapeOf>(shape_of))
            {
                ps->set_is_foldable(false);
            }
            auto dimensions = OutputVector{};
            auto output_dimensions = vector<Dimension>(partial_shape);
            for (int64_t i = 0; i < output_dimensions.size(); ++i)
            {
                if (output_dimensions[i].is_static())
                {
                    auto temp = std::make_shared<op::v0::Constant>(
                        output_type,
                        Shape{1},
                        std::vector<int64_t>{output_dimensions[i].get_length()});
                    temp->set_friendly_name("ConstDim/" + temp->get_name());
                    dimensions.push_back(temp);
                }
                else
                {
                    auto index = std::make_shared<op::v0::Constant>(
                        output_type, Shape{1}, std::vector<int64_t>{i});
                    auto axis = std::make_shared<op::v0::Constant>(
                        element::i64, Shape{}, std::vector<int64_t>{0});
                    auto temp = make_shared<op::v1::Gather>(shape_of, index, axis);
                    temp->set_friendly_name("DynDim/" + temp->get_name());
                    dimensions.push_back(temp);
                }
            }

            replacement = std::make_shared<op::Concat>(dimensions, 0);
            return true;
        }
        return false;
    }
}

bool op::v3::ShapeOf::evaluate(EvaluatorTensorVector& output_values,
                               const EvaluatorTensorVector& input_values)
{
    if (input_values[0]->get_partial_shape().is_static())
    {
        evaluate_shape_of(m_output_type, output_values[0], input_values[0]);
        return true;
    }
    return false;
}

bool op::v3::ShapeOf::constant_fold(OutputVector& output_values, const OutputVector& input_values)
{
    return constant_fold_shape_of(this, output_values[0], input_values[0], m_is_foldable);
}

// op::v0::ShapeOf
constexpr NodeTypeInfo op::v0::ShapeOf::type_info;

op::v0::ShapeOf::ShapeOf(const Output<Node>& arg)
    : Op({arg})
{
    constructor_validate_and_infer_types();
}

void op::v0::ShapeOf::validate_and_infer_types()
{
    set_input_is_relevant_to_value(0, false);
    set_output_type(0, element::i64, PartialShape{get_input_partial_shape(0).rank()});
}

bool ngraph::op::v0::ShapeOf::visit_attributes(AttributeVisitor& visitor)
{
    return true;
}

shared_ptr<Node> op::v0::ShapeOf::clone_with_new_inputs(const OutputVector& new_args) const
{
    check_new_args_count(this, new_args);
    auto new_shape_of = make_shared<op::v0::ShapeOf>(new_args.at(0));
    new_shape_of->set_is_foldable(m_is_foldable);
    return new_shape_of;
}

bool op::v0::ShapeOf::evaluate(EvaluatorTensorVector& output_values,
                               const EvaluatorTensorVector& input_values)
{
    if (input_values[0]->get_partial_shape().is_static())
    {
        evaluate_shape_of(element::i64, output_values[0], input_values[0]);
        return true;
    }
    return false;
}

bool op::v0::ShapeOf::constant_fold(OutputVector& output_values, const OutputVector& input_values)
{
    return constant_fold_shape_of(this, output_values[0], input_values[0], m_is_foldable);
}

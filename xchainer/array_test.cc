#include "xchainer/array.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <nonstd/optional.hpp>

#include "xchainer/array_node.h"
#include "xchainer/axes.h"
#include "xchainer/backend.h"
#include "xchainer/backprop_mode.h"
#include "xchainer/backprop_scope.h"
#include "xchainer/backward.h"
#include "xchainer/check_backward.h"
#include "xchainer/constant.h"
#include "xchainer/context.h"
#include "xchainer/device.h"
#include "xchainer/device_id.h"
#include "xchainer/dtype.h"
#include "xchainer/error.h"
#include "xchainer/graph.h"
#include "xchainer/indexable_array.h"
#include "xchainer/indexer.h"
#include "xchainer/op_node.h"
#include "xchainer/scalar.h"
#include "xchainer/shape.h"
#include "xchainer/slice.h"
#include "xchainer/testing/array.h"
#include "xchainer/testing/array_check.h"
#include "xchainer/testing/context_session.h"
#include "xchainer/testing/device_session.h"
#include "xchainer/testing/util.h"

namespace xchainer {
namespace {

class ArrayTest : public ::testing::TestWithParam<std::string> {
protected:
    void SetUp() override {
        const std::string& backend_name = GetParam();
        device_session_.emplace(DeviceId{backend_name, 0});
    }

    void TearDown() override { device_session_.reset(); }

public:
    template <typename T>
    void CheckContiguousFill(T expected, Scalar scalar) {
        Dtype dtype = TypeToDtype<T>;
        Array x = Empty(Shape{3, 2}, dtype);
        x.Fill(scalar);
        testing::ExpectDataEqual(expected, x);
    }

    template <typename T>
    void CheckContiguousFill(T value) {
        CheckContiguousFill(value, value);
    }

private:
    nonstd::optional<testing::DeviceSession> device_session_;
};

TEST_P(ArrayTest, DefaultCtor) {
    Array a;
    EXPECT_EQ(nullptr, internal::GetArrayBody(a));
}

TEST_P(ArrayTest, CopyCtor) {
    Array a = testing::BuildArray({4, 1}).WithData<bool>({true, true, false, false});
    Array b = a;  // NOLINT

    // A copy-constructed instance must share the same body.
    EXPECT_EQ(internal::GetArrayBody(a), internal::GetArrayBody(b));
}

TEST_P(ArrayTest, ArrayMoveCtor) {
    { EXPECT_TRUE(std::is_nothrow_move_constructible<Array>::value); }

    // A view must not be affected by move
    {
        Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
        Array b = a.MakeView();
        Array c = std::move(a);
        testing::ExpectEqual(b, c);
    }

    // A copy must not be affected by move
    {
        Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
        Array b = a.Copy();
        Array c = std::move(a);
        testing::ExpectEqualCopy(b, c);
    }

    // Array body must be transferred by move
    {
        Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
        std::shared_ptr<internal::ArrayBody> body = internal::GetArrayBody(a);
        Array c = std::move(a);
        EXPECT_EQ(body, internal::GetArrayBody(c));
    }
}

TEST_P(ArrayTest, ArrayBodyCtor) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    std::shared_ptr<internal::ArrayBody> body = internal::GetArrayBody(a);
    Array b{body};
    EXPECT_EQ(body, internal::GetArrayBody(b));
    EXPECT_EQ(a.dtype(), b.dtype());
    EXPECT_EQ(a.shape(), b.shape());
    EXPECT_EQ(a.IsContiguous(), b.IsContiguous());
    EXPECT_EQ(a.offset(), b.offset());
    EXPECT_EQ(a.data(), b.data());
    EXPECT_TRUE(internal::GetArrayBody(a)->nodes().empty());
    EXPECT_TRUE(internal::GetArrayBody(b)->nodes().empty());
}

TEST_P(ArrayTest, CopyAssignment) {
    {
        Array a = testing::BuildArray({4, 1}).WithData<bool>({true, true, true, true});
        Array b;
        b = a;

        EXPECT_EQ(internal::GetArrayBody(a), internal::GetArrayBody(b));
    }
    {
        Array a = testing::BuildArray({4, 1}).WithData<bool>({true, true, true, true});
        Array b = testing::BuildArray({1}).WithData<float>({1.0f});
        b = a;

        EXPECT_EQ(internal::GetArrayBody(a), internal::GetArrayBody(b));
    }
}

TEST_P(ArrayTest, MoveAssignment) {
    {
        Array a = testing::BuildArray({4, 1}).WithData<bool>({true, true, true, true});
        Array b;
        std::shared_ptr<internal::ArrayBody> body = internal::GetArrayBody(a);
        b = std::move(a);

        EXPECT_EQ(body, internal::GetArrayBody(b));
    }
    {
        Array a = testing::BuildArray({4, 1}).WithData<bool>({true, true, true, true});
        Array b = testing::BuildArray({1}).WithData<float>({1.0f});
        std::shared_ptr<internal::ArrayBody> body = internal::GetArrayBody(a);
        b = std::move(a);

        EXPECT_EQ(body, internal::GetArrayBody(b));
    }
}

TEST_P(ArrayTest, RequireGrad) {
    // Default graph
    {
        Array x = testing::BuildArray({1}).WithData<float>({2.0f});
        ASSERT_FALSE(x.IsBackpropRequired());
        x.RequireGrad();
        ASSERT_TRUE(x.IsBackpropRequired());
    }

    // User-specified graph
    {
        BackpropScope backprop_scope{"bp1"};
        BackpropId backprop_id = backprop_scope.backprop_id();

        Array x = testing::BuildArray({1}).WithData<float>({2.0f});
        ASSERT_FALSE(x.IsBackpropRequired(backprop_id));
        x.RequireGrad(backprop_id);
        ASSERT_TRUE(x.IsBackpropRequired(backprop_id));
    }
}

TEST_P(ArrayTest, RequireGradDtype) {
    EXPECT_THROW({ Ones(Shape{1}, Dtype::kBool).RequireGrad(); }, DtypeError);
    EXPECT_THROW({ Ones(Shape{1}, Dtype::kInt8).RequireGrad(); }, DtypeError);
    EXPECT_THROW({ Ones(Shape{1}, Dtype::kInt16).RequireGrad(); }, DtypeError);
    EXPECT_THROW({ Ones(Shape{1}, Dtype::kInt32).RequireGrad(); }, DtypeError);
    EXPECT_THROW({ Ones(Shape{1}, Dtype::kInt64).RequireGrad(); }, DtypeError);
    EXPECT_THROW({ Ones(Shape{1}, Dtype::kUInt8).RequireGrad(); }, DtypeError);
    // no throw
    Ones(Shape{1}, Dtype::kFloat32).RequireGrad();
    Ones(Shape{1}, Dtype::kFloat64).RequireGrad();
}

TEST_P(ArrayTest, Grad) {
    using T = float;
    BackpropScope backprop_scope{"bp1"};
    BackpropId backprop_id = backprop_scope.backprop_id();
    Shape shape{2, 3};

    Array x = testing::BuildArray(shape).WithData<T>({5, 3, 2, 1, 4, 6});
    Array g = testing::BuildArray(shape).WithData<T>({8, 4, 6, 3, 2, 1});

    x.RequireGrad(backprop_id);
    g.RequireGrad(backprop_id);

    EXPECT_FALSE(x.GetGrad(backprop_id)) << "grad must be initially unset";

    // Set and get grad
    {
        x.SetGrad(g, backprop_id);

        testing::ExpectEqual(g, *x.GetGrad(backprop_id));
    }

    // Get grad multiple times
    {
        const nonstd::optional<Array>& grad1 = x.GetGrad(backprop_id);
        const nonstd::optional<Array>& grad2 = x.GetGrad(backprop_id);
        EXPECT_EQ(&*grad1, &*grad2) << "Multiple retrieval of grad must return the same arrays";
    }

    // ClearGrad
    {
        Array grad_view = *x.GetGrad(backprop_id);  // Make a view of grad

        x.ClearGrad(backprop_id);

        EXPECT_FALSE(x.GetGrad(backprop_id)) << "grad must be cleared after calling ClearGrad()";

        // ClearGrad() must not affect previously retrieved view to grad
        testing::ExpectEqual(grad_view, g);
    }
}

TEST_P(ArrayTest, InvalidGradNoGraph) {
    using T = float;
    BackpropScope backprop_scope{"bp1"};
    BackpropId backprop_id = backprop_scope.backprop_id();
    Shape shape{2, 3};

    Array x = testing::BuildArray(shape).WithData<T>({5, 3, 2, 1, 4, 6});
    Array g = testing::BuildArray(shape).WithData<T>({8, 4, 6, 3, 2, 1});

    EXPECT_THROW(x.SetGrad(g), XchainerError);  // x does not belong to the default graph.
    EXPECT_THROW(x.SetGrad(g, backprop_id), XchainerError);  // x does not belong to the given graph.
}

TEST_P(ArrayTest, InvalidGradMismatchedShape) {
    using T = float;
    Shape shape{2, 3};
    Shape mismatched_shape{1, 3};

    Array x = testing::BuildArray(shape).WithData<T>({5, 3, 2, 1, 4, 6});
    Array g = testing::BuildArray(mismatched_shape).WithData<T>({8, 4, 6});
    x.RequireGrad();

    EXPECT_THROW(x.SetGrad(g), DimensionError);
}

TEST_P(ArrayTest, InvalidGradMismatchedDtype) {
    using T = float;
    using MismatchedT = int32_t;
    Shape shape{2, 3};

    Array x = testing::BuildArray(shape).WithData<T>({5, 3, 2, 1, 4, 6});
    Array g = testing::BuildArray(shape).WithData<MismatchedT>({8, 4, 6, 3, 2, 1});
    x.RequireGrad();

    EXPECT_THROW(x.SetGrad(g), DtypeError);
}

TEST_P(ArrayTest, InvalidGradMismatchedDevice) {
    XCHAINER_REQUIRE_DEVICE(GetParam(), 2);
    using T = float;
    Shape shape{2, 3};
    Device& device = GetDefaultDevice();
    Device& mismatched_device = device.backend().GetDevice(device.index() + 1);

    Array x = testing::BuildArray(shape).WithData<T>({5, 3, 2, 1, 4, 6}).WithDevice(device);
    Array g = testing::BuildArray(shape).WithData<T>({8, 4, 6, 3, 2, 1}).WithDevice(mismatched_device);
    x.RequireGrad();

    EXPECT_THROW(x.SetGrad(g), DeviceError);
}

TEST_P(ArrayTest, ContiguousFill) {
    CheckContiguousFill(true);
    CheckContiguousFill(false);
    CheckContiguousFill(int8_t{0});
    CheckContiguousFill(int8_t{-1});
    CheckContiguousFill(int8_t{5});
    CheckContiguousFill(int8_t{-128});
    CheckContiguousFill(int8_t{127});
    CheckContiguousFill(int16_t{0});
    CheckContiguousFill(int16_t{-3});
    CheckContiguousFill(int32_t{0});
    CheckContiguousFill(int32_t{-3});
    CheckContiguousFill(int64_t{0});
    CheckContiguousFill(int64_t{-3});
    CheckContiguousFill(uint8_t{0});
    CheckContiguousFill(uint8_t{255});
    CheckContiguousFill(float{0});
    CheckContiguousFill(float{std::numeric_limits<float>::infinity()});
    CheckContiguousFill(float{std::nanf("")});
    CheckContiguousFill(double{0});
    CheckContiguousFill(double{std::numeric_limits<double>::infinity()});
    CheckContiguousFill(double{std::nan("")});

    CheckContiguousFill(true, Scalar(int32_t{1}));
    CheckContiguousFill(true, Scalar(int32_t{2}));
    CheckContiguousFill(true, Scalar(int32_t{-1}));
    CheckContiguousFill(false, Scalar(int32_t{0}));
    CheckContiguousFill(int8_t{1}, Scalar(int32_t{1}));
    CheckContiguousFill(int8_t{1}, Scalar(int64_t{1}));
    CheckContiguousFill(int8_t{1}, Scalar(uint8_t{1}));
    CheckContiguousFill(int8_t{1}, Scalar(true));
    CheckContiguousFill(int8_t{1}, Scalar(1.0f));
    CheckContiguousFill(int8_t{1}, Scalar(1.0));
    CheckContiguousFill(int16_t{1}, Scalar(int32_t{1}));
    CheckContiguousFill(int16_t{1}, Scalar(int64_t{1}));
    CheckContiguousFill(int16_t{1}, Scalar(uint8_t{1}));
    CheckContiguousFill(int16_t{1}, Scalar(true));
    CheckContiguousFill(int16_t{1}, Scalar(1.0f));
    CheckContiguousFill(int16_t{1}, Scalar(1.0));
    CheckContiguousFill(int32_t{1}, Scalar(int32_t{1}));
    CheckContiguousFill(int32_t{1}, Scalar(int64_t{1}));
    CheckContiguousFill(int32_t{1}, Scalar(uint8_t{1}));
    CheckContiguousFill(int32_t{1}, Scalar(true));
    CheckContiguousFill(int32_t{1}, Scalar(1.0f));
    CheckContiguousFill(int32_t{1}, Scalar(1.0));
    CheckContiguousFill(int64_t{1}, Scalar(int32_t{1}));
    CheckContiguousFill(int64_t{1}, Scalar(int64_t{1}));
    CheckContiguousFill(int64_t{1}, Scalar(uint8_t{1}));
    CheckContiguousFill(int64_t{1}, Scalar(true));
    CheckContiguousFill(int64_t{1}, Scalar(1.0f));
    CheckContiguousFill(int64_t{1}, Scalar(1.0));
    CheckContiguousFill(uint8_t{1}, Scalar(int32_t{1}));
    CheckContiguousFill(uint8_t{1}, Scalar(int64_t{1}));
    CheckContiguousFill(uint8_t{1}, Scalar(uint8_t{1}));
    CheckContiguousFill(uint8_t{1}, Scalar(true));
    CheckContiguousFill(uint8_t{1}, Scalar(1.0f));
    CheckContiguousFill(uint8_t{1}, Scalar(1.0));
    CheckContiguousFill(float{1}, Scalar(int32_t{1}));
    CheckContiguousFill(float{1}, Scalar(int64_t{1}));
    CheckContiguousFill(float{1}, Scalar(uint8_t{1}));
    CheckContiguousFill(float{1}, Scalar(true));
    CheckContiguousFill(float{1}, Scalar(1.0f));
    CheckContiguousFill(float{1}, Scalar(1.0));
    CheckContiguousFill(double{1}, Scalar(int32_t{1}));
    CheckContiguousFill(double{1}, Scalar(int64_t{1}));
    CheckContiguousFill(double{1}, Scalar(uint8_t{1}));
    CheckContiguousFill(double{1}, Scalar(true));
    CheckContiguousFill(double{1}, Scalar(1.0f));
    CheckContiguousFill(double{1}, Scalar(1.0));
}

TEST_P(ArrayTest, NonContiguousFill) {
    Dtype dtype = Dtype::kFloat32;
    float value = 1.0f;
    {
        Array a = Zeros(Shape{3, 3}, dtype);
        Array b = a.Transpose();
        b.Fill(value);
        testing::ExpectDataEqual(value, b);
        testing::ExpectDataEqual(value, a);
    }
    {
        Array a = Zeros(Shape{3, 3}, dtype);
        a.At({1}).Fill(value);
        testing::ExpectDataEqual(value, a.At({1}));
        // check other rows are not affected
        testing::ExpectDataEqual(0.0f, a.At({0}));
        testing::ExpectDataEqual(0.0f, a.At({2}));
    }
    {
        Array a = Zeros(Shape{3, 3}, dtype);
        a.At({Slice{}, {1}}).Fill(value);
        testing::ExpectDataEqual(value, a.At({Slice{}, {1}}));
        // check other columns are not affected
        testing::ExpectDataEqual(0.0f, a.At({Slice{}, {0}}));
        testing::ExpectDataEqual(0.0f, a.At({Slice{}, {2}}));
    }
}

TEST_P(ArrayTest, Negative) {
    Array a = testing::BuildArray({3}).WithData<float>({-1, 0, 2});
    Array e = testing::BuildArray({3}).WithData<float>({1, 0, -2});
    Array b = -a;
    testing::ExpectEqual(e, b);
}

TEST_P(ArrayTest, Equality) {
    using T = int32_t;
    Array a = testing::BuildArray({2, 3}).WithData<T>({1, 2, 3, 4, 3, 2});
    Array b = testing::BuildArray({2, 1}).WithData<T>({1, 2});
    Array e = testing::BuildArray({2, 3}).WithData<bool>({true, false, false, false, false, true});
    Array c = a == b;

    ASSERT_EQ(c.dtype(), Dtype::kBool);
    testing::ExpectEqual(e, c);
}

TEST_P(ArrayTest, IAdd) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array b = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array e = testing::BuildArray({3, 1}).WithData<float>({2, 4, 6});
    a += b;
    testing::ExpectEqual(e, a);
}

TEST_P(ArrayTest, IAddScalar) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array e = testing::BuildArray({3, 1}).WithData<float>({3, 4, 5});
    a += Scalar{2.f};
    testing::ExpectEqual(e, a);
}

TEST_P(ArrayTest, ISubtract) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array b = testing::BuildArray({3, 1}).WithData<float>({4, 0, -2});
    Array e = testing::BuildArray({3, 1}).WithData<float>({-3, 2, 5});
    a -= b;
    testing::ExpectEqual(e, a);
}

TEST_P(ArrayTest, ISubtractScalar) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1.f, 2.f, 3.f});
    Array e = testing::BuildArray({3, 1}).WithData<float>({0.5f, 1.5f, 2.5f});
    a -= Scalar{0.5f};
    testing::ExpectEqual(e, a);
}

TEST_P(ArrayTest, IMultiply) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array b = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array e = testing::BuildArray({3, 1}).WithData<float>({1, 4, 9});
    a *= b;
    testing::ExpectEqual(e, a);
}

TEST_P(ArrayTest, IMultiplyScalar) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array e = testing::BuildArray({3, 1}).WithData<float>({2, 4, 6});
    a *= Scalar{2.f};
    testing::ExpectEqual(e, a);
}

TEST_P(ArrayTest, IDivide) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1.f, 2.f, 3.f});
    Array b = testing::BuildArray({3, 1}).WithData<float>({1.f, 0.5f, 2.f});
    Array e = testing::BuildArray({3, 1}).WithData<float>({1.f, 4.f, 1.5f});
    a /= b;
    testing::ExpectEqual(e, a);
}

TEST_P(ArrayTest, IDivideScalar) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1.f, 2.f, 3.f});
    Array e = testing::BuildArray({3, 1}).WithData<float>({0.5f, 1.f, 1.5f});
    a /= Scalar{2.f};
    testing::ExpectEqual(e, a);
}

TEST_P(ArrayTest, Add) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array b = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array e = testing::BuildArray({3, 1}).WithData<float>({2, 4, 6});
    Array o = a + b;
    testing::ExpectEqual(e, o);
}

TEST_P(ArrayTest, AddScalar) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Scalar b{2.f};
    Array e = testing::BuildArray({3, 1}).WithData<float>({3, 4, 5});
    {
        Array o = a + b;
        testing::ExpectEqual(e, o);
    }
    {
        Array o = b + a;
        testing::ExpectEqual(e, o);
    }
}

TEST_P(ArrayTest, Subtract) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array b = testing::BuildArray({3, 1}).WithData<float>({4, 0, -2});
    Array e = testing::BuildArray({3, 1}).WithData<float>({-3, 2, 5});
    Array o = a - b;
    testing::ExpectEqual(e, o);
}

TEST_P(ArrayTest, SubtractScalar) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Scalar b{1.5f};
    {
        Array e = testing::BuildArray({3, 1}).WithData<float>({-0.5f, 0.5f, 1.5f});
        Array o = a - b;
        testing::ExpectEqual(e, o);
    }
    {
        Array e = testing::BuildArray({3, 1}).WithData<float>({0.5f, -0.5f, -1.5f});
        Array o = b - a;
        testing::ExpectEqual(e, o);
    }
}

TEST_P(ArrayTest, Multiply) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array b = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array e = testing::BuildArray({3, 1}).WithData<float>({1, 4, 9});
    Array o = a * b;
    testing::ExpectEqual(e, o);
}

TEST_P(ArrayTest, MultiplyScalar) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Scalar b{2.f};
    Array e = testing::BuildArray({3, 1}).WithData<float>({2, 4, 6});
    {
        Array o = a * b;
        testing::ExpectEqual(e, o);
    }
    {
        Array o = b * a;
        testing::ExpectEqual(e, o);
    }
}

TEST_P(ArrayTest, Divide) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1.f, 2.f, 3.f});
    Array b = testing::BuildArray({3, 1}).WithData<float>({0.5f, 0.5f, 2.f});
    Array e = testing::BuildArray({3, 1}).WithData<float>({2.f, 4.f, 1.5f});
    Array o = a / b;
    testing::ExpectEqual(e, o);
}

TEST_P(ArrayTest, DivideScalar) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array e = testing::BuildArray({3, 1}).WithData<float>({0.5f, 1.f, 1.5f});
    Array o = a / Scalar{2.f};
    testing::ExpectEqual(e, o);
}

TEST_P(ArrayTest, ComputationalGraph) {
    // c = a + b
    // o = a * c
    Array a = testing::BuildArray({2, 1}).WithData<float>({2.f, 3.f});
    Array b = testing::BuildArray({2, 1}).WithData<float>({5.f, 1.f});

    BackpropScope backprop_scope{"bp1"};
    BackpropId backprop_id = backprop_scope.backprop_id();
    a.RequireGrad(backprop_id);
    b.RequireGrad(backprop_id);

    {
        auto a_array_node = internal::GetArrayBody(a)->GetArrayNode(backprop_id);
        auto b_array_node = internal::GetArrayBody(b)->GetArrayNode(backprop_id);
        EXPECT_NE(a_array_node, nullptr);
        EXPECT_NE(b_array_node, nullptr);
        auto a_op_node = a_array_node->creator_op_node();
        auto b_op_node = b_array_node->creator_op_node();
        EXPECT_EQ(a_op_node, nullptr);
        EXPECT_EQ(b_op_node, nullptr);
    }

    Array c = a + b;
    {
        auto a_array_node = internal::GetArrayBody(a)->GetArrayNode(backprop_id);
        auto b_array_node = internal::GetArrayBody(b)->GetArrayNode(backprop_id);
        auto c_array_node = internal::GetArrayBody(c)->GetArrayNode(backprop_id);
        EXPECT_NE(a_array_node, nullptr);
        EXPECT_NE(b_array_node, nullptr);
        EXPECT_NE(c_array_node, nullptr);
        auto a_op_node = a_array_node->creator_op_node();
        auto b_op_node = b_array_node->creator_op_node();
        auto c_op_node = c_array_node->creator_op_node();
        EXPECT_EQ(a_op_node, nullptr);
        EXPECT_EQ(b_op_node, nullptr);
        EXPECT_NE(c_op_node, nullptr);
        EXPECT_EQ(c_op_node->name(), "add");
    }

    Array o = a * c;
    {
        auto a_array_node = internal::GetArrayBody(a)->GetArrayNode(backprop_id);
        auto b_array_node = internal::GetArrayBody(b)->GetArrayNode(backprop_id);
        auto c_array_node = internal::GetArrayBody(c)->GetArrayNode(backprop_id);
        auto o_array_node = internal::GetArrayBody(o)->GetArrayNode(backprop_id);
        EXPECT_NE(a_array_node, nullptr);
        EXPECT_NE(b_array_node, nullptr);
        EXPECT_NE(c_array_node, nullptr);
        EXPECT_NE(o_array_node, nullptr);
        auto a_op_node = a_array_node->creator_op_node();
        auto b_op_node = b_array_node->creator_op_node();
        auto c_op_node = c_array_node->creator_op_node();
        auto o_op_node = o_array_node->creator_op_node();
        EXPECT_EQ(a_op_node, nullptr);
        EXPECT_EQ(b_op_node, nullptr);
        EXPECT_NE(c_op_node, nullptr);
        EXPECT_NE(o_op_node, nullptr);
        EXPECT_EQ(c_op_node->name(), "add");
        EXPECT_EQ(o_op_node->name(), "multiply");
    }
}

TEST_P(ArrayTest, InplaceWithArrayNodes) {
    BackpropScope backprop_scope{"bp1"};
    BackpropId backprop_id = backprop_scope.backprop_id();

    // Both input/output arrays have nodes
    {
        Array x = testing::BuildArray({4, 1}).WithLinearData<float>();
        Array y = testing::BuildArray({4, 1}).WithLinearData<float>();
        x.RequireGrad(backprop_id);
        y.RequireGrad(backprop_id);
        EXPECT_THROW({ y += x; }, XchainerError);
    }

    {
        Array x = testing::BuildArray({4, 1}).WithLinearData<float>();
        Array y = testing::BuildArray({4, 1}).WithLinearData<float>();
        x.RequireGrad(backprop_id);
        y.RequireGrad(backprop_id);
        EXPECT_THROW({ y *= x; }, XchainerError);
    }

    // Only output array has nodes
    {
        Array x = testing::BuildArray({4, 1}).WithLinearData<float>();
        Array y = testing::BuildArray({4, 1}).WithLinearData<float>();
        y.RequireGrad(backprop_id);
        EXPECT_THROW({ y *= x; }, XchainerError);
    }

    // Only input array has nodes
    {
        Array x = testing::BuildArray({4, 1}).WithLinearData<float>();
        Array y = testing::BuildArray({4, 1}).WithLinearData<float>();
        x.RequireGrad(backprop_id);
        EXPECT_THROW({ y *= x; }, XchainerError);
    }

    // Only output arrays has nodes, with no backprop scope
    {
        Array x = testing::BuildArray({4, 1}).WithLinearData<float>();
        Array y = testing::BuildArray({4, 1}).WithLinearData<float>();
        y.RequireGrad(backprop_id);

        NoBackpropModeScope scope{backprop_id};
        EXPECT_THROW({ y *= x; }, XchainerError);
    }

    // Only input arrays has nodes, with no backprop scope
    {
        Array x = testing::BuildArray({4, 1}).WithLinearData<float>();
        Array y = testing::BuildArray({4, 1}).WithLinearData<float>();
        x.RequireGrad(backprop_id);

        NoBackpropModeScope scope{backprop_id};
        y *= x;  // no throw
    }
}

TEST_P(ArrayTest, Transpose) {
    Array a = testing::BuildArray({2, 3}).WithLinearData<int32_t>().WithPadding(0);
    Array b = a.Transpose();

    EXPECT_EQ(Shape({3, 2}), b.shape());
    EXPECT_EQ(Strides({4, 12}), b.strides());

    Array e = testing::BuildArray({3, 2}).WithData<int32_t>({0, 3, 1, 4, 2, 5});
    testing::ExpectEqual(e, b);
}

TEST_P(ArrayTest, Copy) {
    using T = int32_t;
    Array a = testing::BuildArray({3, 1}).WithData<T>({1, 2, 3});
    Array o = a.Copy();
    testing::ExpectEqualCopy(a, o);
}

TEST_P(ArrayTest, MakeView) {
    Array a = testing::BuildArray({4, 1}).WithData<bool>({true, true, false, false});
    Array o = a.MakeView();
    testing::ExpectEqualView(a, o);
}

TEST_P(ArrayTest, MakeViewBackward) {
    using T = double;
    Array x = (*testing::BuildArray({3, 2}).WithLinearData<T>()).RequireGrad();
    Array gy = testing::BuildArray({3, 2}).WithLinearData<T>(2.0, -0.5);
    Array eps = FullLike(x, 1e-3);
    CheckBackward([](const std::vector<Array>& xs) -> std::vector<Array> { return {xs[0].MakeView()}; }, {x}, {gy}, {eps});
}

TEST_P(ArrayTest, MakeViewDoubleBackward) {
    using T = double;
    Array x = (*testing::BuildArray({3, 2}).WithLinearData<T>()).RequireGrad();
    Array gy = (*testing::BuildArray({3, 2}).WithLinearData<T>(2.0, -0.5)).RequireGrad();
    Array ggx = testing::BuildArray({3, 2}).WithLinearData<T>(-1.0, 0.3);
    Array eps = FullLike(x, 1e-3);
    CheckDoubleBackwardComputation(
            [](const std::vector<Array>& xs) -> std::vector<Array> {
                Array y = xs[0].MakeView();
                return {y * y};
            },
            {x},
            {gy},
            {ggx},
            {eps, eps});
}

TEST_P(ArrayTest, IsBackpropRequired) {
    BackpropScope backprop_scope{"bp1"};
    BackpropId backprop_id = backprop_scope.backprop_id();

    Array a = testing::BuildArray({2, 1}).WithLinearData<float>();
    a.RequireGrad(backprop_id);
    EXPECT_TRUE(a.IsBackpropRequired(AnyGraph{}));
}

TEST_P(ArrayTest, AsGradStoppedCopy) {
    // Stop gradients on all graphs
    {
        BackpropScope backprop_scope1{"bp1"};
        BackpropScope backprop_scope2{"bp2"};
        BackpropId backprop_id1 = backprop_scope1.backprop_id();
        BackpropId backprop_id2 = backprop_scope2.backprop_id();

        Array a = testing::BuildArray({4, 1}).WithLinearData<float>();
        a.RequireGrad(backprop_id1);
        a.RequireGrad(backprop_id2);
        ASSERT_TRUE(a.IsBackpropRequired(backprop_id1));
        ASSERT_TRUE(a.IsBackpropRequired(backprop_id2));
        Array b = a.AsGradStopped(CopyKind::kCopy);

        EXPECT_EQ(&b.device(), &a.device());

        testing::ExpectEqualCopy(a, b);
        EXPECT_FALSE(b.IsBackpropRequired(backprop_id1));
        EXPECT_FALSE(b.IsBackpropRequired(backprop_id2));

        EXPECT_TRUE(a.IsBackpropRequired(backprop_id1));
        EXPECT_TRUE(a.IsBackpropRequired(backprop_id2));
    }

    // Stop gradients on graphs
    {
        BackpropScope backprop_scope1{"bp1"};
        BackpropScope backprop_scope2{"bp2"};
        BackpropScope backprop_scope3{"bp3"};
        BackpropId backprop_id1 = backprop_scope1.backprop_id();
        BackpropId backprop_id2 = backprop_scope2.backprop_id();
        BackpropId backprop_id3 = backprop_scope3.backprop_id();

        Array a = testing::BuildArray({4, 1}).WithLinearData<float>();
        a.RequireGrad(backprop_id1);
        a.RequireGrad(backprop_id2);
        a.RequireGrad(backprop_id3);
        ASSERT_TRUE(a.IsBackpropRequired(backprop_id1));
        ASSERT_TRUE(a.IsBackpropRequired(backprop_id2));
        ASSERT_TRUE(a.IsBackpropRequired(backprop_id3));
        Array b = a.AsGradStopped({backprop_id1, backprop_id2}, CopyKind::kCopy);

        EXPECT_EQ(&b.device(), &a.device());

        testing::ExpectEqualCopy(a, b);
        EXPECT_FALSE(b.IsBackpropRequired(backprop_id1));
        EXPECT_FALSE(b.IsBackpropRequired(backprop_id2));
        EXPECT_TRUE(b.IsBackpropRequired(backprop_id3));

        EXPECT_TRUE(a.IsBackpropRequired(backprop_id1));
        EXPECT_TRUE(a.IsBackpropRequired(backprop_id2));
        EXPECT_TRUE(a.IsBackpropRequired(backprop_id3));
    }

    // Non-contiguous
    {
        Array a = testing::BuildArray({4, 1}).WithLinearData<float>().WithPadding(4);
        Array b = a.AsGradStopped(CopyKind::kCopy);
        EXPECT_EQ(&b.device(), &a.device());
        testing::ExpectEqualCopy(a, b);
    }
}

TEST_P(ArrayTest, AsGradStoppedView) {
    // Stop gradients on all graphs
    {
        BackpropScope backprop_scope1{"bp1"};
        BackpropScope backprop_scope2{"bp2"};
        BackpropId backprop_id1 = backprop_scope1.backprop_id();
        BackpropId backprop_id2 = backprop_scope2.backprop_id();

        Array a = testing::BuildArray({4, 1}).WithLinearData<float>();
        a.RequireGrad(backprop_id1);
        a.RequireGrad(backprop_id2);
        ASSERT_TRUE(a.IsBackpropRequired(backprop_id1));
        ASSERT_TRUE(a.IsBackpropRequired(backprop_id2));
        Array b = a.AsGradStopped();

        testing::ExpectEqualView(a, b);
        EXPECT_FALSE(b.IsBackpropRequired(backprop_id1));
        EXPECT_FALSE(b.IsBackpropRequired(backprop_id2));

        EXPECT_TRUE(a.IsBackpropRequired(backprop_id1));
        EXPECT_TRUE(a.IsBackpropRequired(backprop_id2));
    }

    // Stop gradients on some graphs
    {
        BackpropScope backprop_scope1{"bp1"};
        BackpropScope backprop_scope2{"bp2"};
        BackpropScope backprop_scope3{"bp3"};
        BackpropId backprop_id1 = backprop_scope1.backprop_id();
        BackpropId backprop_id2 = backprop_scope2.backprop_id();
        BackpropId backprop_id3 = backprop_scope3.backprop_id();

        Array a = testing::BuildArray({4, 1}).WithLinearData<float>();
        a.RequireGrad(backprop_id1);
        a.RequireGrad(backprop_id2);
        a.RequireGrad(backprop_id3);
        ASSERT_TRUE(a.IsBackpropRequired(backprop_id1));
        ASSERT_TRUE(a.IsBackpropRequired(backprop_id2));
        ASSERT_TRUE(a.IsBackpropRequired(backprop_id3));
        Array b = a.AsGradStopped({backprop_id1, backprop_id2});

        testing::ExpectEqualView(a, b);
        EXPECT_FALSE(b.IsBackpropRequired(backprop_id1));
        EXPECT_FALSE(b.IsBackpropRequired(backprop_id2));
        EXPECT_TRUE(b.IsBackpropRequired(backprop_id3));

        EXPECT_TRUE(a.IsBackpropRequired(backprop_id1));
        EXPECT_TRUE(a.IsBackpropRequired(backprop_id2));
        EXPECT_TRUE(a.IsBackpropRequired(backprop_id3));
    }
    // Non-contiguous
    {
        Array a = testing::BuildArray({4, 1}).WithLinearData<float>().WithPadding(4);
        Array b = a.AsGradStopped(CopyKind::kView);
        EXPECT_EQ(&b.device(), &a.device());
        testing::ExpectEqualView(a, b);
    }
}

TEST_P(ArrayTest, AsTypeFloatToDouble) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array o = a.AsType(Dtype::kFloat64);
    Array e = testing::BuildArray({3, 1}).WithData<double>({1, 2, 3});
    testing::ExpectEqualCopy(e, o);
}

TEST_P(ArrayTest, AsTypeFloatToInt) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array o = a.AsType(Dtype::kInt32);
    Array e = testing::BuildArray({3, 1}).WithData<int32_t>({1, 2, 3});
    testing::ExpectEqualCopy(e, o);
}

TEST_P(ArrayTest, AsTypeBoolToFloat) {
    Array a = testing::BuildArray({3, 1}).WithData<bool>({true, false, true});
    Array o = a.AsType(Dtype::kFloat32);
    Array e = testing::BuildArray({3, 1}).WithData<float>({1.0, 0.0, 1.0});
    testing::ExpectEqualCopy(e, o);
}

TEST_P(ArrayTest, AsTypeCopyFalse) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array o = a.AsType(Dtype::kFloat32, false);
    EXPECT_EQ(internal::GetArrayBody(a), internal::GetArrayBody(o))
            << "Bodies must be same in order for the reference to be preserved in Python";
}

TEST_P(ArrayTest, AsTypeCopyFalseButDifferentType) {
    Array a = testing::BuildArray({3, 1}).WithData<float>({1, 2, 3});
    Array o = a.AsType(Dtype::kFloat64, false);
    Array e = testing::BuildArray({3, 1}).WithData<double>({1, 2, 3});
    testing::ExpectEqualCopy(e, o);
}

TEST_P(ArrayTest, AsTypeBackward) {
    using InT = float;
    using OutT = double;
    Shape shape{2, 3};

    Array a = (*testing::BuildArray(shape).WithLinearData<InT>(-3).WithPadding(1)).RequireGrad();
    Array go = testing::BuildArray(shape).WithLinearData<OutT>(-0.1, 0.1).WithPadding(1);
    Array eps = Full(shape, 1e-3f);

    CheckBackward([](const std::vector<Array>& xs) -> std::vector<Array> { return {xs[0].AsType(TypeToDtype<OutT>)}; }, {a}, {go}, {eps});
}

TEST_P(ArrayTest, AsTypeToNonFloatNoGraph) {
    Array a = (*testing::BuildArray({2, 3}).WithLinearData<float>(-3).WithPadding(1)).RequireGrad();
    EXPECT_FALSE(a.AsType(Dtype::kInt32).IsBackpropRequired());
    EXPECT_FALSE(a.AsType(Dtype::kBool).IsBackpropRequired());
}

TEST_P(ArrayTest, AsTypeDoubleBackward) {
    using InT = float;
    using OutT = double;
    Shape shape{2, 3};

    Array a = (*testing::BuildArray(shape).WithLinearData<InT>(-3).WithPadding(1)).RequireGrad();
    Array go = (*testing::BuildArray(shape).WithLinearData<OutT>(-0.1, 0.1).WithPadding(1)).RequireGrad();
    Array ggi = testing::BuildArray(shape).WithLinearData<InT>(-0.1, 0.1).WithPadding(1);
    Array a_eps = Full(shape, 1e-3f);
    Array go_eps = Full(shape, 1e-3);

    CheckDoubleBackwardComputation(
            [](const std::vector<Array>& xs) -> std::vector<Array> {
                auto y = xs[0].AsType(TypeToDtype<OutT>);
                return {y * y};  // to make it nonlinear
            },
            {a},
            {go},
            {ggi},
            {a_eps, go_eps});
}

TEST_P(ArrayTest, ToNative) {
    using T = float;
    Array a = (*testing::BuildArray({2, 3}).WithLinearData<T>().WithPadding(1)).RequireGrad();

    Array b = a.ToNative();
    EXPECT_EQ("native:0", b.device().name());
    EXPECT_EQ(&a.device().backend().context(), &b.device().backend().context());
    EXPECT_NE(internal::GetArrayBody(a), internal::GetArrayBody(b));

    EXPECT_EQ(a.dtype(), b.dtype());
    EXPECT_EQ(a.shape(), b.shape());
    testing::ExpectEqual(a.ToNative(), b.ToNative());

    if (a.device().name() == "native:0") {
        // Between the same device
        EXPECT_EQ(&a.device(), &b.device());
        EXPECT_EQ(a.data().get(), b.data().get());
        EXPECT_EQ(a.strides(), b.strides());
    } else {
        // Between different devices
        EXPECT_NE(&a.data(), &b.data());
        EXPECT_TRUE(b.IsContiguous());
    }

    // Graph
    ASSERT_TRUE(!a.GetGrad().has_value());
    Backward(b);
    ASSERT_TRUE(a.GetGrad().has_value());
    EXPECT_EQ(&a.device(), &a.GetGrad()->device());
}

TEST_P(ArrayTest, MultipleGraphsRequireGradDefault) {
    Array a = testing::BuildArray({1}).WithData<float>({2.0f});

    EXPECT_FALSE(a.IsBackpropRequired());

    a.RequireGrad();
    EXPECT_TRUE(a.IsBackpropRequired());

    a.RequireGrad();
    EXPECT_TRUE(a.IsBackpropRequired());
}

TEST_P(ArrayTest, MultipleGraphsRequireGradNamed) {
    BackpropScope backprop_scope{"bp1"};
    BackpropId backprop_id = backprop_scope.backprop_id();

    Array a = testing::BuildArray({1}).WithData<float>({2.0f});

    ASSERT_FALSE(a.IsBackpropRequired(backprop_id));

    a.RequireGrad(backprop_id);
    EXPECT_TRUE(a.IsBackpropRequired(backprop_id));

    a.RequireGrad(backprop_id);
    EXPECT_TRUE(a.IsBackpropRequired(backprop_id));
}

TEST_P(ArrayTest, MultipleGraphsRequireGradChainedCallsCtor) {
    Array a = (*testing::BuildArray({1}).WithData<float>({2.0f})).RequireGrad();

    EXPECT_TRUE(a.IsBackpropRequired());

    a.RequireGrad();
    EXPECT_TRUE(a.IsBackpropRequired());
}

TEST_P(ArrayTest, MultipleGraphsRequireGradChainedCallsRequireGrad) {
    Array a = testing::BuildArray({1}).WithData<float>({2.0f});

    a.RequireGrad().RequireGrad();
    EXPECT_TRUE(a.IsBackpropRequired());
}

TEST_P(ArrayTest, MultipleGraphsForward) {
    BackpropScope backprop_scope1{"bp1"};
    BackpropScope backprop_scope2{"bp2"};
    BackpropScope backprop_scope3{"bp3"};
    BackpropId backprop_id1 = backprop_scope1.backprop_id();
    BackpropId backprop_id2 = backprop_scope2.backprop_id();
    BackpropId backprop_id3 = backprop_scope3.backprop_id();

    Array a = testing::BuildArray({1}).WithData<float>({2.0f});
    Array b = testing::BuildArray({1}).WithData<float>({2.0f});

    a.RequireGrad(backprop_id1);
    b.RequireGrad(backprop_id2);

    EXPECT_TRUE(a.IsBackpropRequired(backprop_id1));
    EXPECT_FALSE(a.IsBackpropRequired(backprop_id2));

    EXPECT_FALSE(b.IsBackpropRequired(backprop_id1));
    EXPECT_TRUE(b.IsBackpropRequired(backprop_id2));

    Array o = a * b;

    EXPECT_TRUE(o.IsBackpropRequired(backprop_id1));
    EXPECT_TRUE(o.IsBackpropRequired(backprop_id2));

    // No unspecified or previously unused graphs are generated
    EXPECT_FALSE(o.IsBackpropRequired());
    EXPECT_FALSE(o.IsBackpropRequired(backprop_id3));
}

TEST_P(ArrayTest, RequireGradWithBackpropModeScope) {
    Array a = testing::BuildArray({1}).WithData<float>({2.0f});
    {
        NoBackpropModeScope scope{};
        a.RequireGrad();
    }
    EXPECT_FALSE(a.IsBackpropRequired());
    {
        ForceBackpropModeScope scope{};
        a.RequireGrad();
    }
    EXPECT_TRUE(a.IsBackpropRequired());
}

TEST_P(ArrayTest, Take) {
    using T = int8_t;
    Shape input_shape{2, 4};
    Shape indices_shape{2, 3};
    Shape output_shape{2, 2, 3};
    Array a = testing::BuildArray(input_shape).WithLinearData<T>().WithPadding(1);
    Array indices = testing::BuildArray(indices_shape).WithData<int64_t>({0, 14, 3, 1, -10, 1});
    Array b = a.Take(indices, 1);

    EXPECT_EQ(output_shape, b.shape());
    Array e = testing::BuildArray(output_shape).WithData<T>({0, 2, 3, 1, 2, 1, 4, 6, 7, 5, 6, 5});
    testing::ExpectEqual(e, b);
}

INSTANTIATE_TEST_CASE_P(
        ForEachBackend,
        ArrayTest,
        ::testing::Values(
#ifdef XCHAINER_ENABLE_CUDA
                std::string{"cuda"},
#endif  // XCHAINER_ENABLE_CUDA
                std::string{"native"}));

TEST(ArrayGradTest, ClearGradThrow) {
    testing::ContextSession context_session{};
    BackpropScope backprop_scope1{"bp1"};
    BackpropScope backprop_scope2{"bp2"};
    BackpropId backprop_id1 = backprop_scope1.backprop_id();
    BackpropId backprop_id2 = backprop_scope2.backprop_id();

    Array x = testing::BuildArray({2, 1}).WithLinearData<float>();

    EXPECT_THROW(x.ClearGrad(), XchainerError);
    EXPECT_THROW(x.ClearGrad(backprop_id1), XchainerError);

    x.RequireGrad(backprop_id1);

    EXPECT_THROW(x.ClearGrad(), XchainerError);
    EXPECT_THROW(x.ClearGrad(backprop_id2), XchainerError);
    x.ClearGrad(backprop_id1);  // no throw
}

TEST(ArrayAtTest, At) {
    using T = int32_t;
    testing::ContextSession context_session{};
    Shape input_shape{2, 3, 1};
    Shape output_shape{1, 2, 1};
    std::vector<ArrayIndex> indices{-1, NewAxis{}, Slice{1, 3}};
    Array a = testing::BuildArray(input_shape).WithLinearData<T>();
    Array b = a.At(indices);

    EXPECT_EQ(output_shape, b.shape());
    Array e = testing::BuildArray(output_shape).WithData<T>({4, 5});
    testing::ExpectEqual(e, b);

    // Check if strides are 0 for newaxis.
    EXPECT_EQ(0, b.strides()[0]);
    EXPECT_NE(0, b.strides()[1]);
    EXPECT_NE(0, b.strides()[2]);
}

TEST(ArrayReshapeTest, Reshape) {
    using T = int32_t;
    testing::ContextSession context_session{};
    Shape input_shape{2, 3, 4};
    Shape output_shape{3, 4, 2};

    Array a = testing::BuildArray(input_shape).WithLinearData<T>();
    Array b = a.Reshape(output_shape);
    ASSERT_EQ(output_shape, b.shape());
    EXPECT_EQ(a.data().get(), b.data().get()) << "Reshape must be done without copying data";
    Array e = testing::BuildArray(output_shape).WithLinearData<T>();
    testing::ExpectEqual(e, b);
}

TEST(ArraySqueezeTest, SqueezeSpecifiedUnitLenghtAxes) {
    using T = int32_t;
    testing::ContextSession context_session{};

    Array a = testing::BuildArray({1, 2, 1, 3, 1, 1, 4}).WithLinearData<T>();
    Array b = a.Squeeze(Axes{2, 0, 4});
    Array e = testing::BuildArray({2, 3, 1, 4}).WithLinearData<T>();
    testing::ExpectEqual(e, b);
}

TEST(ArraySqueezeTest, SqueezeAllAxes) {
    using T = int32_t;
    testing::ContextSession context_session{};

    Array a = testing::BuildArray({1, 1, 1}).WithLinearData<T>();
    Array b = a.Squeeze();
    Array e = testing::BuildArray({}).WithData<T>(std::vector<T>(1, 0));
    testing::ExpectEqual(e, b);
}

TEST(ArrayBroadcastToTest, BroadcastTo) {
    using T = int32_t;
    testing::ContextSession context_session{};
    Shape input_shape{2, 3, 1};
    Shape output_shape{3, 1, 2, 3, 1, 2};

    Array aa = testing::BuildArray(input_shape).WithData<T>({1, 2, 3, 4, 5, 6});
    Array a = aa.At({Slice(), Slice(), Slice(), NewAxis{}});  // Make a broadcastable axis.
    ASSERT_EQ(Shape({2, 3, 1, 1}), a.shape());  // Check test precondition

    Array b = a.BroadcastTo(output_shape);
    ASSERT_EQ(output_shape, b.shape());
    EXPECT_EQ(a.data().get(), b.data().get()) << "BroadcastTo must be done without copying data";
    ASSERT_EQ(0, b.strides()[1]) << "Stride of broadcasted dimension must be 0";

    std::vector<T> output_data;
    for (int i = 0; i < 3; ++i) {
        output_data.insert(output_data.end(), {1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6});
    }
    Array e = testing::BuildArray(output_shape).WithData<T>(output_data);
    testing::ExpectEqual(e, b);
}

TEST(ArrayArgMaxTest, ArgMax) {
    using T = float;
    testing::ContextSession context_session{};

    Array a = testing::BuildArray({2, 3}).WithData<T>({1, 4, 3, 0, 1, 4});
    Array b = a.ArgMax(0);
    Array e = testing::BuildArray({3}).WithData<int64_t>({0, 0, 1});
    testing::ExpectEqual(e, b);
}

TEST(ArrayArgMaxTest, ArgMaxAllAxes) {
    using T = float;
    testing::ContextSession context_session{};

    Array a = testing::BuildArray({2, 3}).WithData<T>({1, 4, 3, 0, 1, 4});
    Array b = a.ArgMax();
    Array e = testing::BuildArray({}).WithData<int64_t>({1});
    testing::ExpectEqual(e, b);
}

TEST(ArraySumTest, Sum) {
    using T = float;
    testing::ContextSession context_session{};

    Array a = testing::BuildArray({2, 3, 4, 3}).WithLinearData<T>().WithPadding(1);
    Array b = a.Sum(Axes{2, 1, -1});
    EXPECT_EQ(Shape{2}, b.shape());
    Array e = testing::BuildArray({2}).WithData<T>({630.0f, 1926.0f});
    testing::ExpectEqual(e, b);
}

TEST(ArraySumTest, SumAllAxes) {
    using T = float;
    testing::ContextSession context_session{};

    Array a = testing::BuildArray({2, 3, 3}).WithLinearData<T>().WithPadding(1);
    Array b = a.Sum();
    EXPECT_EQ(Shape{}, b.shape());
    Array e = testing::BuildArray({}).WithData<T>({153.0f});
    testing::ExpectEqual(e, b);
}

TEST(ArraySumTest, SumKeepDims) {
    using T = float;
    testing::ContextSession context_session{};

    Array a = testing::BuildArray({2, 3, 2, 4}).WithLinearData<T>().WithPadding(1);
    Array b = a.Sum(Axes{-1, 1}, true);
    EXPECT_EQ(Shape({2, 1, 2, 1}), b.shape());
    EXPECT_EQ(0, b.strides()[1]);
    EXPECT_EQ(0, b.strides()[3]);
    Array e = testing::BuildArray({2, 1, 2, 1}).WithData<T>({114.0f, 162.0f, 402.0f, 450.0f});
    testing::ExpectEqual(e, b);
}

TEST(ArrayMaxTest, Max) {
    testing::ContextSession context_session;
    Array a = testing::BuildArray({2, 3, 4, 3}).WithLinearData<float>().WithPadding(1);
    Array b = a.Max(Axes{2, 0, -1});
    EXPECT_EQ(Shape{3}, b.shape());
    Array e = testing::BuildArray({3}).WithData<float>({47.f, 59.f, 71.f});
    testing::ExpectEqual(e, b);
}

TEST(ArrayMaxTest, MaxAllAxes) {
    testing::ContextSession context_session;
    Array a = testing::BuildArray({2, 3, 3}).WithLinearData<float>().WithPadding(1);
    Array b = a.Max();
    EXPECT_EQ(Shape{}, b.shape());
    Array e = testing::BuildArray({}).WithData<float>({17.f});
    testing::ExpectEqual(e, b);
}

TEST(ArrayMaxTest, MaxKeepDims) {
    testing::ContextSession context_session;
    Array a = testing::BuildArray({2, 3, 2, 4}).WithLinearData<float>().WithPadding(1);
    Array b = a.Max(Axes{-1, 1}, true);
    EXPECT_EQ(Shape({2, 1, 2, 1}), b.shape());
    EXPECT_EQ(0, b.strides()[1]);
    EXPECT_EQ(0, b.strides()[3]);
    Array e = testing::BuildArray({2, 1, 2, 1}).WithData<float>({19.f, 23.f, 43.f, 47.f});
    testing::ExpectEqual(e, b);
}

TEST(ArrayDotTest, Dot) {
    using T = float;
    testing::ContextSession context_session{};

    Array a = testing::BuildArray({2, 3}).WithLinearData(1.f).WithPadding(1);
    Array b = testing::BuildArray({3, 2}).WithData<T>({1.f, 2.f, -1.f, -3.f, 2.f, 4.f}).WithPadding(2);
    Array c = a.Dot(b);
    Array e = testing::BuildArray({2, 2}).WithData<T>({5.f, 8.f, 11.f, 17.f});
    testing::ExpectEqual(e, c);
}

}  // namespace
}  // namespace xchainer

/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "TestCommonFunctions.h"
#include "emulation_helper.h"

static const std::string UNARY_FUNCTION = R"(
__kernel void test(__global float16* out, __global float16* in) {
  size_t gid = get_global_id(0);
  out[gid] = FUNC(in[gid]);
}
)";

static const std::string BINARY_FUNCTION = R"(
__kernel void test(__global float16* out, __global float16* in0, __global float16* in1) {
  size_t gid = get_global_id(0);
  out[gid] = FUNC(in0[gid], in1[gid]);
}
)";

static const std::string TERNARY_FUNCTION = R"(
__kernel void test(__global float16* out, __global float16* in0, __global float16* in1, __global float16* in2) {
  size_t gid = get_global_id(0);
  out[gid] = FUNC(in0[gid], in1[gid], in2[gid]);
}
)";

TestCommonFunctions::TestCommonFunctions(const vc4c::Configuration& config) : config(config)
{
    TEST_ADD(TestCommonFunctions::testClamp);
    TEST_ADD(TestCommonFunctions::testDegrees);
    TEST_ADD(TestCommonFunctions::testMax);
    TEST_ADD(TestCommonFunctions::testMin);
    TEST_ADD(TestCommonFunctions::testMix);
    TEST_ADD(TestCommonFunctions::testRadians);
    TEST_ADD(TestCommonFunctions::testStep);
    TEST_ADD(TestCommonFunctions::testSmoothStep);
    TEST_ADD(TestCommonFunctions::testSign);
}

TestCommonFunctions::~TestCommonFunctions() = default;

void TestCommonFunctions::onMismatch(const std::string& expected, const std::string& result)
{
    TEST_ASSERT_EQUALS(expected, result)
}

template <std::size_t ULP>
static void testUnaryFunction(vc4c::Configuration& config, const std::string& options,
    const std::function<float(float)>& op, const std::function<void(const std::string&, const std::string&)>& onError)
{
    std::stringstream code;
    compileBuffer(config, code, UNARY_FUNCTION, options);

    auto in = generateInput<float, 16 * 12>(true);

    auto out = runEmulation<float, float, 16, 12>(code, {in});
    auto pos = options.find("-DFUNC=") + std::string("-DFUNC=").size();
    checkUnaryResults<float, float, 16 * 12, CompareULP<ULP>>(
        in, out, op, options.substr(pos, options.find(' ', pos) - pos), onError);
}

template <std::size_t ULP>
static void testBinaryFunction(vc4c::Configuration& config, const std::string& options,
    const std::function<float(float, float)>& op,
    const std::function<void(const std::string&, const std::string&)>& onError)
{
    std::stringstream code;
    compileBuffer(config, code, BINARY_FUNCTION, options);

    auto in0 = generateInput<float, 16 * 12>(true);
    auto in1 = generateInput<float, 16 * 12>(true);

    auto out = runEmulation<float, float, 16, 12>(code, {in0, in1});
    auto pos = options.find("-DFUNC=") + std::string("-DFUNC=").size();
    checkBinaryResults<float, float, 16 * 12, CompareULP<ULP>>(
        in0, in1, out, op, options.substr(pos, options.find(' ', pos) - pos), onError);
}

template <std::size_t ULP>
static void testTernaryFunction(vc4c::Configuration& config, const std::string& options,
    const std::function<float(float, float, float)>& op,
    const std::function<void(const std::string&, const std::string&)>& onError)
{
    std::stringstream code;
    compileBuffer(config, code, TERNARY_FUNCTION, options);

    auto in0 = generateInput<float, 16 * 12>(true);
    auto in1 = generateInput<float, 16 * 12>(true);
    auto in2 = generateInput<float, 16 * 12>(true);

    auto out = runEmulation<float, float, 16, 12>(code, {in0, in1, in2});
    auto pos = options.find("-DFUNC=") + std::string("-DFUNC=").size();
    checkTernaryResults<float, float, 16 * 12, CompareULP<ULP>>(
        in0, in1, in2, out, op, options.substr(pos, options.find(' ', pos) - pos), onError);
}

static float checkClamp(float x, float min, float max)
{
    return std::min(std::max(x, min), max);
}

void TestCommonFunctions::testClamp()
{
    testTernaryFunction<0>(config, "-DFUNC=clamp", checkClamp,
        std::bind(&TestCommonFunctions::onMismatch, this, std::placeholders::_1, std::placeholders::_2));
}

void TestCommonFunctions::testDegrees()
{
    // maximum error not in OpenCL 1.2 standard, but in latest
    testUnaryFunction<2>(config, "-DFUNC=degrees",
        [](float a) -> float { return a * (180.f / static_cast<float>(M_PI)); },
        std::bind(&TestCommonFunctions::onMismatch, this, std::placeholders::_1, std::placeholders::_2));
}

void TestCommonFunctions::testMax()
{
    // maximum error not in OpenCL 1.2 standard, but in latest
    testBinaryFunction<0>(config, "-DFUNC=max", [](float a, float b) -> float { return std::max(a, b); },
        std::bind(&TestCommonFunctions::onMismatch, this, std::placeholders::_1, std::placeholders::_2));
}

void TestCommonFunctions::testMin()
{
    // maximum error not in OpenCL 1.2 standard, but in latest
    testBinaryFunction<0>(config, "-DFUNC=min", [](float a, float b) -> float { return std::min(a, b); },
        std::bind(&TestCommonFunctions::onMismatch, this, std::placeholders::_1, std::placeholders::_2));
}

void TestCommonFunctions::testMix()
{
    // maximum error not in OpenCL 1.2 standard, but in latest
    // technically implementation defined, but we derive the maximum ULP from the ULPs of the used functions
    testTernaryFunction<0>(config, "-DFUNC=mix", [](float a, float b, float c) -> float { return a + (b - a) * c; },
        std::bind(&TestCommonFunctions::onMismatch, this, std::placeholders::_1, std::placeholders::_2));
}

void TestCommonFunctions::testRadians()
{
    // maximum error not in OpenCL 1.2 standard, but in latest
    testUnaryFunction<2>(config, "-DFUNC=radians",
        [](float a) -> float { return a * (static_cast<float>(M_PI) / 180.f); },
        std::bind(&TestCommonFunctions::onMismatch, this, std::placeholders::_1, std::placeholders::_2));
}

void TestCommonFunctions::testStep()
{
    // maximum error not in OpenCL 1.2 standard, but in latest
    testBinaryFunction<0>(config, "-DFUNC=step", [](float a, float b) -> float { return b < a ? 0.0f : 1.0f; },
        std::bind(&TestCommonFunctions::onMismatch, this, std::placeholders::_1, std::placeholders::_2));
}

void TestCommonFunctions::testSmoothStep()
{
    // maximum error not in OpenCL 1.2 standard, but in latest
    auto smoothstep = [](float edge0, float edge1, float val) -> float {
        float t = checkClamp((val - edge0) / (edge1 - edge0), 0, 1);
        return t * t * (3 - 2 * t);
    };
    // TODO guarantee that edge0 <= edge1? What does the standard say?
    // "Results are undefined if edge0 >= edge1 or if x, edge0 or edge1 is a NaN."
    testTernaryFunction<3>(config, "-DFUNC=smoothstep", smoothstep,
        std::bind(&TestCommonFunctions::onMismatch, this, std::placeholders::_1, std::placeholders::_2));
}

void TestCommonFunctions::testSign()
{
    // maximum error not in OpenCL 1.2 standard, but in latest
    testUnaryFunction<0>(config, "-DFUNC=sign",
        [](float a) -> float { return (std::signbit(a) ? -1.0f : 1.0f) * (std::abs(a) > 0 ? 1.0f : 0.0f); },
        std::bind(&TestCommonFunctions::onMismatch, this, std::placeholders::_1, std::placeholders::_2));
}

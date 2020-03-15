#include "test.h"

using Float = CUDAArray<float>;
using Int32 = CUDAArray<int32_t>;

TEST_CUDA(01_creation_destruction) {
    // Checks simple reference counting of a variable
    Float value(1234);
    (void) value;
}

TEST_CUDA(02_fill_and_print) {
    /// Checks array initialization from a given pointer, jitc_fill(), and stringification
    jitc_log(Info, "  int8_t: %s", CUDAArray<  int8_t>::full(-111, 5).str());
    jitc_log(Info, " uint8_t: %s", CUDAArray< uint8_t>::full( 222, 5).str());
    jitc_log(Info, " int16_t: %s", CUDAArray< int16_t>::full(-1111, 5).str());
    jitc_log(Info, "uint16_t: %s", CUDAArray<uint16_t>::full( 2222, 5).str());
    jitc_log(Info, " int32_t: %s", CUDAArray< int32_t>::full(-1111111111, 5).str());
    jitc_log(Info, "uint32_t: %s", CUDAArray<uint32_t>::full( 2222222222, 5).str());
    jitc_log(Info, " int64_t: %s", CUDAArray< int64_t>::full(-1111111111111111111, 5).str());
    jitc_log(Info, "uint64_t: %s", CUDAArray<uint64_t>::full( 2222222222222222222, 5).str());
    jitc_log(Info, "   float: %s", CUDAArray<   float>::full(1.f/3.f, 5).str());
    jitc_log(Info, "  double: %s", CUDAArray<  double>::full(1.0/3.0, 5).str());
}

TEST_CUDA(03_eval_scalar) {
    /// Checks that we can evaluate a simple kernel
    Float value(1234);
    jitc_log(Info, "value=%s", value.str());
}

TEST_CUDA(04_eval_scalar_csa) {
    /// Checks common subexpression elimination
    Float value_1(1234),
          value_2(1235),
          value_3(1234),
          value_4 = value_1 + value_2,
          value_5 = value_1 + value_3,
          value_6 = value_1 + value_2;
    jitc_eval();
    jitc_log(Info, "value_1=%s", value_1.str());
    jitc_log(Info, "value_2=%s", value_2.str());
    jitc_log(Info, "value_3=%s", value_3.str());
    jitc_log(Info, "value_4=%s", value_4.str());
    jitc_log(Info, "value_5=%s", value_5.str());
    jitc_log(Info, "value_6=%s", value_6.str());
}

TEST_CUDA(05_argument_out) {
    /// Test kernels with very many outputs that exceed the max. size of the CUDA parameter table
    scoped_set_log_level ssll(LogLevel::Info);
    /* With reduced log level */ {
        Int32 value[1024];
        for (int i = 1; i < 1024; i *= 3) {
            Int32 out = 0;
            for (int j = 0; j < i; ++j) {
                value[j] = j;
                out += value[j];
            }
            jitc_log(Info, "value=%s vs %u", out.str(), i * (i - 1) / 2);
        }
    }
}

TEST_CUDA(06_argument_inout) {
    /// Test kernels with very many inputs that exceed the max. size of the CUDA parameter table
    scoped_set_log_level ssll(LogLevel::Info);
    /* With reduced log level */ {
        Int32 value[1024];
        for (int i = 1; i < 1024; i *= 3) {
            Int32 out = 0;
            for (int j = 0; j < i; ++j) {
                if (!value[j].valid())
                    value[j] = j;
                out += value[j];
            }
            jitc_log(Info, "value=%s vs %u", out.str(), i * (i - 1) / 2);
        }
    }
}

/// parallel dispatch
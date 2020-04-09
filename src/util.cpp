#include <enoki/traits.h>
#include "internal.h"
#include "util.h"
#include "var.h"
#include "registry.h"
#include "log.h"
#include "tbb.h"

#if defined(ENOKI_TBB)
#  include <tbb/tbb.h>
#endif

const char *reduction_name[(int) ReductionType::Count] = { "add", "mul", "min",
                                                           "max", "and", "or" };

/// Fill a device memory region with constants of a given type
void jit_memset(void *ptr, uint32_t size_, uint32_t isize, const void *src) {
    Stream *stream = active_stream;

    if (unlikely(!stream))
        jit_raise("jit_memset(): you must invoke jit_device_set() to "
                  "choose a target device before calling this function.");
    else if (isize != 1 && isize != 2 && isize != 4 && isize != 8)
        jit_raise("jit_memset(): invalid element size (must be 1, 2, 4, or 8)!");

    jit_trace("jit_memset(" ENOKI_PTR ", isize=%u, size=%u)",
              (uintptr_t) ptr, isize, size_);

    if (size_ == 0)
        return;

    size_t size = size_;

    // Try to convert into ordinary memset if possible
    uint64_t zero = 0;
    if (memcmp(src, &zero, isize) == 0) {
        size *= isize;
        isize = 1;
    }

    if (stream->cuda) {
        switch (isize) {
            case 1:
                cuda_check(cuMemsetD8Async((CUdeviceptr) ptr,
                                           ((uint8_t *) src)[0], size,
                                           stream->handle));
                break;

            case 2:
                cuda_check(cuMemsetD16Async((CUdeviceptr) ptr,
                                            ((uint16_t *) src)[0], size,
                                            stream->handle));
                break;

            case 4:
                cuda_check(cuMemsetD32Async((CUdeviceptr) ptr,
                                            ((uint32_t *) src)[0], size,
                                            stream->handle));
                break;

            case 8: {
                    const Device &device = state.devices[stream->device];
                    uint32_t block_count, thread_count;
                    device.get_launch_config(&block_count, &thread_count, size);
                    void *args[] = { &ptr, &size_, (void *) src };
                    CUfunction kernel = jit_cuda_fill_64[device.id];
                    cuda_check(cuLaunchKernel(kernel, block_count, 1, 1,
                                              thread_count, 1, 1, 0,
                                              stream->handle, args, nullptr));
                }
                break;
        }
    } else {
        struct Inputs {
            void *ptr;
            size_t size;
            uint32_t isize;
            uint8_t src[8];
        };

        Inputs inputs;
        inputs.ptr = ptr;
        inputs.size = size;
        inputs.isize = isize;
        memcpy(inputs.src, src, isize);

        auto func = [](void *inputs_) {
            Inputs inputs = *((Inputs *) inputs_);
            switch (inputs.isize) {
                case 1:
                    memset(inputs.ptr, inputs.src[0], inputs.size);
                    break;

                case 2: {
                        uint16_t value = ((uint16_t *) inputs.src)[0],
                                 *p    = (uint16_t *) inputs.ptr;
                        for (uint32_t i = 0; i < inputs.size; ++i)
                            p[i] = value;
                    }
                    break;

                case 4: {
                        uint32_t value = ((uint32_t *) inputs.src)[0],
                                 *p    = (uint32_t *) inputs.ptr;
                        for (uint32_t i = 0; i < inputs.size; ++i)
                            p[i] = value;
                    }
                    break;

                case 8: {
                        uint64_t value = ((uint64_t *) inputs.src)[0],
                                 *p    = (uint64_t *) inputs.ptr;
                        for (uint32_t i = 0; i < inputs.size; ++i)
                            p[i] = value;
                    }
                    break;
            }
        };

#if defined(ENOKI_TBB)
        tbb_stream_enqueue_func(stream, func, &inputs, sizeof(Inputs));
#else
        unlock_guard guard(state.mutex);
        func(&inputs);
#endif
    }
}

/// Perform a synchronous copy operation
void jit_memcpy(void *dst, const void *src, size_t size) {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_raise("jit_memcpy(): you must invoke jit_device_set() to choose a "
                  "target device before calling this function.");

    // Temporarily release the lock while copying
    unlock_guard guard(state.mutex);
    if  (stream->cuda) {
        cuda_check(cuStreamSynchronize(stream->handle));
        cuda_check(cuMemcpy((CUdeviceptr) dst, (CUdeviceptr) src, size));
    } else {
#if defined(ENOKI_TBB)
        tbb_stream_sync(stream);
#endif
        memcpy(dst, src, size);
    }
}

/// Perform an assynchronous copy operation
void jit_memcpy_async(void *dst, const void *src, size_t size) {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_raise("jit_memcpy_async(): you must invoke jit_device_set() to "
                  "choose a target device before calling this function.");

    if  (stream->cuda) {
        cuda_check(cuMemcpyAsync((CUdeviceptr) dst, (CUdeviceptr) src, size,
                                 stream->handle));
    } else {
        struct Inputs {
            void *dst;
            const void *src;
            size_t size;
        };

        Inputs inputs { dst, src, size };

        auto func = [](void *inputs_) {
            Inputs inputs = *((Inputs *) inputs_);
            memcpy(inputs.dst, inputs.src, inputs.size);
        };

#if defined(ENOKI_TBB)
        tbb_stream_enqueue_func(stream, func, &inputs, sizeof(Inputs));
#else
        unlock_guard guard(state.mutex);
        func(&inputs);
#endif
    }
}

using Reduction = void (*) (const void *ptr, uint32_t start, uint32_t end, void *out);

template <typename Value>
static Reduction jit_reduce_create(ReductionType rtype) {
    using UInt = uint_with_size_t<Value>;

    switch (rtype) {
        case ReductionType::Add:
            return [](const void *ptr_, uint32_t start, uint32_t end, void *out) {
                const Value *ptr = (const Value *) ptr_;
                Value result = 0;
                for (uint32_t i = start; i != end; ++i)
                    result += ptr[i];
                *((Value *) out) = result;
            };

        case ReductionType::Mul:
            return [](const void *ptr_, uint32_t start, uint32_t end, void *out) {
                const Value *ptr = (const Value *) ptr_;
                Value result = 1;
                for (uint32_t i = start; i != end; ++i)
                    result *= ptr[i];
                *((Value *) out) = result;
            };

        case ReductionType::Max:
            return [](const void *ptr_, uint32_t start, uint32_t end, void *out) {
                const Value *ptr = (const Value *) ptr_;
                Value result = std::is_integral<Value>::value
                                   ?  std::numeric_limits<Value>::min()
                                   : -std::numeric_limits<Value>::infinity();
                for (uint32_t i = start; i != end; ++i)
                    result = std::max(result, ptr[i]);
                *((Value *) out) = result;
            };

        case ReductionType::Min:
            return [](const void *ptr_, uint32_t start, uint32_t end, void *out) {
                const Value *ptr = (const Value *) ptr_;
                Value result = std::is_integral<Value>::value
                                   ?  std::numeric_limits<Value>::max()
                                   :  std::numeric_limits<Value>::infinity();
                for (uint32_t i = start; i != end; ++i)
                    result = std::max(result, ptr[i]);
                *((Value *) out) = result;
            };

        case ReductionType::Or:
            return [](const void *ptr_, uint32_t start, uint32_t end, void *out) {
                const UInt *ptr = (const UInt *) ptr_;
                UInt result = 0;
                for (uint32_t i = start; i != end; ++i)
                    result |= ptr[i];
                *((UInt *) out) = result;
            };

        case ReductionType::And:
            return [](const void *ptr_, uint32_t start, uint32_t end, void *out) {
                const UInt *ptr = (const UInt *) ptr_;
                UInt result = (UInt) -1;
                for (uint32_t i = start; i != end; ++i)
                    result &= ptr[i];
                *((UInt *) out) = result;
            };

        default: jit_raise("jit_reduce_create(): unsupported reduction type!");
    }
}

static Reduction jit_reduce_create(VarType type, ReductionType rtype) {
    switch (type) {
        case VarType::Int8:    return jit_reduce_create<int8_t  >(rtype);
        case VarType::UInt8:   return jit_reduce_create<uint8_t >(rtype);
        case VarType::Int16:   return jit_reduce_create<int16_t >(rtype);
        case VarType::UInt16:  return jit_reduce_create<uint16_t>(rtype);
        case VarType::Int32:   return jit_reduce_create<int32_t >(rtype);
        case VarType::UInt32:  return jit_reduce_create<uint32_t>(rtype);
        case VarType::Int64:   return jit_reduce_create<int64_t >(rtype);
        case VarType::UInt64:  return jit_reduce_create<uint64_t>(rtype);
        case VarType::Float32: return jit_reduce_create<float   >(rtype);
        case VarType::Float64: return jit_reduce_create<double  >(rtype);
        default: jit_raise("jit_reduce_create(): unsupported data type!");
    }
}

void jit_reduce(VarType type, ReductionType rtype, const void *ptr, uint32_t size,
                void *out) {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_raise("jit_reduce(): you must invoke jit_device_set() to "
                  "choose a target device before calling this function.");

    jit_log(Debug, "jit_reduce(" ENOKI_PTR ", type=%s, rtype=%s, size=%u)",
            (uintptr_t) ptr, var_type_name[(int) type],
            reduction_name[(int) rtype], size);

    uint32_t type_size = var_type_size[(int) type];

    if (stream->cuda) {
        const Device &device = state.devices[stream->device];
        CUfunction func = jit_cuda_reductions[(int) rtype][(int) type][device.id];
        if (!func)
            jit_raise("jit_reduce(): no existing kernel for type=%s, rtype=%s!",
                      var_type_name[(int) type], reduction_name[(int) rtype]);

        uint32_t thread_count = 1024,
                 shared_size = thread_count * type_size,
                 block_count;

        device.get_launch_config(&block_count, nullptr, size, thread_count);

        if (size <= 1024) {
            /// This is a small array, do everything in just one reduction.
            void *args[] = { &ptr, &size, &out };
            cuda_check(cuLaunchKernel(func, 1, 1, 1, thread_count, 1, 1,
                                      shared_size, stream->handle, args,
                                      nullptr));
        } else {
            void *temp = jit_malloc(AllocType::Device, block_count * type_size);

            // First reduction
            void *args_1[] = { &ptr, &size, &temp };
            cuda_check(cuLaunchKernel(func, block_count, 1, 1, thread_count, 1, 1,
                                      shared_size, stream->handle, args_1,
                                      nullptr));

            // Second reduction
            void *args_2[] = { &temp, &block_count, &out };
            cuda_check(cuLaunchKernel(func, 1, 1, 1, thread_count, 1, 1,
                                      shared_size, stream->handle, args_2,
                                      nullptr));

            jit_free(temp);
        }
    } else {
        Reduction reduction = jit_reduce_create(type, rtype);

#if defined(ENOKI_TBB)
        struct Inputs {
            uint32_t size;
            uint32_t isize;
            Reduction reduction;
            const void *ptr;
            void *out;
        };

        auto func = [](void *inputs_) {
            Inputs inputs = *((Inputs *) inputs_);
            Reduction reduction = inputs.reduction;
            uint32_t size = inputs.size;
            uint32_t isize = inputs.isize;
            const void *ptr = inputs.ptr;
            void *out = inputs.out;

            uint64_t identity = 0;
            inputs.reduction(nullptr, 0, 0, &identity);

            uint64_t result = tbb::parallel_deterministic_reduce(
                tbb::blocked_range<uint32_t>(0, size, 16384),
                identity,
                [reduction, ptr, isize](const tbb::blocked_range<uint32_t> &range, uint64_t value) {
                    uint8_t temp[16] { };
                    memcpy(temp, &value, isize);
                    reduction(ptr, range.begin(), range.end(), temp + isize);
                    reduction(temp, 0, 2, temp);
                    return *((uint64_t *) temp);
                },
                [reduction, isize](uint64_t a, uint64_t b) {
                    uint8_t temp[16] { };
                    memcpy(temp, &a, isize);
                    memcpy(temp + isize, &b, isize);
                    reduction(temp, 0, 2, &temp);
                    return *((uint64_t *) temp);
                }
            );

            memcpy(out, &result, isize);
        };

        Inputs inputs;
        inputs.size = size;
        inputs.isize = var_type_size[(int) type];
        inputs.reduction = reduction;
        inputs.ptr = ptr;
        inputs.out = out;

        tbb_stream_enqueue_func(stream, func, &inputs, sizeof(Inputs));
#else
        reduction(ptr, 0, size, out);
#endif
    }
}

/// 'All' reduction for boolean arrays
uint8_t jit_all(uint8_t *values, uint32_t size) {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_raise("jit_all(): you must invoke jit_device_set() to "
                  "choose a target device before calling this function.");

    uint32_t reduced_size = (size + 3) / 4,
             trailing     = reduced_size * 4 - size;

    jit_log(Debug, "jit_all(" ENOKI_PTR ", size=%u)", (uintptr_t) values, size);

    if (trailing) {
        bool filler = true;
        jit_memset(values + size, trailing, sizeof(bool), &filler);
    }

    uint8_t result;
    if (stream->cuda) {
        uint8_t *out = (uint8_t *) jit_malloc(AllocType::HostPinned, 4);
        jit_reduce(VarType::UInt32, ReductionType::And, values, reduced_size, out);
        jit_sync_stream();
        result = (out[0] & out[1] & out[2] & out[3]) != 0;
        jit_free(out);
    } else {
        uint8_t out[4];
        jit_reduce(VarType::UInt32, ReductionType::And, values, reduced_size, out);
        jit_sync_stream();
        result = (out[0] & out[1] & out[2] & out[3]) != 0;
    }

    return result;
}

/// 'Any' reduction for boolean arrays
uint8_t jit_any(uint8_t *values, uint32_t size) {
    Stream *stream = active_stream;
    uint32_t reduced_size = (size + 3) / 4,
             trailing     = reduced_size * 4 - size;

    jit_log(Debug, "jit_any(" ENOKI_PTR ", size=%u)", (uintptr_t) values, size);

    if (trailing) {
        bool filler = false;
        jit_memset(values + size, trailing, sizeof(bool), &filler);
    }

    uint8_t result;
    if (stream->cuda) {
        uint8_t *out = (uint8_t *) jit_malloc(AllocType::HostPinned, 4);
        jit_reduce(VarType::UInt32, ReductionType::Or, values, reduced_size, out);
        jit_sync_stream();
        result = (out[0] | out[1] | out[2] | out[3]) != 0;
        jit_free(out);
    } else {
        uint8_t out[4];
        jit_reduce(VarType::UInt32, ReductionType::Or, values, reduced_size, out);
        jit_sync_stream();
        result = (out[0] | out[1] | out[2] | out[3]) != 0;
    }

    return result;
}

/// Exclusive prefix sum
void jit_scan(const uint32_t *in, uint32_t *out, uint32_t size) {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_raise("jit_scan(): you must invoke jit_device_set() to "
                  "choose a target device before calling this function.");

    if (stream->cuda) {
        const Device &device = state.devices[stream->device];

        /// Exclusive prefix scan processes 4K elements / block, 4 per thread
        uint32_t block_count      = (size + 4096 - 1) / 4096,
                 thread_count     = std::min(round_pow2((size + 3u) / 4u), 1024u),
                 shared_size      = thread_count * 2 * sizeof(uint32_t);

        jit_log(Debug, "jit_scan(" ENOKI_PTR " -> " ENOKI_PTR ", size=%u)",
                (uintptr_t) in, (uintptr_t) out, size);

        if (size == 0) {
            return;
        } else if (size == 1) {
            cuda_check(cuMemsetD8Async((CUdeviceptr) out, 0, sizeof(uint32_t),
                                       stream->handle));
        } else if (size <= 4096) {
            void *args[] = { &in, &out, &size };
            cuda_check(cuLaunchKernel(jit_cuda_scan_small_u32[device.id], 1, 1, 1,
                                      thread_count, 1, 1, shared_size,
                                      stream->handle, args, nullptr));
        } else {
            uint32_t *block_sums = (uint32_t *) jit_malloc(
                AllocType::Device, block_count * sizeof(uint32_t));

            void *args[] = { &in, &out, &block_sums };
            cuda_check(cuLaunchKernel(jit_cuda_scan_large_u32[device.id], block_count, 1, 1,
                                      thread_count, 1, 1, shared_size,
                                      stream->handle, args, nullptr));

            jit_scan(block_sums, block_sums, block_count);

            void *args_2[] = { &out, &block_sums };
            cuda_check(cuLaunchKernel(jit_cuda_scan_offset[device.id], block_count, 1, 1,
                                      thread_count, 1, 1, 0, stream->handle,
                                      args_2, nullptr));
            jit_free(block_sums);
        }
    } else {
#if defined(ENOKI_TBB)
        struct Inputs {
            const uint32_t *in;
            uint32_t *out;
            uint32_t size;
        };
        auto func = [](void *inputs_) {
            Inputs inputs = *((Inputs *) inputs_);
            tbb::parallel_scan(
                tbb::blocked_range<uint32_t>(0u, inputs.size, 4096u), 0u,
                [inputs](const tbb::blocked_range<uint32_t> &range, uint32_t sum,
                          bool final_scan) -> uint32_t {
                    if (final_scan) {
                        for (uint32_t i = range.begin(); i < range.end(); ++i) {
                            uint32_t backup = sum;
                            sum += inputs.in[i];
                            inputs.out[i] = backup;
                        }
                    } else {
                        for (uint32_t i = range.begin(); i < range.end(); ++i)
                            sum += inputs.in[i];

                    }
                    return sum;
                },
                [](uint32_t v0, uint32_t v1) -> uint32_t { return v0 + v1; });
        };
        Inputs inputs { in, out, size };
        tbb_stream_enqueue_func(stream, func, &inputs, sizeof(Inputs));
#else
        unlock_guard guard(state.mutex);
        uint32_t accum = 0;
        for (uint32_t i = 0; i < size; ++i) {
            uint32_t value = in[i];
            out[i] = accum;
            accum += value;
        }
#endif
    }
}

static void cuda_transpose(Stream *stream, const uint32_t *in, uint32_t *out,
                           uint32_t rows, uint32_t cols) {
    const Device &device = state.devices[stream->device];

    uint16_t blocks_x = (cols + 15u) / 16u,
             blocks_y = (rows + 15u) / 16u;

    jit_log(Debug,
            "jit_transpose(" ENOKI_PTR " -> " ENOKI_PTR
            ", rows=%u, cols=%u, blocks=%ux%u)",
            (uintptr_t) in, (uintptr_t) out, rows, cols, blocks_x, blocks_y);

    void *args[] = { &in, &out, &rows, &cols };
    cuda_check(cuLaunchKernel(
        jit_cuda_transpose[device.id], blocks_x, blocks_y, 1, 16, 16, 1,
        16 * 17 * sizeof(uint32_t), stream->handle, args, nullptr));
}

/// Compute a permutation to reorder an integer array into a sorted configuration
uint32_t jit_mkperm(const uint32_t *ptr, uint32_t size, uint32_t bucket_count,
                    uint32_t *perm, uint32_t *offsets) {
    if (size == 0)
        return 0;
    else if (unlikely(bucket_count == 0))
        jit_fail("jit_mkperm(): bucket_count cannot be zero!");

    Stream *stream = active_stream;

    if (stream->cuda) {
        const Device &device = state.devices[stream->device];

        // Don't use more than 1 block/SM due to shared memory requirement
        const uint32_t warp_size = 32;
        uint32_t block_count, thread_count;
        device.get_launch_config(&block_count, &thread_count, size, 1024, 1);

        // Always launch full warps (the kernel impl. assumes that this is the case)
        uint32_t warp_count = (thread_count + warp_size - 1) / warp_size;
        thread_count = warp_count * warp_size;

        uint32_t bucket_size_1   = bucket_count * sizeof(uint32_t),
                 bucket_size_all = bucket_size_1 * block_count;

        /* If there is a sufficient amount of shared memory, atomically accumulate into a
           shared memory buffer. Otherwise, use global memory, which is much slower. */
        uint32_t shared_size = 0;
        const char *variant = nullptr;
        CUfunction phase_1 = nullptr, phase_4 = nullptr;
        bool initialize_buckets = false;

        if (bucket_size_1 * warp_count <= device.shared_memory_bytes) {
            /* "Tiny" variant, which uses shared memory atomics to produce a stable
               permutation. Handles up to 512 buckets with 64KiB of shared memory. */

            phase_1 = jit_cuda_mkperm_phase_1_tiny[device.id];
            phase_4 = jit_cuda_mkperm_phase_4_tiny[device.id];
            shared_size = bucket_size_1 * warp_count;
            bucket_size_all *= warp_count;
            variant = "tiny";
        } else if (bucket_size_1 <= device.shared_memory_bytes) {
            /* "Small" variant, which uses shared memory atomics and handles up to
               16K buckets with 64KiB of shared memory. The permutation can be
               somewhat unstable due to scheduling variations when performing atomic
               operations (although some effort is made to keep it stable within
               each group of 32 elements by performing an intra-warp reduction.) */

            phase_1 = jit_cuda_mkperm_phase_1_small[device.id];
            phase_4 = jit_cuda_mkperm_phase_4_small[device.id];
            shared_size = bucket_size_1;
            variant = "small";
        } else {
            /* "Large" variant, which uses global memory atomics and handles
               arbitrarily many elements (though this is somewhat slower than the
               previous two shared memory variants). The permutation can be somewhat
               unstable due to scheduling variations when performing atomic
               operations (although some effort is made to keep it stable within
               each group of 32 elements by performing an intra-warp reduction.)
               Buckets must be zero-initialized explicitly. */

            phase_1 = jit_cuda_mkperm_phase_1_large[device.id];
            phase_4 = jit_cuda_mkperm_phase_4_large[device.id];
            variant = "large";
            initialize_buckets = true;
        }

        bool needs_transpose = bucket_size_1 != bucket_size_all;
        uint32_t *buckets_1, *buckets_2, *counter = nullptr;
        buckets_1 = buckets_2 =
            (uint32_t *) jit_malloc(AllocType::Device, bucket_size_all);

        // Scratch space for matrix transpose operation
        if (needs_transpose)
            buckets_2 = (uint32_t *) jit_malloc(AllocType::Device, bucket_size_all);

        if (offsets) {
            counter = (uint32_t *) jit_malloc(AllocType::Device, sizeof(uint32_t)),
            cuda_check(cuMemsetD8Async((CUdeviceptr) counter, 0, sizeof(uint32_t),
                                       stream->handle));
        }

        if (initialize_buckets)
            cuda_check(cuMemsetD8Async((CUdeviceptr) buckets_1, 0,
                                       bucket_size_all, stream->handle));

        /* Determine the amount of work to be done per block, and ensure that it is
           divisible by the warp size (the kernel implementation assumes this.) */
        uint32_t size_per_block = (size + block_count - 1) / block_count;
        size_per_block = (size_per_block + warp_size - 1) / warp_size * warp_size;

        jit_log(Debug,
                "jit_mkperm(" ENOKI_PTR
                ", size=%u, bucket_count=%u, block_count=%u, thread_count=%u, "
                "size_per_block=%u, variant=%s, shared_size=%u)",
                (uintptr_t) ptr, size, bucket_count, block_count, thread_count,
                size_per_block, variant, shared_size);

        // Phase 1: Count the number of occurrences per block
        void *args_1[] = { &ptr, &buckets_1, &size, &size_per_block,
                           &bucket_count };

        cuda_check(cuLaunchKernel(phase_1, block_count, 1, 1, thread_count, 1, 1,
                                  shared_size, stream->handle, args_1, nullptr));

        // Phase 2: exclusive prefix sum over transposed buckets
        if (needs_transpose)
            cuda_transpose(stream, buckets_1, buckets_2,
                           bucket_size_all / bucket_size_1, bucket_count);

        jit_scan(buckets_2, buckets_2, bucket_size_all / sizeof(uint32_t));

        if (needs_transpose)
            cuda_transpose(stream, buckets_2, buckets_1, bucket_count,
                           bucket_size_all / bucket_size_1);

        // Phase 3: collect non-empty buckets (optional)
        if (likely(offsets)) {
            uint32_t block_count_3, thread_count_3;
            device.get_launch_config(&block_count_3, &thread_count_3,
                                     bucket_count * block_count);

            // Round up to a multiple of the thread count
            uint32_t bucket_count_rounded =
                (bucket_count + thread_count_3 - 1) / thread_count_3 * thread_count_3;

            void *args_3[] = { &buckets_1, &bucket_count, &bucket_count_rounded,
                               &size,      &counter,      &offsets };

            cuda_check(cuLaunchKernel(jit_cuda_mkperm_phase_3[device.id],
                                      block_count_3, 1, 1, thread_count_3, 1, 1,
                                      sizeof(uint32_t) * thread_count_3,
                                      stream->handle, args_3, nullptr));

            cuda_check(cuMemcpyAsync((CUdeviceptr) (offsets + 4 * bucket_count),
                                     (CUdeviceptr) counter, sizeof(uint32_t),
                                     stream->handle));

            cuda_check(cuEventRecord(stream->event, stream->handle));
        }

        // Phase 4: write out permutation based on bucket counts
        void *args_4[] = { &ptr, &buckets_1, &perm, &size, &size_per_block,
                           &bucket_count };
        cuda_check(cuLaunchKernel(phase_4, block_count, 1, 1, thread_count, 1, 1,
                                  shared_size, stream->handle, args_4, nullptr));

        if (likely(offsets)) {
            unlock_guard guard(state.mutex);
            cuda_check(cuEventSynchronize(stream->event));
        }

        jit_free(buckets_1);
        if (needs_transpose)
            jit_free(buckets_2);
        jit_free(counter);

        return offsets ? offsets[4 * bucket_count] : 0u;
    } else { // if (!stream->cuda)
#if defined(ENOKI_TBB)
        jit_sync_stream(); /// XXX can we also enqueue this asynchronously?

        uint32_t num_tasks      = jit_llvm_thread_count * 4,
                 items_per_task = std::max(4096u, (size + num_tasks - 1) / num_tasks);
        num_tasks = (size + items_per_task - 1) / items_per_task;

        size_t bucket_size_local = sizeof(uint32_t) * (size_t) bucket_count;

        uint32_t **buckets =
            (uint32_t **) jit_malloc(AllocType::Host, sizeof(uint32_t *) * num_tasks);

        for (uint32_t i = 0; i < num_tasks; ++i)
            buckets[i] =
                (uint32_t *) jit_malloc(AllocType::Host, bucket_size_local);

        tbb::parallel_for(
            tbb::blocked_range<uint32_t>(0u, num_tasks, 1u),
            [&](const tbb::blocked_range<uint32_t> &range) {
                uint32_t start = range.begin() * items_per_task,
                         end = std::min(size, start + items_per_task);

                uint32_t *buckets_local = buckets[range.begin()];

                memset(buckets_local, 0, bucket_size_local);
                for (uint32_t i = start; i != end; ++i)
                    buckets_local[ptr[i]]++;
            },
            tbb::simple_partitioner()
        );

        uint32_t sum = 0, unique_count = 0;
        for (uint32_t i = 0; i < bucket_count; ++i) {
            uint32_t sum_local = 0;
            for (uint32_t j = 0; j < num_tasks; ++j) {
                uint32_t value = buckets[j][i];
                buckets[j][i] = sum + sum_local;
                sum_local += value;
            }
            if (sum_local > 0) {
                if (offsets) {
                    offsets[unique_count*4] = i;
                    offsets[unique_count*4 + 1] = sum;
                    offsets[unique_count*4 + 2] = sum_local;
                    offsets[unique_count*4 + 3] = 0;
                }
                unique_count++;
                sum += sum_local;
            }
        }

        tbb::parallel_for(
            tbb::blocked_range<uint32_t>(0u, num_tasks, 1u),
            [&](const tbb::blocked_range<uint32_t> &range) {
                uint32_t start = range.begin() * items_per_task,
                         end = std::min(size, start + items_per_task);

                uint32_t *buckets_local = buckets[range.begin()];

                for (uint32_t i = start; i != end; ++i) {
                    uint32_t index = buckets_local[ptr[i]]++;
                    perm[index] = i;
                }
            },
            tbb::simple_partitioner()
        );

        for (uint32_t i = 0; i < num_tasks; ++i)
            jit_free(buckets[i]);
        jit_free(buckets);

        return unique_count;
#else
        size_t bucket_size = sizeof(uint32_t) * (size_t) bucket_count;
        uint32_t *buckets = (uint32_t *) jit_malloc(AllocType::Host, bucket_size);
        memset(buckets, 0, bucket_size);
        uint32_t unique_count = 0;

        for (uint32_t i = 0; i < size; ++i)
            buckets[ptr[i]]++;

        uint32_t sum = 0;
        for (uint32_t i = 0; i < bucket_count; ++i) {
            if (buckets[i] > 0) {
                if (offsets) {
                    offsets[unique_count*4] = i;
                    offsets[unique_count*4 + 1] = sum;
                    offsets[unique_count*4 + 2] = buckets[i];
                    offsets[unique_count*4 + 3] = 0;
                }
                unique_count++;
            }
            uint32_t temp = buckets[i];
            buckets[i] = sum;
            sum += temp;
        }

        for (uint32_t i = 0; i < size; ++i) {
            uint32_t index = buckets[ptr[i]]++;
            perm[index] = i;
        }

        jit_free(buckets);
        return unique_count;
#endif
    }
}

// Compute a permutation to reorder an array of registered pointers
VCallBucket *jit_vcall(const char *domain, uint32_t index,
                       uint32_t *bucket_count_out) {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_raise("jit_vcall(): you must invoke jit_device_set() to "
                  "choose a target device before calling this function.");
    bool cuda = stream->cuda;

    auto it = state.vcall_cache.find(index);
    if (it != state.vcall_cache.end()) {
        *bucket_count_out = it.value().first;
        return it.value().second;
    }

    jit_var_eval(index);
    Variable *v = jit_var(index);
    const void *ptr = v->data;
    uint32_t size = v->size;

    uint32_t bucket_count = jit_registry_get_max(domain) + 1;

    jit_log(Debug, "jit_vcall(%u, domain=\"%s\")", index, domain);

    size_t perm_size    = (size_t) size * (size_t) sizeof(uint32_t),
           offsets_size = (bucket_count * 4 + 1) * sizeof(uint32_t);

    uint32_t *offsets = (uint32_t *) jit_malloc(
        cuda ? AllocType::HostPinned : AllocType::Host, offsets_size);
    uint32_t *perm = (uint32_t *) jit_malloc(
        cuda ? AllocType::Device : AllocType::Host, perm_size);

    // Compute permutation
    uint32_t unique_count = jit_mkperm((const uint32_t *) ptr, size,
                                       bucket_count, perm, offsets),
             unique_count_out = unique_count;

#if defined(ENOKI_TBB)
    if (!stream->cuda)
        perm = (uint32_t *) jit_malloc_migrate(perm, AllocType::HostAsync);
#endif

    // Register permutation variable with JIT backend and transfer ownership
    uint32_t perm_var = jit_var_map(VarType::UInt32, perm, size, 1);

    Variable v2;
    v2.type = (uint32_t) VarType::UInt32;
    v2.dep[0] = perm_var;
    v2.retain_data = true;
    v2.tsize = 1;
    v2.cuda = active_stream->cuda;
    v2.unaligned = 1;

    uint32_t *offsets_out = offsets;

    for (uint32_t i = 0; i < unique_count; ++i) {
        uint32_t bucket_id     = offsets[i * 4 + 0],
                 bucket_offset = offsets[i * 4 + 1],
                 bucket_size   = offsets[i * 4 + 2];

        if (bucket_id == 0) {
            --unique_count_out;
            continue;
        }

        /// Crete variable for permutation subrange
        v2.data = perm + bucket_offset;
        v2.size = bucket_size;

        uint32_t index;
        Variable *vo;
        std::tie(index, vo) = jit_trace_append(v2);

        jit_var_inc_ref_int(perm_var);
        jit_var_inc_ref_ext(index, vo);

        void *ptr = jit_registry_get_ptr(domain, bucket_id);
        memcpy(offsets_out, &ptr, sizeof(void *));
        memcpy(offsets_out + 2, &index, sizeof(uint32_t *));
        offsets_out += 4;

        jit_trace(
            "jit_vcall(): registered variable %u: bucket %u (%p) of size %u.",
            index, bucket_id, ptr, bucket_size);
    }

    jit_var_dec_ref_ext(perm_var);

    *bucket_count_out = unique_count_out;

    v = jit_var(index);
    v->vcall_cached = true;
    state.vcall_cache[index] =
        std::make_pair(unique_count_out, (VCallBucket *) offsets);

    return (VCallBucket *) offsets;
}

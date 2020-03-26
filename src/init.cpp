#include "internal.h"
#include "malloc.h"
#include "internal.h"
#include "log.h"
#include <glob.h>
#include <dlfcn.h>
#include <sys/stat.h>

State state;
Buffer buffer{1024};
__thread Stream *active_stream = nullptr;

static_assert(
    sizeof(tsl::detail_robin_hash::bucket_entry<VariableMap::value_type, false>) == 64,
    "VariableMap: incorrect bucket size, likely an issue with padding/packing!");

static_assert(sizeof(VariableKey) == 4*8,
    "VariableKey: incorrect size, likely an issue with padding/packing!");

/// Initialize core data structures of the JIT compiler
void jit_init(int llvm, int cuda) {
    if (state.has_llvm != 0 || state.has_cuda != 0 || (llvm == 0 && cuda == 0))
        return;

    // Enumerate CUDA devices and collect suitable ones
    jit_log(Info, "jit_init(): detecting devices ..");

    state.has_llvm = llvm && jit_llvm_init();
    state.has_cuda = cuda && jit_cuda_init();

    for (int i = 0; cuda && i < jit_cuda_devices; ++i) {
        int pci_bus_id = 0, pci_dom_id = 0, pci_dev_id = 0, num_sm = 0,
            unified_addr = 0, managed = 0, concurrent_managed = 0,
            shared_memory_bytes = 0;
        size_t mem_total = 0;
        char name[256];

        cuda_check(cuDeviceTotalMem(&mem_total, i));
        cuda_check(cuDeviceGetName(name, sizeof(name), i));
        cuda_check(cuDeviceGetAttribute(&pci_bus_id, CU_DEVICE_ATTRIBUTE_PCI_BUS_ID, i));
        cuda_check(cuDeviceGetAttribute(&pci_dev_id, CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID, i));
        cuda_check(cuDeviceGetAttribute(&pci_dom_id, CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID, i));
        cuda_check(cuDeviceGetAttribute(&num_sm, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, i));
        cuda_check(cuDeviceGetAttribute(&unified_addr, CU_DEVICE_ATTRIBUTE_UNIFIED_ADDRESSING, i));
        cuda_check(cuDeviceGetAttribute(&managed, CU_DEVICE_ATTRIBUTE_CONCURRENT_MANAGED_ACCESS, i));
        cuda_check(cuDeviceGetAttribute(&concurrent_managed, CU_DEVICE_ATTRIBUTE_MANAGED_MEMORY, i));
        cuda_check(cuDeviceGetAttribute(&shared_memory_bytes, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK, i));

        jit_log(Info,
                " - Found CUDA device %i: \"%s\" "
                "(PCI ID %02x:%02x.%i, %i SMs w/%s shared mem., %s global mem.)",
                i, name, pci_bus_id, pci_dev_id, pci_dom_id, num_sm,
                std::string(jit_mem_string(shared_memory_bytes)).c_str(),
                std::string(jit_mem_string(mem_total)).c_str());

        if (unified_addr == 0) {
            jit_log(Warn, " - Warning: device does *not* support unified addressing, skipping ..");
            continue;
        } else if (managed == 0) {
            jit_log(Warn, " - Warning: device does *not* support managed memory, skipping ..");
            continue;
        }
        if (concurrent_managed == 0)
            jit_log(Warn, " - Warning: device does *not* support concurrent managed access.");

        Device device;
        device.id = i;
        device.num_sm = num_sm;
        device.shared_memory_bytes = shared_memory_bytes;
        cuda_check(cuDevicePrimaryCtxRetain(&device.context, i));
        state.devices.push_back(device);
    }

    // Enable P2P communication if possible
    for (auto &a : state.devices) {
        for (auto &b : state.devices) {
            if (a.id == b.id)
                continue;

            int peer_ok = 0;
            cuda_check(cuDeviceCanAccessPeer(&peer_ok, a.id, b.id));
            if (peer_ok) {
                jit_log(Debug, " - Enabling peer access from device %i -> %i",
                        a.id, b.id);
                cuda_check(cuCtxSetCurrent(a.context));
                CUresult rv = cuCtxEnablePeerAccess(b.context, 0);
                if (rv == CUDA_ERROR_PEER_ACCESS_ALREADY_ENABLED)
                    continue;
                cuda_check(rv);
            }
        }
    }

    if (!state.devices.empty())
        cuda_check(cuCtxSetCurrent(state.devices[0].context));

    state.variable_index = 1;
    state.alloc_id_ctr = 1;
    state.variables.reserve(512);
    state.alloc_used.reserve(512);
    state.alloc_id_rev.reserve(512);
    state.alloc_id_fwd.reserve(512);
    state.cse_cache.reserve(512);
    state.kernel_cache.reserve(128);
}

/// Release all resources used by the JIT compiler, and report reference leaks.
void jit_shutdown(int light) {
    if (state.has_cuda) {
        jit_log(Info, "jit_shutdown(): destroying streams ..");

        for (auto &v : state.streams) {
            const Stream *stream = v.second;
            jit_device_set(stream->device, stream->stream);
            jit_free_flush();
            cuda_check(cuStreamSynchronize(stream->handle));
            cuda_check(cuEventDestroy(stream->event));
            cuda_check(cuStreamDestroy(stream->handle));
            delete stream->release_chain;
            delete stream;
        }
        state.streams.clear();
        active_stream = nullptr;
    }

    for (auto &v : state.kernel_cache) {
        const KernelKey &key = v.first;
        const Kernel &kernel = v.second;

        if (key.device == -1) {
            jit_llvm_free(kernel);
        } else {
            const Device &device = state.devices.at(key.device);
            cuda_check(cuCtxSetCurrent(device.context));
            cuda_check(cuModuleUnload(kernel.cuda.cu_module));
        }

        free(key.str);
    }
    state.kernel_cache.clear();

    if (std::max(state.log_level_stderr, state.log_level_callback) >= LogLevel::Warn) {
        uint32_t n_leaked = 0;
        for (auto &var : state.variables) {
            if (n_leaked == 0)
                jit_log(Warn, "jit_shutdown(): detected variable leaks:");
            if (n_leaked < 10)
                jit_log(Warn,
                        " - variable %u is still being referenced! (internal "
                        "references=%u, external references=%u)",
                        var.first, var.second.ref_count_int,
                        var.second.ref_count_ext);
            else if (n_leaked == 10)
                jit_log(Warn, " - (skipping remainder)");
            ++n_leaked;
        }

        if (n_leaked > 0)
            jit_log(Warn, "jit_shutdown(): %u variables are still referenced!", n_leaked);
    }

    if (state.variables.empty() && !state.cse_cache.empty()) {
        for (auto &kv: state.cse_cache)
            jit_log(Warn, " - %u: %u, %u, %u", kv.second, kv.first.dep[0], kv.first.dep[1], kv.first.dep[2]);
        jit_fail("jit_shutdown(): detected a common subexpression elimination cache leak!");
    }

    if (state.variables.empty() && !state.variable_from_ptr.empty())
        jit_fail("jit_shutdown(): detected a pointer-literal leak!");

    jit_malloc_shutdown();

    if (state.has_cuda) {
        cuda_check(cuCtxSetCurrent(nullptr));
        for (auto &v : state.devices)
            cuda_check(cuDevicePrimaryCtxRelease(v.id));
        state.devices.clear();
    }

    jit_log(Info, "jit_shutdown(): done");

    if (light == 0) {
        jit_llvm_shutdown();
        jit_cuda_shutdown();
    }

    state.has_cuda = false;
    state.has_llvm = false;
}

/// Set the currently active device & stream
void jit_device_set(int32_t device, uint32_t stream) {
    if (device == -1) {
        if (active_stream != nullptr) {
            cuda_check(cuCtxSetCurrent(nullptr));
            active_stream = nullptr;
        }
        return;
    }

    if ((size_t) device >= state.devices.size())
        jit_raise("jit_device_set(): invalid device ID!");

    cuda_check(cuCtxSetCurrent(state.devices[device].context));

    std::pair<uint32_t, uint32_t> key(device, stream);
    auto it = state.streams.find(key);

    Stream *stream_ptr, *active_stream_ptr = active_stream;
    if (it != state.streams.end()) {
        stream_ptr = it->second;
        if (stream_ptr == active_stream_ptr)
            return;
        jit_trace("jit_device_set(device=%i, stream=%i): selecting stream", device, stream);
    } else {
        jit_trace("jit_device_set(device=%i, stream=%i): creating stream", device, stream);
        CUstream handle = nullptr;
        CUevent event = nullptr;
        cuda_check(cuStreamCreate(&handle, CU_STREAM_NON_BLOCKING));
        cuda_check(cuEventCreate(&event, CU_EVENT_DISABLE_TIMING));

        stream_ptr = new Stream();
        stream_ptr->device = device;
        stream_ptr->stream = stream;
        stream_ptr->handle = handle;
        stream_ptr->event = event;
        state.streams[key] = stream_ptr;
    }

    active_stream = stream_ptr;
}

/// Wait for all computation on the current stream to finish
void jit_sync_stream() {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        return;

    jit_trace("jit_sync_stream(): starting ..");
    /* Release mutex while synchronizing */ {
        unlock_guard guard(state.mutex);
        cuda_check(cuStreamSynchronize(stream->handle));
    }
    jit_trace("jit_sync_stream(): done.");
}

/// Wait for all computation on the current device to finish
void jit_sync_device() {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        return;

    jit_trace("jit_sync_device(): starting ..");
    /* Release mutex while synchronizing */ {
        unlock_guard guard(state.mutex);
        cuda_check(cuCtxSynchronize());
    }
    jit_trace("jit_sync_device(): done.");
}

void *jit_find_library(const char *fname, const char *glob_pat,
                       const char *env_var) {
    const char *env_var_val = env_var ? getenv(env_var) : nullptr;
    if (env_var_val != nullptr && strlen(env_var_val) == 0)
        env_var_val = nullptr;

    void *handle = dlopen(env_var_val ? env_var_val : fname, RTLD_LAZY);

    if (!handle & !env_var_val) {
        glob_t g;
        if (glob(glob_pat, 0, nullptr, &g) == 0) {
            const char *chosen = nullptr;
            if (g.gl_pathc > 1) {
                jit_log(Warn, "jit_llvm_init(): Multiple versions of "
                              "%s were found on your system!\n", fname);
                std::sort(g.gl_pathv, g.gl_pathv + g.gl_pathc,
                          [](const char *a, const char *b) {
                              return strcmp(a, b) < 0;
                          });
                uint32_t counter = 1;
                for (int j = 0; j < 2; ++j) {
                    for (size_t i = 0; i < g.gl_pathc; ++i) {
                        struct stat buf;
                        // Skip symbolic links at first
                        if (j == 0 && (lstat(g.gl_pathv[i], &buf) || S_ISLNK(buf.st_mode)))
                            continue;
                        jit_log(Warn, " %u. \"%s\"", counter++, g.gl_pathv[i]);
                        chosen = g.gl_pathv[i];
                    }
                    if (chosen)
                        break;
                }
                jit_log(Warn,
                        "\nChoosing the last one. Specify a path manually "
                        "using the environment\nvariable '%s' "
                        "to override this behavior.\n", env_var);
            } else if (g.gl_pathc == 1) {
                chosen = g.gl_pathv[0];
            }
            if (chosen)
                handle = dlopen(chosen, RTLD_LAZY);
            globfree(&g);
        }
    }

    return handle;
}

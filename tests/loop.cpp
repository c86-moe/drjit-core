#include "test.h"
#include <enoki-jit/containers.h>
#include <utility>

template <typename Mask> struct Loop {
    static constexpr JitBackend Backend = Mask::Backend;

    template <typename... Args>
    Loop(const char *name, Args &... args)
        : m_name(name), m_state(0), m_cond(0), m_se_offset((uint32_t) -1), m_size(0) {
        if constexpr (sizeof...(Args) > 0) {
            (put(args), ...);
            init();
        }
    }

    ~Loop() {
        jit_var_dec_ref_ext(m_cond);

        if (m_se_offset != (uint32_t) -1) {
            jit_side_effects_rollback(Backend, m_se_offset);
            jit_set_flags(m_flags);
        }

        if (m_state != 0 && m_state != 3) {
            jit_log(Warn, "Loop(): de-allocated in an inconsistent state. "
                          "(Loop.cond() must run exactly twice!)");
        }
    }

    template <typename Value> void put(Value &value) {
        m_index_p.push_back(value.index_ptr());
        size_t size = value.size();
        if (m_size != 0 && size != 1 && size != m_size)
            jit_raise("Loop.put(): loop variables have inconsistent sizes!");
        if (size > m_size)
            m_size = size;
    }

    void init() {
        if (m_state)
            jit_raise("Loop(): was already initialized!");
        step();
        m_se_offset = jit_side_effects_scheduled(Backend);
        m_flags = jit_flags();
        jit_set_flag(JitFlag::DisableSideEffects, 0);
        m_state = 1;
    }

    bool cond(Mask value) {
        switch (m_state) {
            case 0:
                jit_raise("Loop(): must be initialized first!");

            case 1:
                m_cond = value.index();
                jit_var_inc_ref_ext(m_cond);
                step();
                for (uint32_t i = 0; i < m_index_p.size(); ++i)
                    m_index_in.push_back(*m_index_p[i]);

                break;

            case 2:
                for (uint32_t i = 0; i < m_index_p.size(); ++i)
                    m_index_out.push_back(*m_index_p[i]);
                jit_var_loop(m_name, m_cond, (uint32_t) m_index_p.size(),
                             m_index_in.data(), m_index_out.data(),
                             m_se_offset, m_index_out.data());
                for (uint32_t i = 0; i < m_index_p.size(); ++i) {
                    uint32_t &index = *m_index_p[i];
                    jit_var_dec_ref_ext(index);
                    index = m_index_out[i];
                }
                m_se_offset = (uint32_t) -1;
                jit_set_flags(m_flags);
                break;

            default:
                jit_raise("Loop(): invalid state!");
        }

        return m_state++ == 1;
    }

protected:
    // Insert an indirection via placeholder variables
    void step() {
        for (size_t i = 0; i < m_index_p.size(); ++i) {
            uint32_t &index = *m_index_p[i],
                     next = jit_var_new_placeholder(index, 0);
            jit_var_dec_ref_ext(index);
            index = next;
        }
    }

private:
    const char *m_name;
    ek_vector<uint32_t> m_index_in;
    ek_vector<uint32_t> m_index_out;
    ek_vector<uint32_t *> m_index_p;
    uint32_t m_state;
    uint32_t m_cond;
    uint32_t m_se_offset;
    uint32_t m_flags;
    size_t m_size;
};

#if 0
TEST_CUDA(01_symbolic_loop) {
    // Tests a simple loop evaluated at once, or in parts
    for (uint32_t i = 0; i < 3; ++i) {
        jit_set_flag(JitFlag::LoopRecord, i != 0);
        jit_set_flag(JitFlag::LoopOptimize, i == 2);

        for (uint32_t j = 0; j < 2; ++j) {
            UInt32 x = arange<UInt32>(10);
            Float y = zero<Float>(1);
            Float z = 1;

            Loop<Mask> loop("MyLoop", x, y, z);
            while (loop.cond(x < 5)) {
                y += Float(x);
                x += 1;
                z += 1;
            }

            if (j == 0) {
                jit_var_schedule(x.index());
                jit_var_schedule(y.index());
                jit_var_schedule(z.index());
            }

            jit_assert(strcmp(z.str(), "[6, 5, 4, 3, 2, 1, 1, 1, 1, 1]") == 0);
            jit_assert(strcmp(y.str(), "[10, 10, 9, 7, 4, 0, 0, 0, 0, 0]") == 0);
            jit_assert(strcmp(x.str(), "[5, 5, 5, 5, 5, 5, 6, 7, 8, 9]") == 0);
        }
    }
}
#endif

TEST_CUDA(02_side_effect) {
    // Tests that side effects only happen once
    for (uint32_t i = 0; i < 2; ++i) {
        jit_set_flag(JitFlag::LoopRecord, i != 0);
        jit_set_flag(JitFlag::LoopOptimize, i == 2);

        for (uint32_t j = 0; j < 3; ++j) {
            UInt32 x = arange<UInt32>(10);
            Float y = zero<Float>(1);
            UInt32 target = zero<UInt32>(11);

            Loop<Mask> loop("MyLoop", x, y);
            while (loop.cond(x < 5)) {
                scatter_reduce(ReduceOp::Add, target, UInt32(1), x);
                y += Float(x);
                x += 1;
            }

            if (j == 0) {
                jit_var_schedule(x.index());
                jit_var_schedule(y.index());
            }

            jit_assert(strcmp(y.str(), "[10, 10, 9, 7, 4, 0, 0, 0, 0, 0]") == 0);
            jit_assert(strcmp(x.str(), "[5, 5, 5, 5, 5, 5, 6, 7, 8, 9]") == 0);
            jit_assert(strcmp(target.str(), "[1, 2, 3, 4, 5, 0, 0, 0, 0, 0, 0]") == 0);
        }
    }
}

TEST_CUDA(03_side_effect_2) {
    // Tests that side effects work that don't reference loop variables
    for (uint32_t i = 0; i < 2; ++i) {
        jit_set_flag(JitFlag::LoopRecord, i != 0);
        jit_set_flag(JitFlag::LoopOptimize, i == 2);

        for (uint32_t j = 0; j < 3; ++j) {
            UInt32 x = arange<UInt32>(10);
            UInt32 target = zero<UInt32>(11);

            Loop<Mask> loop("MyLoop", x);
            while (loop.cond(x < 5)) {
                scatter_reduce(ReduceOp::Add, target, UInt32(2), UInt32(2));
                x += 1;
            }

            jit_assert(strcmp(x.str(), "[5, 5, 5, 5, 5, 5, 6, 7, 8, 9]") == 0);
            jit_assert(strcmp(target.str(), "[0, 0, 30, 0, 0, 0, 0, 0, 0, 0, 0]") == 0);
        }
    }
}

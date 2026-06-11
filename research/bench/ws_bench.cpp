// Work-stealing micro-benchmark for the CAWS oneTBB patch.
//
// Mimics the per-frame structure of PARSEC bodytrack / fluidanimate:
// an outer sequential loop ("frames") each running a parallel_for over a
// data array with *imbalanced* per-element cost, so every frame ends with
// an implicit barrier whose latency is dominated by the slowest worker.
//
// Usage: ws_bench [n_elements] [frames] [mode]
//   mode: bench (default) | verify
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_reduce.h>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/task_group.h>
#include <oneapi/tbb/global_control.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

double element_work(int i, int rounds) {
    volatile double x = 1.0 + i * 1e-3;
    for (int k = 0; k < rounds; ++k) {
        x = x + std::sqrt(x) * 1e-6;
    }
    return x;
}

// Triangular cost profile: later elements are ~5x more expensive than early
// ones, which makes the tail chunks of each frame critical-path sensitive.
int rounds_for(int i, int n) {
    return 200 + (i * 800) / n;
}

long long fib_serial(int n) { return n < 2 ? n : fib_serial(n - 1) + fib_serial(n - 2); }

// Recursive task_group fib: stresses deep deques and steal decisions.
long long fib_tasks(int n) {
    if (n < 16) return fib_serial(n);
    long long a = 0, b = 0;
    tbb::task_group tg;
    tg.run([&] { a = fib_tasks(n - 1); });
    b = fib_tasks(n - 2);
    tg.wait();
    return a + b;
}

} // namespace

int main(int argc, char** argv) {
    int n = argc > 1 ? std::atoi(argv[1]) : 200000;
    int frames = argc > 2 ? std::atoi(argv[2]) : 100;
    const char* mode = argc > 3 ? argv[3] : "bench";

    std::vector<double> out(n, 0.0);

    if (std::strcmp(mode, "verify") == 0) {
        // Correctness: parallel results must match serial references.
        tbb::parallel_for(tbb::blocked_range<int>(0, n), [&](const tbb::blocked_range<int>& r) {
            for (int i = r.begin(); i != r.end(); ++i) out[i] = element_work(i, 50);
        });
        for (int i = 0; i < n; ++i) {
            double ref = element_work(i, 50);
            if (out[i] != ref) { std::printf("FAIL parallel_for at %d\n", i); return 1; }
        }
        double psum = tbb::parallel_reduce(tbb::blocked_range<int>(0, n), 0.0,
            [&](const tbb::blocked_range<int>& r, double acc) {
                for (int i = r.begin(); i != r.end(); ++i) acc += std::sqrt((double)i);
                return acc;
            }, std::plus<double>());
        double ssum = 0.0;
        for (int i = 0; i < n; ++i) ssum += std::sqrt((double)i);
        if (std::abs(psum - ssum) > 1e-6 * std::abs(ssum)) { std::printf("FAIL reduce\n"); return 1; }
        long long f = fib_tasks(30);
        if (f != 832040) { std::printf("FAIL fib: %lld\n", f); return 1; }
        std::printf("PASS\n");
        return 0;
    }

    // Warmup (thread pool creation, first-touch).
    tbb::parallel_for(tbb::blocked_range<int>(0, n), [&](const tbb::blocked_range<int>& r) {
        for (int i = r.begin(); i != r.end(); ++i) out[i] = 0.0;
    });

    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        tbb::parallel_for(tbb::blocked_range<int>(0, n), [&](const tbb::blocked_range<int>& r) {
            for (int i = r.begin(); i != r.end(); ++i) out[i] = element_work(i, rounds_for(i, n));
        });
    }
    auto t1 = std::chrono::steady_clock::now();

    double sum = 0.0;
    for (double v : out) sum += v;
    std::printf("checksum %.6e elapsed %.3f s\n", sum, std::chrono::duration<double>(t1 - t0).count());
    return 0;
}

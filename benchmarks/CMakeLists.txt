# Phase 1 benchmarks.
#
# Deliberately minimal. The real measurement work is Phase 13, and benchmarking
# before there is a workload to benchmark is exactly the "fake-HFT optimisation
# theatre" the research warns against. What matters now is that the `benchmark`
# preset CONFIGURES AND RUNS -- an advertised preset that errors out is a broken
# promise in the developer-facing surface (review finding H-2).
#
# These four cover the primitives that will sit in the per-event path once the
# engine exists, so Phase 13 has a baseline to compare against rather than a
# blank page.

add_executable(ptl_bench_core bench_core.cpp)
target_link_libraries(ptl_bench_core PRIVATE
    ptl::core ptl::oms ptl::portfolio ptl::risk ptl::execution ptl::market ptl::features ptl::labels ptl::validation ptl::research ptl::signal ptl::sizing ptl::construction ptl::pipeline ptl::databento
    benchmark::benchmark_main ptl::native_arch)

# Model benchmarks are compiled only when Eigen is available, matching the
# conditional build of ptl::models itself.
if(PTL_HAVE_EIGEN)
    target_link_libraries(ptl_bench_core PRIVATE ptl::models)
    target_compile_definitions(ptl_bench_core PRIVATE PTL_HAVE_MODELS=1)
endif()

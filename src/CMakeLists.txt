# One target per module. Dependencies are declared explicitly and point only
# downward, so a cyclic include becomes a CMake configure error rather than a
# design problem discovered later.

add_library(ptl_core
    core/types.cpp
    core/time.cpp
    core/clock.cpp
    core/rng.cpp
    core/result.cpp
    core/instrument_table.cpp)
add_library(ptl::core ALIAS ptl_core)
target_include_directories(ptl_core PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(ptl_core PUBLIC ptl::warnings PRIVATE ptl::sanitizers ptl::coverage)
if(PTL_ENABLE_TRACE)
    target_compile_definitions(ptl_core PUBLIC PTL_ENABLE_TRACE=1)
endif()
if(PTL_FORCE_RESULT_FALLBACK)
    target_compile_definitions(ptl_core PUBLIC PTL_FORCE_RESULT_FALLBACK=1)
endif()

add_library(ptl_log log/logger.cpp)
add_library(ptl::log ALIAS ptl_log)
target_link_libraries(ptl_log PUBLIC ptl::core PRIVATE ptl::sanitizers)

add_library(ptl_config config/config.cpp)
add_library(ptl::config ALIAS ptl_config)
target_link_libraries(ptl_config
    PUBLIC ptl::core ptl::log
    PRIVATE tomlplusplus::tomlplusplus ptl::sanitizers)

add_library(ptl_experiments experiments/registry.cpp)
add_library(ptl::experiments ALIAS ptl_experiments)
target_link_libraries(ptl_experiments
    PUBLIC ptl::core
    PRIVATE ptl::sqlite3 ptl::sanitizers)

add_library(ptl_market
    market/calendar.cpp
    market/events.cpp
    market/provider.cpp
    market/source.cpp
    market/validator.cpp
    market/http.cpp
    market/alpaca.cpp
    market/symbol_mapper.cpp)
add_library(ptl::market ALIAS ptl_market)
target_link_libraries(ptl_market
    PUBLIC ptl::core ptl::auth
    PRIVATE nlohmann_json::nlohmann_json ptl::sanitizers)

add_library(ptl_auth auth/credentials.cpp)
add_library(ptl::auth ALIAS ptl_auth)
target_link_libraries(ptl_auth PUBLIC ptl::core PRIVATE ptl::sanitizers)

add_library(ptl_oms
    oms/order.cpp
    oms/order_manager.cpp)
add_library(ptl::oms ALIAS ptl_oms)
target_link_libraries(ptl_oms PUBLIC ptl::core PRIVATE ptl::sanitizers)

add_library(ptl_portfolio
    portfolio/position.cpp
    portfolio/portfolio.cpp)
add_library(ptl::portfolio ALIAS ptl_portfolio)
target_link_libraries(ptl_portfolio PUBLIC ptl::core ptl::oms ptl::market PRIVATE ptl::sanitizers)

add_library(ptl_execution
    execution/models.cpp
    execution/broker.cpp
    execution/quote_book.cpp)
add_library(ptl::execution ALIAS ptl_execution)
target_link_libraries(ptl_execution PUBLIC ptl::core ptl::oms ptl::market PRIVATE ptl::sanitizers)

add_library(ptl_risk risk/risk_manager.cpp)
add_library(ptl::risk ALIAS ptl_risk)
target_link_libraries(ptl_risk PUBLIC ptl::core ptl::oms ptl::portfolio PRIVATE ptl::sanitizers)

add_library(ptl_accounting accounting/journal.cpp)
add_library(ptl::accounting ALIAS ptl_accounting)
target_link_libraries(ptl_accounting PUBLIC ptl::core ptl::oms ptl::portfolio PRIVATE ptl::sanitizers)

add_library(ptl_analytics
    analytics/metrics.cpp
    analytics/drawdown.cpp
    analytics/exposure.cpp
    analytics/risk.cpp
    analytics/attribution.cpp
    analytics/report.cpp
    analytics/rolling.cpp)
add_library(ptl::analytics ALIAS ptl_analytics)
target_link_libraries(ptl_analytics PUBLIC ptl::core ptl::portfolio ptl::accounting PRIVATE ptl::sanitizers)

add_library(ptl_engine
    engine/strategy.cpp
    engine/engine.cpp)
add_library(ptl::engine ALIAS ptl_engine)
target_link_libraries(ptl_engine
    PUBLIC ptl::core ptl::market ptl::oms ptl::portfolio ptl::risk
           ptl::execution ptl::accounting ptl::analytics
    PRIVATE ptl::sanitizers)

add_library(ptl_features
    features/intraday.cpp
    features/feature_set.cpp
    features/cross_sectional.cpp
    features/matrix.cpp
    features/factor.cpp
    features/validation.cpp)
add_library(ptl::features ALIAS ptl_features)
target_link_libraries(ptl_features PUBLIC ptl::core ptl::market PRIVATE ptl::sanitizers)

# ---------------------------------------------------------------------------
# ptl_labels -- THE ONLY MODULE PERMITTED TO INDEX FORWARD IN TIME.
#
# Deliberately its own target with NO reverse dependency. The live binaries do
# not link it, so a leak through labels is a LINKER ERROR rather than a subtle
# bug. See include/ptl/labels/label.hpp and docs/00-architecture.md.
# ---------------------------------------------------------------------------
add_library(ptl_labels labels/label.cpp)
add_library(ptl::labels ALIAS ptl_labels)
target_link_libraries(ptl_labels PUBLIC ptl::core PRIVATE ptl::sanitizers)

add_library(ptl_validation validation/walk_forward.cpp)
add_library(ptl::validation ALIAS ptl_validation)
target_link_libraries(ptl_validation PUBLIC ptl::core PRIVATE ptl::sanitizers)

add_library(ptl_research
    research/evaluation.cpp
    research/trial.cpp)
add_library(ptl::research ALIAS ptl_research)
target_link_libraries(ptl_research
    PUBLIC ptl::core ptl::validation ptl::experiments ptl::features
    PRIVATE ptl::sanitizers)

# ---------------------------------------------------------------------------
# ptl_models -- Phase 6. Built only when Eigen is available, so a clone without
# it still compiles and tests everything else rather than failing outright.
# ---------------------------------------------------------------------------
if(PTL_HAVE_EIGEN)
    add_library(ptl_models
        models/matrix.cpp
        models/scaler.cpp
        models/linear.cpp
        models/logistic.cpp
        models/pipeline.cpp)
    add_library(ptl::models ALIAS ptl_models)
    target_link_libraries(ptl_models
        PUBLIC ptl::core ptl::features ptl::validation
        # PRIVATE: Eigen is an implementation detail and must not leak into the
        # include path of anything that links ptl::models.
        PRIVATE ${PTL_EIGEN_TARGET} ptl::sanitizers)
endif()

add_library(ptl_signal
    signal/signal.cpp
    signal/generator.cpp
    signal/filter.cpp)
add_library(ptl::signal ALIAS ptl_signal)
target_link_libraries(ptl_signal PUBLIC ptl::core ptl::market PRIVATE ptl::sanitizers)

add_library(ptl_sizing sizing/sizer.cpp)
add_library(ptl::sizing ALIAS ptl_sizing)
target_link_libraries(ptl_sizing PUBLIC ptl::core ptl::signal ptl::portfolio PRIVATE ptl::sanitizers)

add_library(ptl_construction construction/rebalance.cpp)
add_library(ptl::construction ALIAS ptl_construction)
target_link_libraries(ptl_construction
    PUBLIC ptl::core ptl::oms ptl::portfolio ptl::sizing
    PRIVATE ptl::sanitizers)

add_library(ptl_pipeline pipeline/trading_pipeline.cpp)
add_library(ptl::pipeline ALIAS ptl_pipeline)
target_link_libraries(ptl_pipeline
    PUBLIC ptl::core ptl::engine ptl::signal ptl::sizing ptl::construction
    PRIVATE ptl::sanitizers)

add_library(ptl_databento
    databento/decoder.cpp
    databento/client.cpp)
add_library(ptl::databento ALIAS ptl_databento)
target_link_libraries(ptl_databento
    PUBLIC ptl::core ptl::market ptl::auth ptl::execution
    PRIVATE nlohmann_json::nlohmann_json ptl::sanitizers)

add_library(ptl_algo
    algo/schedule.cpp
    algo/algorithms.cpp
    algo/executor.cpp)
add_library(ptl::algo ALIAS ptl_algo)
target_link_libraries(ptl_algo
    PUBLIC ptl::core ptl::oms ptl::execution ptl::engine
    PRIVATE ptl::sanitizers)

add_library(ptl_report report/generator.cpp)
add_library(ptl::report ALIAS ptl_report)
target_link_libraries(ptl_report PUBLIC ptl::core ptl::analytics PRIVATE ptl::sanitizers)

add_library(ptl_optimization
    optimization/covariance.cpp
    optimization/constraints.cpp
    optimization/objective.cpp
    optimization/optimizer.cpp)
add_library(ptl::optimization ALIAS ptl_optimization)
# PUBLIC ptl::core only. The optimizer computes weights from forecasts and a
# risk model; it has no need of the OMS, the broker or the engine, and linking
# them would create a path by which it could reach the venue.
target_link_libraries(ptl_optimization PUBLIC ptl::core PRIVATE ptl::sanitizers)

add_library(ptl_attribution
    attribution/pnl.cpp
    attribution/execution_quality.cpp)
add_library(ptl::attribution ALIAS ptl_attribution)
# Read-only observer: it consumes completed history and holds no handle to the
# OMS, broker or engine, so it cannot trade, size, optimize or execute.
target_link_libraries(ptl_attribution
    PUBLIC ptl::core ptl::portfolio ptl::oms ptl::accounting ptl::analytics
    PRIVATE ptl::sanitizers)

add_library(ptl_reporting reporting/reports.cpp)
add_library(ptl::reporting ALIAS ptl_reporting)
# Composes with ptl::report rather than duplicating it: json_escape is reused,
# not reimplemented. Two escaping implementations is how one ends up wrong.
target_link_libraries(ptl_reporting
    PUBLIC ptl::core ptl::analytics ptl::attribution ptl::report
    PRIVATE ptl::sanitizers)

add_library(ptl_strategy strategy/registry.cpp)
add_library(ptl::strategy ALIAS ptl_strategy)
# ptl::core ONLY. The catalogue must never know about execution: a registry
# that could reach the engine would be a registry that could trade.
target_link_libraries(ptl_strategy PUBLIC ptl::core PRIVATE ptl::sanitizers)

add_library(ptl_storage storage/registry.cpp)
add_library(ptl::storage ALIAS ptl_storage)
target_link_libraries(ptl_storage PUBLIC ptl::core PRIVATE ptl::sanitizers)

add_library(ptl_experiment experiment/experiment.cpp)
add_library(ptl::experiment ALIAS ptl_experiment)
# Depends on strategy, storage and analytics; NONE of them depends on this.
# The dependency runs one way only, so the execution engine never learns that
# experiments exist.
target_link_libraries(ptl_experiment
    PUBLIC ptl::core ptl::strategy ptl::storage ptl::analytics ptl::attribution
    PRIVATE ptl::sanitizers)

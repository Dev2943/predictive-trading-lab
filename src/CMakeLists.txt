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
    market/alpaca.cpp)
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
    execution/broker.cpp)
add_library(ptl::execution ALIAS ptl_execution)
target_link_libraries(ptl_execution PUBLIC ptl::core ptl::oms ptl::market PRIVATE ptl::sanitizers)

add_library(ptl_risk risk/risk_manager.cpp)
add_library(ptl::risk ALIAS ptl_risk)
target_link_libraries(ptl_risk PUBLIC ptl::core ptl::oms ptl::portfolio PRIVATE ptl::sanitizers)

add_library(ptl_accounting accounting/journal.cpp)
add_library(ptl::accounting ALIAS ptl_accounting)
target_link_libraries(ptl_accounting PUBLIC ptl::core ptl::oms ptl::portfolio PRIVATE ptl::sanitizers)

add_library(ptl_analytics analytics/metrics.cpp)
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

add_executable(ptl_tests
    unit/test_named_type.cpp
    unit/test_types.cpp
    unit/test_time_chain.cpp
    unit/test_clock.cpp
    unit/test_rng.cpp
    unit/test_instrument_table.cpp
    unit/test_config.cpp
    unit/test_registry.cpp
    unit/test_logger.cpp
    unit/test_catch_integration.cpp
    unit/test_calendar.cpp
    unit/test_market_events.cpp
    unit/test_entitlement.cpp
    unit/test_credentials.cpp
    leakage/test_replay.cpp
    unit/test_validator.cpp
    unit/test_alpaca.cpp
    unit/test_orders.cpp
    property/test_portfolio.cpp
    leakage/test_execution.cpp
    unit/test_risk_metrics.cpp
    leakage/test_engine.cpp)
# tests/ on the include path so "support/ptl_catch.hpp" resolves. That header
# carries the Catch2 StringMaker specialisations for ptl vocabulary types --
# see its comment block for the libc++/libstdc++ divergence it exists to fix.
target_include_directories(ptl_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ptl_tests PRIVATE
    ptl::core ptl::log ptl::config ptl::experiments ptl::market ptl::auth ptl::oms ptl::portfolio ptl::execution ptl::risk ptl::accounting ptl::analytics ptl::engine
    Catch2::Catch2WithMain ptl::sanitizers)
catch_discover_tests(ptl_tests)

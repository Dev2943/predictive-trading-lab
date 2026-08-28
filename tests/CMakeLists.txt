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
    leakage/test_engine.cpp
    leakage/test_feature_causality.cpp
    unit/test_features_intraday.cpp
    leakage/test_cross_sectional.cpp
    unit/test_feature_storage.cpp
    leakage/test_labels.cpp
    leakage/test_walk_forward.cpp
    unit/test_research.cpp
    unit/test_execution_algos.cpp
    unit/test_quote_book.cpp
    unit/test_databento.cpp
    unit/test_signals.cpp
    unit/test_sizing_construction.cpp
    leakage/test_pipeline_e2e.cpp
    leakage/test_quote_execution.cpp
    leakage/test_executor.cpp)
# tests/ on the include path so "support/ptl_catch.hpp" resolves. That header
# carries the Catch2 StringMaker specialisations for ptl vocabulary types --
# see its comment block for the libc++/libstdc++ divergence it exists to fix.
target_include_directories(ptl_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ptl_tests PRIVATE
    ptl::core ptl::log ptl::config ptl::experiments ptl::market ptl::auth ptl::oms ptl::portfolio ptl::execution ptl::risk ptl::accounting ptl::analytics ptl::engine ptl::features ptl::signal ptl::sizing ptl::construction ptl::pipeline ptl::databento ptl::algo ptl::labels ptl::validation ptl::research
    Catch2::Catch2WithMain ptl::sanitizers)
if(PTL_HAVE_EIGEN)
    target_sources(ptl_tests PRIVATE
        unit/test_models.cpp
        leakage/test_model_leakage.cpp)
    target_link_libraries(ptl_tests PRIVATE ptl::models)
    target_compile_definitions(ptl_tests PRIVATE PTL_HAVE_MODELS=1)
endif()

catch_discover_tests(ptl_tests)

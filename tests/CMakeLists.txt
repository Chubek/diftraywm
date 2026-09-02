add_executable(diftraywm_smoke_test smoke.cpp)
target_compile_features(diftraywm_smoke_test PRIVATE cxx_std_20)
target_include_directories(diftraywm_smoke_test PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
add_test(NAME diftraywm_smoke_test COMMAND diftraywm_smoke_test)

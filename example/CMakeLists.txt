add_executable(easycat_example easycat_example.cc)
target_link_libraries(easycat_example kickcat)
set_target_properties(easycat_example PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON
)

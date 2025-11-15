
# Check if libcamera is available
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA libcamera)

if (LIBCAMERA_FOUND)
    set(HAS_LIBCAMERA ON)
else()
    set(HAS_LIBCAMERA OFF)
endif()

MJPG_STREAMER_PLUGIN_OPTION(input_libcamera "Raspberry Pi libcamera input plugin"
                            ONLYIF HAS_LIBCAMERA)

if (PLUGIN_INPUT_LIBCAMERA)

    # Include libcamera headers
    include_directories(${LIBCAMERA_INCLUDE_DIRS})

    # Compile the plugin (note: .cpp extension for C++ source)
    MJPG_STREAMER_PLUGIN_COMPILE(input_libcamera input_libcamera.cpp)

    # Set C++ standard
    set_property(TARGET input_libcamera PROPERTY CXX_STANDARD 17)
    set_property(TARGET input_libcamera PROPERTY CXX_STANDARD_REQUIRED ON)

    # Link libcamera libraries
    target_link_libraries(input_libcamera ${LIBCAMERA_LIBRARIES})

    message(STATUS "libcamera plugin will be compiled")
    message(STATUS "  libcamera include dirs: ${LIBCAMERA_INCLUDE_DIRS}")
    message(STATUS "  libcamera libraries: ${LIBCAMERA_LIBRARIES}")

endif()

cmake_minimum_required(VERSION 3.16)

project(uwavclient VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

include(GNUInstallDirs)

set(PROJECT_SOURCES
        main.cpp
        uwavclient.cpp
        uwavclient.h
        uwavclient.ui
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(uwavclient
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET uwavclient APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(uwavclient SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(uwavclient
            ${PROJECT_SOURCES}
            u_audioprovider.h u_audioprovider.cpp
            u_interfacedescriptor.h
            u_ringbuffer.h u_ringbuffer.cpp
            u_enginestatistics.h u_enginestatistics.cpp
            sioevent.h
            localconfig.h localconfig.cpp
            u_sharedfunctions.h

        )
    endif()
endif()

target_link_libraries(uwavclient PRIVATE Qt${QT_VERSION_MAJOR}::Widgets sioclient)

# Build stuff for Socket.IO C++ implementation
add_definitions(
    # Uses the Asio C++ library (note: "Asynchronous I/O," NOT "Audio Stream")
    # which we need to compile without Boost
    -DBOOST_DATE_TIME_NO_LIB
    -DBOOST_REGEX_NO_LIB
    -DASIO_STANDALONE

    # sioclient also needs to be compiled with C++ 11 conventions, for consistency
    # I'm guessing
    -D_WEBSOCKETPP_CPP11_STL_
    -D_WEBSOCKETPP_CPP11_FUNCTIONAL_
    -D_WEBSOCKETPP_CPP11_TYPE_TRAITS_
    -D_WEBSOCKETPP_CPP11_CHROMO_
)
set(SOCKETIO_SOURCES
    "sio/sio_client.cpp"
    "sio/sio_socket.cpp"
    "sio/internal/sio_client_impl.cpp"
    "sio/internal/sio_packet.cpp"
)
add_library(sioclient ${SOCKETIO_SOURCES}
)
target_compile_options(sioclient PUBLIC -Wno-deprecated-declarations)

# These are submodules we're integrating directly from a github
# repo, these are part of the Socket.IO C++ implementation
set(MODULE_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/lib/websocketpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/rapidjson/include
    ${CMAKE_CURRENT_LIST_DIR}/lib/asio/asio/include
)
target_include_directories(sioclient PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/sio
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
    ${MODULE_INCLUDE_DIRS}
)
target_compile_features(sioclient PUBLIC cxx_std_11)
target_link_libraries(sioclient PUBLIC Threads::Threads)

# Not sure what this does but it's in the sioclient project's
# cmake file
list(APPEND TARGET_LIBARIES sioclient)

# Include SSL in Socket.IO library if able
find_package(OpenSSL)
if(OPENSSL_FOUND)
    add_library(sioclient_tls ${SOCKETIO_SOURCES})
    target_include_directories(sioclient_tls PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sio>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
        ${MODULE_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
    )
    target_compile_features(sioclient_tls PUBLIC cxx_std_11)
    target_link_libraries(sioclient_tls PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(sioclient_tls PRIVATE -DSIO_TLS)
    target_link_libraries(sioclient_tls PUBLIC Threads::Threads)
    list(APPEND TARGET_LIBRARIES sioclient_tls)
endif()

export(PACKAGE sioclient)
file(GLOB ALL_HEADERS ${CMAKE_CURRENT_LIST_DIR}/sio/*.h)
install(FILES ${ALL_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(TARGETS ${TARGET_LIBRARIES} EXPORT sioclientTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Generate cmake config files
include(CMakePackageConfigHelpers)
set(SioConfigPackageLocation ${CMAKE_INSTALL_LIBDIR}/cmake/sioclient)
string(REGEX REPLACE "([^;]+)" "find_dependency(\\1)" _find_dependency_calls "${_package_dependencies}")
string(REPLACE ";" "\n" _find_dependency_calls "${_find_dependency_calls}")

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/sioclient/sioclientConfigVersion.cmake"
    VERSION ${sioclient_VERSION}
    COMPATIBILITY AnyNewerVersion
)
export(EXPORT sioclientTargets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/sioclient/sioclientTargets.cmake"
    NAMESPACE sioclient::
)
configure_package_config_file(sioclientConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/sioclient/sioclientConfig.cmake"
    INSTALL_DESTINATION "${SioConfigPackageLocation}"
)
# End Socket.IO section

# Continuing from earlier pkconfig search for pipewire libs...
target_include_directories(uwavclient PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
target_link_libraries(uwavclient PRIVATE ${PIPEWIRE_LIBRARIES})
target_compile_options(uwavclient PRIVATE ${PIPEWIRE_CFLAGS_OTHER})

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.uwavclient)
endif()
set_target_properties(uwavclient PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

# SIO
install(EXPORT sioclientTargets
    NAMESPACE sioclient::
    DESTINATION ${SioConfigPackageLocation}
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/sioclient/sioclientConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/sioclient/sioclientConfigVersion.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/sioclient/sioclientTargets.cmake"
    DESTINATION
    ${SioConfigPackageLocation}
)
# /SIO

install(TARGETS uwavclient
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(uwavclient)
endif()

cmake_minimum_required(VERSION 3.21)

project(MnetPlusDownloader VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Concurrent Network Sql Widgets)

qt_add_executable(MnetPlusDownloader
    src/main.cpp
    src/localization.cpp
    src/localization.h
    src/mainwindow.cpp
    src/mainwindow.h
    src/browsercookieloader.cpp
    src/browsercookieloader.h
    src/mnetresolver.cpp
    src/mnetresolver.h
    src/hlsdownloader.cpp
    src/hlsdownloader.h
    src/downloadcontroller.cpp
    src/downloadcontroller.h
)

target_link_libraries(MnetPlusDownloader PRIVATE
    Qt6::Concurrent
    Qt6::Network
    Qt6::Sql
    Qt6::Widgets
)

if(APPLE)
    target_link_libraries(MnetPlusDownloader PRIVATE
        "-framework CoreFoundation"
        "-framework Security"
    )
endif()

if(WIN32)
    target_link_libraries(MnetPlusDownloader PRIVATE
        Crypt32
        Bcrypt
    )
endif()

set_target_properties(MnetPlusDownloader PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "Mnet Plus Downloader"
    MACOSX_BUNDLE_GUI_IDENTIFIER "local.mnetplus.downloader"
    WIN32_EXECUTABLE TRUE
)

install(TARGETS MnetPlusDownloader
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    qt_add_executable(MnetPlusDownloaderResolverTests
        tests/resolver_test.cpp
        src/localization.cpp
        src/localization.h
        src/mnetresolver.cpp
        src/mnetresolver.h
    )
    target_link_libraries(MnetPlusDownloaderResolverTests PRIVATE
        Qt6::Network
        Qt6::Test
        Qt6::Widgets
    )
    add_test(NAME resolver COMMAND MnetPlusDownloaderResolverTests)

    qt_add_executable(MnetPlusDownloaderLocalizationTests
        tests/localization_test.cpp
        src/localization.cpp
        src/localization.h
    )
    target_link_libraries(MnetPlusDownloaderLocalizationTests PRIVATE
        Qt6::Test
    )
    add_test(NAME localization COMMAND MnetPlusDownloaderLocalizationTests)

    qt_add_executable(MnetPlusDownloaderDownloadTests
        tests/downloadcontroller_test.cpp
        src/localization.cpp
        src/localization.h
        src/downloadcontroller.cpp
        src/downloadcontroller.h
        src/mnetresolver.cpp
        src/mnetresolver.h
        src/hlsdownloader.cpp
        src/hlsdownloader.h
    )
    target_link_libraries(MnetPlusDownloaderDownloadTests PRIVATE
        Qt6::Network
        Qt6::Test
        Qt6::Widgets
    )
    add_test(NAME downloadcontroller COMMAND MnetPlusDownloaderDownloadTests)

    qt_add_executable(MnetPlusDownloaderLiveSessionTests
        tests/live_session_test.cpp
        src/localization.cpp
        src/localization.h
        src/browsercookieloader.cpp
        src/browsercookieloader.h
        src/mnetresolver.cpp
        src/mnetresolver.h
    )
    target_link_libraries(MnetPlusDownloaderLiveSessionTests PRIVATE
        Qt6::Concurrent
        Qt6::Network
        Qt6::Sql
        Qt6::Test
        Qt6::Widgets
    )
    if(APPLE)
        target_link_libraries(MnetPlusDownloaderLiveSessionTests PRIVATE
            "-framework CoreFoundation"
            "-framework Security"
        )
    endif()
    if(WIN32)
        target_link_libraries(MnetPlusDownloaderLiveSessionTests PRIVATE
            Crypt32
            Bcrypt
        )
    endif()
    add_test(NAME live_session COMMAND MnetPlusDownloaderLiveSessionTests)
endif()

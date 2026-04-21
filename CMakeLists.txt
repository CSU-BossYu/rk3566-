cmake_minimum_required(VERSION 3.10)

set(BR_OUT  "/home/tspi/sdk/buildroot/output/rockchip_rk3566")
set(BR_HOST "${BR_OUT}/host")

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   "${BR_HOST}/bin/aarch64-buildroot-linux-gnu-gcc" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${BR_HOST}/bin/aarch64-buildroot-linux-gnu-g++" CACHE FILEPATH "" FORCE)

set(BR_SYSROOT "${BR_HOST}/aarch64-buildroot-linux-gnu/sysroot")
set(BR_STAGING "${BR_OUT}/staging")

if(EXISTS "${BR_SYSROOT}")
  set(CMAKE_SYSROOT "${BR_SYSROOT}" CACHE PATH "" FORCE)
  set(CMAKE_FIND_ROOT_PATH "${BR_SYSROOT}" CACHE PATH "" FORCE)
elseif(EXISTS "${BR_STAGING}")
  set(CMAKE_SYSROOT "${BR_STAGING}" CACHE PATH "" FORCE)
  set(CMAKE_FIND_ROOT_PATH "${BR_STAGING}" CACHE PATH "" FORCE)
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

project(demo_rga_v4l2 C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -fPIC)

# --- Crash trace (no gdb): print backtrace on SIGSEGV/SIGBUS/SIGABRT ---
option(ENABLE_CRASH_TRACE "Enable backtrace symbols in crash log" ON)
if(ENABLE_CRASH_TRACE)
  add_compile_options(-g -fno-omit-frame-pointer)
endif()

option(ENABLE_OPENCV "Enable OpenCV CPU-DNN path (opencv_process)" ON)
option(ENABLE_RKNN   "Enable RKNN path (rknn/*)" ON)
option(ALLOW_SHLIB_UNDEFINED "Allow unresolved symbols in linked shared libs" OFF)
option(ENABLE_LVGL "Enable LVGL UI (DRM + EVDEV)" ON)

# =========================
# Sources
# =========================
set(SRC_LIST
  main.cpp

  app/app_runtime.cpp
  app/vision_worker.cpp
  app/camera_service.cpp

  capture/v4l2_capture.cpp
  rga/rga_preprocess.cpp
  rga/rga_worker.cpp

  utils/face_db.cpp

  
)

if(ENABLE_OPENCV)
  list(APPEND SRC_LIST
    opencv_process/face_align.cpp
  )
endif()

if(ENABLE_RKNN)
  list(APPEND SRC_LIST
    rknn/rknn_model.cpp
    rknn/scrfd_detector_rknn.cpp
    rknn/arcface_rknn.cpp
  )
endif()

if(ENABLE_LVGL)
  list(APPEND SRC_LIST
    ui/generated/ui.cpp

    ui/ui_app.cpp
    ui/ui_model.cpp
    ui/ui_runtime.cpp
    ui/ui_navigator.cpp
    ui/ui_controller.cpp

    ui/screens/screen_recog.cpp
    ui/screens/screen_enroll.cpp
    ui/screens/screen_manage.cpp
  )
endif()

add_executable(${PROJECT_NAME} ${SRC_LIST})

if(ENABLE_CRASH_TRACE)
  # -rdynamic: keep symbols for backtrace_symbols_fd
  target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
endif()

# --- SQLite (amalgamation, build-in) ---
target_sources(${PROJECT_NAME} PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/sqlite
)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/capture
  ${CMAKE_SOURCE_DIR}/rga
  ${CMAKE_SOURCE_DIR}/rga/include
  ${CMAKE_SOURCE_DIR}/opencv_process
  ${CMAKE_SOURCE_DIR}/utils
  ${CMAKE_SOURCE_DIR}/ui
  ${CMAKE_SOURCE_DIR}/app
)

if(ENABLE_RKNN)
  target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/rknn
    ${CMAKE_SOURCE_DIR}/rknn/include
  )
  target_compile_definitions(${PROJECT_NAME} PRIVATE USE_RKNN=1)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
  # 简化部署：adb push build/out/ 到开发板后直接运行 ./demo_rga_v4l2
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
)

# 运行时从可执行文件旁边的 lib/ 加载
set_target_properties(${PROJECT_NAME} PROPERTIES
  BUILD_RPATH "\$ORIGIN/lib"
  INSTALL_RPATH "\$ORIGIN/lib"
)

add_library(rga SHARED IMPORTED GLOBAL)
set_target_properties(rga PROPERTIES
  IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/lib/librga.so"
)

if(ENABLE_RKNN)
  add_library(rknnrt SHARED IMPORTED GLOBAL)
  set_target_properties(rknnrt PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/lib/librknnrt.so"
  )
endif()

find_package(Threads REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE
  rga
  Threads::Threads
  dl
  m
  rt
)

if(ENABLE_RKNN)
  target_link_libraries(${PROJECT_NAME} PRIVATE rknnrt)
endif()

if(ENABLE_OPENCV)
  set(OPENCV_PREFIX "${CMAKE_SOURCE_DIR}/third_party/opencv-linux/rk356x-glibc-aarch64")
  set(OpenCV_DIR "${OPENCV_PREFIX}/lib/cmake/opencv4")
  find_package(OpenCV REQUIRED COMPONENTS core imgproc dnn)

  target_include_directories(${PROJECT_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party
)

if(ENABLE_LVGL)
  target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/lvgl
    ${CMAKE_SOURCE_DIR}/third_party/lvgl/src
    ${CMAKE_SOURCE_DIR}/third_party/lv_drivers
    ${CMAKE_SOURCE_DIR}/ui
  )

  find_path(DRM_INCLUDE_XF86 xf86drm.h)
  find_path(DRM_INCLUDE_FOURCC drm_fourcc.h PATH_SUFFIXES libdrm)

  if(NOT DRM_INCLUDE_XF86)
    message(FATAL_ERROR "Cannot find xf86drm.h in sysroot/include. Check Buildroot sysroot and include paths.")
  endif()
  if(NOT DRM_INCLUDE_FOURCC)
    message(FATAL_ERROR "Cannot find drm_fourcc.h (often under include/libdrm). Check Buildroot sysroot.")
  endif()

  find_library(DRM_LIB drm)
  if(NOT DRM_LIB)
    message(FATAL_ERROR "Cannot find libdrm (libdrm.so / libdrm.a) in sysroot/lib.")
  endif()

  target_include_directories(${PROJECT_NAME} PRIVATE
    ${DRM_INCLUDE_XF86}
    ${DRM_INCLUDE_FOURCC}
  )

  target_link_libraries(${PROJECT_NAME} PRIVATE
    ${DRM_LIB}
  )

  target_compile_definitions(${PROJECT_NAME} PRIVATE
    LV_CONF_INCLUDE_SIMPLE=1
    LV_LVGL_H_INCLUDE_SIMPLE=1
    USE_DRM=1
    USE_EVDEV=1
  )

  file(GLOB_RECURSE LVGL_SOURCES
    ${CMAKE_SOURCE_DIR}/third_party/lvgl/src/*.c
  )

  set(LVDRV_SOURCES
    ${CMAKE_SOURCE_DIR}/third_party/lv_drivers/display/drm.c
    ${CMAKE_SOURCE_DIR}/third_party/lv_drivers/indev/evdev.c
  )

  target_sources(${PROJECT_NAME} PRIVATE
    ${LVGL_SOURCES}
    ${LVDRV_SOURCES}
  )
endif()
cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_NETINT "Build NETINT T408 Encoder Plugin" ON)

if(NOT ENABLE_NETINT)
  target_disable_feature(obs-netint-t4xx "NETINT T408 Encoder")
  target_disable(obs-netint-t4xx)
  return()
endif()

add_library(obs-netint-t4xx MODULE)
add_library(OBS::netint-t4xx ALIAS obs-netint-t4xx)

target_sources(
  obs-netint-t4xx
  PRIVATE
    obs-netint.c
    netint-encoder.c
    netint-encoder.h
    netint-libxcoder.c
    netint-libxcoder.h
)

target_link_libraries(
  obs-netint-t4xx
  PRIVATE OBS::libobs
  $<$<PLATFORM_ID:Linux>:dl>
)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in obs-netint-t4xx.rc)
  target_sources(obs-netint-t4xx PRIVATE obs-netint-t4xx.rc)
endif()

set_target_properties_obs(obs-netint-t4xx PROPERTIES FOLDER plugins/obs-netint-t4xx PREFIX "")


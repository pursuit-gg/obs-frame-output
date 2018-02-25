cmake_minimum_required(VERSION 3.0.0)
PROJECT(frame-output)

SET(PATH_OBS_STUDIO "" CACHE PATH "OBS Studio Source Code Directory")
SET(obsPath "${PATH_OBS_STUDIO}")
INCLUDE("${PATH_OBS_STUDIO}/cmake/external/Findlibobs.cmake")

FIND_LIBRARY(LIBJPEG_LIBRARY
  NAMES jpeg
  HINTS
    ${PROJECT_SOURCE_DIR}
  PATH_SUFFIXES
    libjpeg/x64/Release
)

SET(frame-output_SOURCES
  frame-output.c)

INCLUDE_DIRECTORIES(
  "${PROJECT_SOURCE_DIR}"
  "${LIBOBS_INCLUDE_DIRS}"
)

ADD_DEFINITIONS(-D_CRT_SECURE_NO_WARNINGS)

ADD_LIBRARY(frame-output MODULE
  ${frame-output_SOURCES}
)

TARGET_LINK_LIBRARIES(frame-output
  ${LIBOBS_LIBRARIES}
  ${LIBJPEG_LIBRARY}
)

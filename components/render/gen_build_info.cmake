# Regenerated on every firmware build. Date/time are UTC so a flash can be
# identified independently of the builder's local timezone (__DATE__/__TIME__
# are local and would disagree between machines).
if(NOT DEFINED OUT)
    message(FATAL_ERROR "OUT is required")
endif()
string(TIMESTAMP BUILD_UTC "%Y-%m-%d %H:%M:%S" UTC)
file(WRITE "${OUT}" "#pragma once\n\n#define APP_BUILD_TIMESTAMP \"${BUILD_UTC} UTC\"\n")

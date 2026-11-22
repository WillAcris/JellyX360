cmake_minimum_required(VERSION 3.25)
project(jellyx360 C)

# JellyX360: a Jellyfin client for the Xbox 360, built with xsdk.
# The .xex and its log are named after the target.

add_executable(jellyx360
	src/main.c
	src/util.c
	src/json.c
	src/http.c
	src/jellyx360.c
	src/config.c
	src/ui.c
	src/image.c
	src/player.c
)
set_target_properties(jellyx360 PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(jellyx360 PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare)
target_include_directories(jellyx360 PRIVATE src)
# Third-party headers are not ours: silence their warnings so -Werror applies to our code only.
target_include_directories(jellyx360 SYSTEM PRIVATE third_party)

# ui_vert and ui_frag land in jellyx360_shaders.h.
xsdk_add_shaders(jellyx360 shaders/ui.vert shaders/ui.frag)

# TITLE_ID 0x4A463336 is "JF36"; the name is what the dashboard lists.
xsdk_add_xex(jellyx360 TITLE_ID 0x4A463336 NAME "JellyX360" VERSION 0.2.0.0)

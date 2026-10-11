define_channel_client("wwdisplay")

set(${MODULE_PREFIX}_SRCS wwdisplay_main.c)

set(${MODULE_PREFIX}_LIBS winpr)

add_channel_client_library(${MODULE_PREFIX} ${MODULE_NAME} ${CHANNEL_NAME} TRUE "DVCPluginEntry")

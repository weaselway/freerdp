set(OPTION_DEFAULT ON)
set(OPTION_CLIENT_DEFAULT ON)
set(OPTION_SERVER_DEFAULT OFF)

define_channel_options(
  NAME
  "wwdisplay"
  TYPE
  "dynamic"
  DESCRIPTION
  "Weaselway display information (the client monitor's refresh rate)"
  SPECIFICATIONS
  "weaselway"
  DEFAULT
  ${OPTION_DEFAULT}
  CLIENT_DEFAULT
  ${OPTION_CLIENT_DEFAULT}
  SERVER_DEFAULT
  ${OPTION_SERVER_DEFAULT}
)

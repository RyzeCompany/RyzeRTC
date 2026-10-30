PHP_ARG_WITH([ryzertc],
  [for RyzeRTC native WebRTC support],
  [AS_HELP_STRING([--with-ryzertc=DIR], [Build RyzeRTC with installed libdatachannel headers and libraries])],
  [no])

if test "$PHP_RYZERTC" != "no"; then
  if test "$PHP_RYZERTC" = "yes"; then
    AC_MSG_ERROR([Use --with-ryzertc=/absolute/path/to/libdatachannel/install])
  fi
  if test ! -f "$PHP_RYZERTC/include/rtc/rtc.h"; then
    AC_MSG_ERROR([Missing rtc/rtc.h at $PHP_RYZERTC/include])
  fi
  if test ! -f "$PHP_RYZERTC/lib/libdatachannel.so"; then
    AC_MSG_ERROR([Missing libdatachannel.so at $PHP_RYZERTC/lib])
  fi
  PHP_ADD_INCLUDE([$PHP_RYZERTC/include])
  PHP_ADD_LIBRARY_WITH_PATH([datachannel], [$PHP_RYZERTC/lib], [RYZERTC_SHARED_LIBADD])
  PHP_ADD_LIBRARY([pthread], [1], [RYZERTC_SHARED_LIBADD])
  PHP_SUBST([RYZERTC_SHARED_LIBADD])
  PHP_NEW_EXTENSION([ryzertc], [src/ryzertc.c], [$ext_shared],, [-std=c11 -pthread])
fi

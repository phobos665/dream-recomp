# The baseline title. Everything here is in cmake/DreamAddGame.cmake; this file exists so that
# adding a game is adding a directory, and so a title can still say something particular about
# itself if it ever needs to.
#
# BOOT_TEST registers crazytaxi.boot, which boots as far as the first display list. It needs the
# owner's disc image at games/crazytaxi.chd and the development interpreter, so it is absent from
# CI and from anyone else's checkout.
dream_add_game(crazytaxi TITLE "Crazy Taxi" BOOT_TEST)

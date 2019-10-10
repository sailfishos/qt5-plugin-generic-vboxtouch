TARGET = vboxtouchplugin

PLUGIN_TYPE = generic
PLUGIN_CLASS_NAME = VirtualboxTouchScreenPlugin
load(qt_plugin)

SOURCES = \
    main.cpp \
    evdevmousehandler.cpp \
    setshape.cpp \
    vboxtouch.cpp \
    zoomindicator.cpp

HEADERS = \
    evdevmousehandler.h \
    vboxtouch.h \
    zoomindicator.h

OTHER_FILES += vboxtouch.json

QT += core gui-private quick

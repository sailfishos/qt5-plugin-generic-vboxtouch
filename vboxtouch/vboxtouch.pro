TARGET = vboxtouchplugin

TEMPLATE = lib
CONFIG += plugin

# Not load(qt_plugin): that is for plugins built inside Qt, and the Qt 6
# installers no longer ship it.
target.path = $$[QT_INSTALL_PLUGINS]/generic
INSTALLS += target

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

QT += core core-private gui-private quick

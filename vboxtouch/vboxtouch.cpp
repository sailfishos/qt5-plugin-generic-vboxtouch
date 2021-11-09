/****************************************************************************
**
** Copyright (c) 2019 Open Mobile Platform LLС
** Copyright (C) 2013 Jolla Ltd.
** Contact: Richard Braakman <richard.braakman@jollamobile.com>
**
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
****************************************************************************/

#include "vboxtouch.h"

#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSocketNotifier>
#include <QStringList>
#include <QTouchDevice>
#include <QtQuick/QQuickWindow>

#include <qpa/qwindowsysteminterface.h>

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/fb.h>

#include "zoomindicator.h"
#include "evdevmousehandler.h"

extern bool set_pointer_shape_ioctl(int fd); // from setshape.cpp

// The reported x and y coordinates are in the range 0 to 65535 (0xffff)
#define VBOX_COORD_MAX 65535

/*
 * I looked at virtualbox source code to figure out the interface of
 * the vboxguest ioctls used here, but I didn't copy any of that code.
 *   Richard Braakman
 */

// Give up after this many ioctl failures in a row
#define MAX_PERMITTED_FAILURES 5

struct vbox_header {
    uint32_t size;
    uint32_t version;
    uint32_t type;
    int32_t rc;
    uint32_t out;
    uint32_t reserved2;
};

struct vbox_mouse_status_request {
    uint32_t size;
    uint32_t version;
    uint32_t type;
    int32_t rc;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t features;
    int32_t x;
    int32_t y;
};
const static vbox_mouse_status_request blank_mouse_status_request = {
    sizeof(vbox_mouse_status_request), // should be 36
    0x10001, // request version
    1, // type: get mouse status
    -1, // rc: pre-emptive error code
    0, 0, 0, 0, 0
};
// flag values in 'features'
// flags sent:
#define VBOXMOUSE_WANT_ABSOLUTE 1
#define VBOXMOUSE_HOST_DRAWS_CURSOR 4
#define VBOXMOUSE_NEW_PROTOCOL 16
// flags received:
#define VBOXMOUSE_IS_ABSOLUTE 2

struct vbox_set_mouse_status {
    uint32_t size;
    uint32_t version;
    uint32_t type;
    int32_t rc;
    uint32_t out;
    uint32_t reserved2;
    union {
        struct {
            /** Mouse status flags (VBOXMOUSE_XXX). */
            uint32_t features;
        } in;
    } u;
};
const static vbox_set_mouse_status set_mouse_status = {
    sizeof(vbox_set_mouse_status),
    0x10001, // request version
    0, // type: default
    -1, // rc: pre-emptive error code
    sizeof(vbox_header), 0, { VBOXMOUSE_WANT_ABSOLUTE | VBOXMOUSE_NEW_PROTOCOL | VBOXMOUSE_HOST_DRAWS_CURSOR }
};

QRect screenGeometryFromFramebuffer()
{
    const QSize uiSize(QGuiApplication::primaryScreen()->size());
    QRect defaultGeometry(QPoint(), uiSize);

    /* Read framebuffer dimentions from device
     * in case of VM resolution in VM extra data (CustomVideoMode1, GUI/LastGuestSizeHint)
     * is different than resolution reported by QGuiApplication::primaryScreen()->geometry().size()
     */
    const QString fbDevice = "/dev/fb0";
    const int fh = open(fbDevice.toLocal8Bit().constData(), O_RDONLY);
    if (fh < 0) {
        qWarning("vboxtouch: cannot open framebuffer %s: %s", qPrintable(fbDevice), strerror(errno));
        return defaultGeometry;
    }

    fb_var_screeninfo var;
    const int err = ioctl(fh, FBIOGET_VSCREENINFO, &var);
    close(fh);
    if (err != 0) {
        qWarning("vboxtouch: framebuffer ioctl error: %s", strerror(errno));
        return defaultGeometry;
    }

    const int scale = qMax(1, qEnvironmentVariableIntValue("QT_QPA_EGLFS_SCALE"));
    const QSize fbSize(var.xres * scale, var.yres * scale);

    qInfo("vboxtouch: ui size: %d,%d", uiSize.width(), uiSize.height());
    qInfo("vboxtouch: fb size: %d,%d", fbSize.width(), fbSize.height());

    /*
     * Relative framebuffer and device screen positions
     * (screen orientation is irrelevant):
     *   0,0
     *     |-------------W
     *     | framebuffer |
     *     |--------     |
     *     | screen |    |
     *     |        |    |
     *     |        |    |
     *     H--------|----|
     *           uiSize fbSize
     */

    return uiSize == fbSize
            ? defaultGeometry
            : QRect(QPoint(0, uiSize.height() - fbSize.height()), fbSize);
}

QPointF screenPointToDevicePoint(const QPointF &p, const QRect &screen)
{
    const qreal normal_x = (p.x() - screen.x()) / (screen.width() - 1);
    const qreal normal_y = (p.y() - screen.y()) / (screen.height() - 1);
    return QPointF(normal_x * VBOX_COORD_MAX, normal_y * VBOX_COORD_MAX);
}

QPointF devicePointToScreenPoint(const QPointF &p, const QRect &screen)
{
    const qreal normal_x = p.x() / VBOX_COORD_MAX;
    const qreal normal_y = p.y() / VBOX_COORD_MAX;
    return QPointF(normal_x * (screen.width() - 1) + screen.x(),
                   normal_y * (screen.height() - 1) + screen.y());
}

QWindowSystemInterface::TouchPoint createTouchPoint(const QPointF &p, Qt::TouchPointState state, bool pressed, const QRect &screen)
{
    const qreal normal_x = p.x() / VBOX_COORD_MAX;
    const qreal normal_y = p.y() / VBOX_COORD_MAX;

    QWindowSystemInterface::TouchPoint tp;
    tp.pressure = pressed ? 1 : 0;
    tp.state = state;
    tp.normalPosition = QPointF(normal_x, normal_y);
    tp.area = QRectF(0, 0, 4, 4);

    tp.area.moveCenter(devicePointToScreenPoint(p, screen));
    tp.rawPositions.append(p);

    return tp;
}

int VirtualboxTouchScreenHandler::s_quitSignalFd = -1;

VirtualboxTouchScreenHandler::VirtualboxTouchScreenHandler(const QString &specification, QObject *parent)
    : QObject(parent), m_fd(-1), m_notifier(0), m_device(0), m_failures(0),
      m_screenGeometry(screenGeometryFromFramebuffer()),
      m_indicator(new ZoomIndicator),
      m_button(false), m_x(0), m_y(0)
{
    setObjectName("Virtualbox Touch Handler");

    setUpSignalHandlers();

    qApp->installEventFilter(this);

    connect(qApp, &QGuiApplication::focusWindowChanged, [this](QWindow *w) {
        if (w == nullptr) return;

        auto quickWindow = static_cast<QQuickWindow *>(w);
        m_indicator->setParentItem(quickWindow->contentItem());
        m_indicator->setPosition(quickWindow->position());
        m_indicator->setWidth(quickWindow->width());
        m_indicator->setHeight(quickWindow->height());
    });

    QString device_name = QString::fromLocal8Bit(qgetenv("VIRTUALBOX_TOUCH_GUEST_DEVICE"));
    // The specification can override the device set in the environment
    Q_FOREACH (QString option, specification.split(':')) {
        if (option.startsWith("vboxguest=")) {
            device_name = option.section('=', 1);
            break;
        }
    }
    // Default device name if nothing was set
    if (device_name.isEmpty())
        device_name = "/dev/vboxguest";
    qDebug("vboxtouch: Using vbox device %s", qPrintable(device_name));

    // Open the connection to the vboxguest device.
    // It will alert us about mouse events by asserting it is ready for reading.
    m_fd = open(device_name.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
    if (m_fd < 0) {
        qWarning("vboxtouch: cannot open %s: %s", qPrintable(device_name), strerror(errno));
        return;
    }

    // We have to set a shape before HOST_DRAWS_CURSOR will work
    set_pointer_shape_ioctl(m_fd);

    // Tell vboxguest our desired feature flags
    vbox_set_mouse_status features = set_mouse_status;
    int err = ioctl(m_fd, _IOWR('V', 15, features), &features);
    if (err != 0) {
        if (err < 0)
            qWarning("vboxtouch init: ioctl error: %s", strerror(errno));
        else
            qWarning("vboxtouch init: vboxguest error %d", err);
        shutdown();
        return;
    }

    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, SIGNAL(activated(int)), this, SLOT(handleInput()));

    QString evdev_device = QString::fromLocal8Bit(qgetenv("VIRTUALBOX_TOUCH_EVDEV_MOUSE"));
    Q_FOREACH (QString option, specification.split(':')) {
        if (option.startsWith("evdev=")) {
            evdev_device = option.section('=', 1);
            break;
        }
        // syntax compatible with evdev plugins
        if (option.startsWith("/dev/")) {
            device_name = option;
            break;
        }
    }
    // Default device name if nothing was set, based on Virtualbox's default.
    if (evdev_device.isEmpty())
        evdev_device = "/dev/input/by-path/platform-i8042-serio-1-event-mouse";
    qDebug("vboxtouch: Using evdev device %s", qPrintable(evdev_device));

    m_mouse = EvdevMouseHandler::create(evdev_device, specification);
    if (!m_mouse) {
        qWarning("vboxtouch init: cannot open evdev mouse %s", qPrintable(evdev_device));
        shutdown();
        return;
    }
    connect(m_mouse, SIGNAL(handleMouseEvent(int,int,Qt::MouseButtons)), this, SLOT(handleEvdevInput(int,int,Qt::MouseButtons)));

    m_device = new QTouchDevice;
    m_device->setName(QFileInfo(device_name).fileName());
    m_device->setType(QTouchDevice::TouchScreen);
    m_device->setCapabilities(QTouchDevice::Position);
    QWindowSystemInterface::registerTouchDevice(m_device);
}

VirtualboxTouchScreenHandler::~VirtualboxTouchScreenHandler()
{
    shutdown();
    // Cannot delete m_device because registerTouchDevice() holds a pointer.

    delete m_indicator;

    if (s_quitSignalFd >= 0) {
        ::close(s_quitSignalFd);
        s_quitSignalFd = -1;
    }
}

void VirtualboxTouchScreenHandler::setUpSignalHandlers()
{
    s_quitSignalFd = ::eventfd(0, 0);
    if (s_quitSignalFd == -1) {
        qWarning("Failed to create eventfd object for signal handling");
        return;
    }

    m_quitSignalNotifier.reset(new QSocketNotifier(s_quitSignalFd, QSocketNotifier::Read, this));
    connect(m_quitSignalNotifier.data(), &QSocketNotifier::activated, qApp, [] {
        uint64_t tmp;
        ssize_t unused = ::read(s_quitSignalFd, &tmp, sizeof(tmp));
        Q_UNUSED(unused);

        qDebug("Signal handled - now exit");
        QCoreApplication::exit(0);
    });

    // We need to catch the SIGTERM and SIGINT signals, so that we can do a
    // proper shutdown of Qt and the lipstick, and avoid crashes, hangs and
    // reboots (e.g. during user switching).
    // This actions is similar to actions in the qt5-qpa-hwcomposer-plugin
    // that is not exists in the emulator so we doing it here.
    struct sigaction action;
    action.sa_handler = quitSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    action.sa_flags |= SA_RESTART;
    if (sigaction(SIGTERM, &action, NULL))
        qWarning("Failed to set up SIGINT handling");
    if (sigaction(SIGINT, &action, NULL))
        qWarning("Failed to set up SIGTERM handling");
}

void VirtualboxTouchScreenHandler::quitSignalHandler(int sig)
{
    uint64_t a = 1;
    ssize_t unused = ::write(s_quitSignalFd, &a, sizeof(a));
    Q_UNUSED(unused);

    qDebug("Exiting on signal: %d", sig);
}

void VirtualboxTouchScreenHandler::shutdown()
{
    qDebug("shutting down vboxtouch");
    if (m_notifier) {
        delete m_notifier;
        m_notifier = 0;
    }
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
    if  (m_mouse) {
        delete m_mouse;
        m_mouse = 0;
    }
}

void VirtualboxTouchScreenHandler::handleInput()
{
    // clear the input event
    char c;
    if (read(m_fd, &c, 1) < 0) {
        if (errno != EINTR && errno != EAGAIN) {
            qWarning("vboxtouch: read error: %s", strerror(errno));
            shutdown();
            return;
        }
    }

    // get the real input via ioctl
    vbox_mouse_status_request request = blank_mouse_status_request;
    int err = ioctl(m_fd, _IOWR('V', 3, request), &request);
    if (err != 0) {
        if (err < 0)
            qWarning("vboxtouch: ioctl error: %s", strerror(errno));
        else
            qWarning("vboxtouch: vboxguest error %d", err);
        if (++m_failures > MAX_PERMITTED_FAILURES)
            shutdown(); // we might be in a high speed error loop, so stop
        return;
    }

    if (!(request.features & VBOXMOUSE_IS_ABSOLUTE)) {
        qWarning("vboxtouch: need absolute coordinates but did not get them");
        if (++m_failures > MAX_PERMITTED_FAILURES)
            shutdown();
        return;
    }

    m_failures = 0; // success resets the counter

    bool moved = m_x != request.x || m_y != request.y;
    m_x = request.x;
    m_y = request.y;
    if (m_button) {
        reportTouch(moved ? Qt::TouchPointMoved : Qt::TouchPointStationary);
    }

    if (m_indicator->isActive())
        m_indicator->moveTo(devicePointToScreenPoint(QPointF(m_x, m_y), m_screenGeometry));
}

void VirtualboxTouchScreenHandler::handleEvdevInput(int x, int y, Qt::MouseButtons buttons)
{
    Q_UNUSED(x);
    Q_UNUSED(y);
    // Only the left button counts as a touch
    bool button = (buttons & Qt::LeftButton) != 0;
    if (button != m_button) {
        m_button = button;
        reportTouch(m_button ? Qt::TouchPointPressed : Qt::TouchPointReleased);
    }
}

void VirtualboxTouchScreenHandler::reportTouch(Qt::TouchPointState state)
{
    QList<QWindowSystemInterface::TouchPoint> touchpoints;

    if (m_indicator->isActive()) {
        // Emulate multitouch: pinch in/out gesture
        const QPointF p1 = screenPointToDevicePoint(m_indicator->p1(), m_screenGeometry);
        const QPointF p2 = screenPointToDevicePoint(m_indicator->p2(), m_screenGeometry);
        touchpoints << createTouchPoint(p1, state, m_button, m_screenGeometry)
                    << createTouchPoint(p2, state, m_button, m_screenGeometry);
    } else {
        touchpoints << createTouchPoint(QPointF(m_x, m_y), state, m_button, m_screenGeometry);
    }

    QWindowSystemInterface::handleTouchEvent(0, m_device, touchpoints);
}

bool VirtualboxTouchScreenHandler::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Control && !m_indicator->isActive())
            m_indicator->setAnchor(devicePointToScreenPoint(QPoint(m_x, m_y), m_screenGeometry));
    }
    if (event->type() == QEvent::KeyRelease) {
        QKeyEvent* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Control)
            m_indicator->reset();
    }
    return QObject::eventFilter(obj, event);
}

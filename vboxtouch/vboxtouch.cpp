/****************************************************************************
**
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
#include <QSocketNotifier>
#include <QStringList>
#include <QTouchDevice>

#include <qpa/qwindowsysteminterface.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "evdevmousehandler.h"

/*
 * I looked at virtualbox source code to figure out the interface of
 * the vboxguest ioctls used here, but I didn't copy any of that code.
 *   Richard Braakman
 */

// Give up after this many ioctl failures in a row
#define MAX_PERMITTED_FAILURES 5

struct vbox_mouse_status_request {
    uint32_t size;
    uint32_t version;
    uint32_t type;
    uint32_t rc;
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

VirtualboxTouchScreenHandler::VirtualboxTouchScreenHandler(const QString &specification, QObject *parent)
    : QObject(parent), m_fd(-1), m_notifier(0), m_device(0), m_failures(0),
      m_x(0), m_y(0), m_button(false)
{
    setObjectName("Virtualbox Touch Handler");

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

    // Tell vboxguest our desired feature flags
    uint32_t features = VBOXMOUSE_WANT_ABSOLUTE | VBOXMOUSE_NEW_PROTOCOL | VBOXMOUSE_HOST_DRAWS_CURSOR;
    int err = ioctl(m_fd, _IOWR('V', 10, features), &features);
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
    // Default device name if nothing was set
    if (evdev_device.isEmpty())
        evdev_device = "/dev/input/mouse0";
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
    QWindowSystemInterface::TouchPoint tp;
    tp.pressure = m_button ? 1 : 0;
    tp.state = state;
    tp.area = QRectF(m_x, m_y, 0, 0);

    QList<QWindowSystemInterface::TouchPoint> touchpoints;
    touchpoints << tp;
    QWindowSystemInterface::handleTouchEvent(0, m_device, touchpoints);
}

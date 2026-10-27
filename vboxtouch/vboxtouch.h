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

#ifndef VBOXTOUCH_H
#define VBOXTOUCH_H

#include <QObject>
#include <QPoint>
#include <QString>
#include <QRect>
#include <QEvent>
#include <QHash>
#include <QList>
#include <QStringList>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QEventPoint>
class QPointingDevice;
typedef QPointingDevice VboxTouchDevice;
typedef QEventPoint::State VboxTouchPointState;
#else
class QTouchDevice;
typedef QTouchDevice VboxTouchDevice;
typedef Qt::TouchPointState VboxTouchPointState;
#endif

class QSocketNotifier;

class EvdevMouseHandler;
class ZoomIndicator;

class VirtualboxTouchScreenHandler : public QObject
{
    Q_OBJECT

public:
    explicit VirtualboxTouchScreenHandler(const QString &specification, QObject *parent = nullptr);
    ~VirtualboxTouchScreenHandler();

private slots:
    void handleInput(); // connected to m_notifier
    void shutdown();
    void handleEvdevInput(int x, int y, bool abs, Qt::MouseButtons buttons,
                          Qt::MouseButton button, QEvent::Type type);
    void handleEvdevWheel(QPoint delta);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    // Where the position comes from: vboxguest's integrated pointer, or an
    // absolute evdev device (VirtualBox's USB tablet or its touch screen).
    enum Mode { VboxGuestMode, EvdevMode };
    Mode m_mode;

    int m_fd;
    QSocketNotifier *m_notifier;
    VboxTouchDevice *m_device;
    QList<EvdevMouseHandler *> m_mice;
    // The buttons each of m_mice holds down: each reports only its own.
    QHash<QObject *, Qt::MouseButtons> m_miceButtons;
    int m_failures;

    QRect m_screenGeometry; // x,y - ui offset relative to framebuffer; size - framebuffer size
    ZoomIndicator *m_indicator;

    static int s_quitSignalFd;
    QScopedPointer<QSocketNotifier> m_quitSignalNotifier;

    // Last known mouse state
    bool m_button;
    int m_x;
    int m_y;
    bool m_positionKnown; // m_x, m_y came from a device, not from the constructor
    bool m_draining; // taking in what the other devices queued
    void reportTouch(VboxTouchPointState state);

    void initVboxGuest(const QStringList &options, const QString &specification);
    void initEvdev(const QStringList &options, const QString &specification);
    void registerTouchDevice(const QString &name);
    QRect screenGeometry() const;

    void setUpSignalHandlers();
    static void quitSignalHandler(int sig);
};

#endif

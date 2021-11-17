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
#include <QString>
#include <QRect>

class QSocketNotifier;
class QTouchDevice;

class EvdevMouseHandler;
class ZoomIndicator;

class VirtualboxTouchScreenHandler : public QObject
{
    Q_OBJECT

public:
    explicit VirtualboxTouchScreenHandler(const QString &specification, QObject *parent = 0);
    ~VirtualboxTouchScreenHandler();

private slots:
    void handleInput(); // connected to m_notifier
    void shutdown();
    void handleEvdevInput(int x, int y, Qt::MouseButtons buttons);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    int m_fd;
    QSocketNotifier *m_notifier;
    QTouchDevice *m_device;
    EvdevMouseHandler *m_mouse;
    int m_failures;

    QRect m_screenGeometry; // x,y - ui offset relative to framebuffer; size - framebuffer size
    ZoomIndicator *m_indicator;

    static int s_quitSignalFd;
    QScopedPointer<QSocketNotifier> m_quitSignalNotifier;

    // Last known mouse state
    bool m_button;
    int m_x;
    int m_y;
    void reportTouch(Qt::TouchPointState state);

    void setUpSignalHandlers();
    static void quitSignalHandler(int sig);
};

#endif

/****************************************************************************
**
** Copyright (c) 2019 Open Mobile Platform LLС
** Contact: Sergey Levin <s.levin@omprussia.ru>
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

#include "zoomindicator.h"

#include <QPainter>
#include <QPen>

static const int radius = 30;
static const int length = 3;
static const QColor color1(0, 64, 255, 120);
static const QColor color2(0, 64, 255, 60);
static const QGradientStops gradientStops = {qMakePair(0, color1), qMakePair(1, color2)};

ZoomIndicator::ZoomIndicator(QQuickItem *parent):
    QQuickPaintedItem(parent)
{
}

void ZoomIndicator::reset()
{
    m_anchor = QPoint();
    m_p1 = QPoint();
    m_p2 = QPoint();
    setVisible(false);
}

bool ZoomIndicator::isActive() const
{
    return !m_anchor.isNull() && !m_p1.isNull() && !m_p2.isNull();
}

QPointF ZoomIndicator::p1() const
{
    return m_p1;
}

QPointF ZoomIndicator::p2() const
{
    return m_p2;
}

QPointF ZoomIndicator::anchor() const
{
    return m_anchor;
}

void ZoomIndicator::moveTo(const QPointF &position)
{
    setVisible(true);
    m_p2 = position;
    m_p1 = QPoint(2 * m_anchor.x() - m_p2.x(), 2 * m_anchor.y() - m_p2.y());
    update();
}

void ZoomIndicator::setAnchor(const QPointF &anchor)
{
    setVisible(true);
    m_anchor = anchor;
    m_p1 = anchor;
    m_p2 = anchor;
    update();
}

void ZoomIndicator::paint(QPainter *painter)
{
    if (!isActive())
        return;

    QRadialGradient g1(m_p1, radius);
    g1.setStops(gradientStops);

    QRadialGradient g2(m_p2, radius);
    g2.setStops(gradientStops);

    QPainterPath p, p1, p2;
    p.addEllipse(m_anchor, radius / 2, radius / 2);
    p1.addEllipse(m_p1, radius, radius);
    p2.addEllipse(m_p2, radius, radius);

    painter->setRenderHints(QPainter::Antialiasing, true);
    painter->setPen(QPen(Qt::blue, length));

    painter->setBrush(color1);
    painter->drawPath(p);

    painter->setBrush(g1);
    painter->drawPath(p1);

    painter->setBrush(g2);
    painter->drawPath(p2);

    // Clip line behind ellipses
    QPainterPath path;
    path.addRect(painter->viewport());
    painter->setClipPath(path.subtracted(p.united(p1).united(p2)));

    painter->setPen(QPen(Qt::blue, length + 1));
    painter->drawLine(m_p1, m_p2);
}

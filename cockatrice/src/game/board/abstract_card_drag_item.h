#ifndef ABSTRACTCARDDRAGITEM_H
#define ABSTRACTCARDDRAGITEM_H

#include "abstract_card_item.h"

class QGraphicsScene;
class CardZone;
class CardInfo;

class AbstractCardDragItem : public QObject, public QGraphicsItem
{
    Q_OBJECT
    Q_INTERFACES(QGraphicsItem)
protected:
    AbstractCardItem *item;
    QPointF hotSpot;
    QList<AbstractCardDragItem *> childDrags;

public:
    enum
    {
        Type = typeCardDrag
    };
    int type() const override
    {
        return Type;
    }
    AbstractCardDragItem(AbstractCardItem *_item, const QPointF &_hotSpot, AbstractCardDragItem *parentDrag = 0);
    QRectF boundingRect() const override
    {
        return QRectF(0, 0, CARD_WIDTH, CARD_HEIGHT);
    }
    QPainterPath shape() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;
    AbstractCardItem *getItem() const
    {
        return item;
    }
    QPointF getHotSpot() const
    {
        return hotSpot;
    }
    void addChildDrag(AbstractCardDragItem *child);
    virtual void updatePosition(const QPointF &cursorScenePos) = 0;

protected:
    void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override;
};

#endif

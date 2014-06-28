#include "editortoolbar.h"

EditorToolBar::EditorToolBar(QWidget *parent) :
    QToolBar(parent)
{
    int defaultColor = this->palette().background().color().lightness();

    buttonNew = new QToolButton;
    buttonOpen = new QToolButton;
    buttonSave = new QToolButton;
    buttonZoomIn = new QToolButton;
    buttonZoomOut = new QToolButton;

    if(defaultColor > 165)
    {
            buttonNew->setIcon(QIcon("://images/blackNew.png"));
            buttonOpen->setIcon(QIcon("://images/Open-32(1).png"));
            buttonSave->setIcon(QIcon("://images/Save-32.png"));
	    buttonZoomIn->setIcon(QIcon("://images/zoomin.png"));
	    buttonZoomOut->setIcon(QIcon("://images/zoomout.png"));
     } else {

            buttonNew->setIcon(QIcon("://images/Document-New-128.png"));
            buttonOpen->setIcon(QIcon("://images/Open-128.png"));
            buttonSave->setIcon(QIcon("://images/Save-128.png"));
     } 

    buttonNew->setToolTip("New");
    buttonOpen->setToolTip("Open");
    buttonSave->setToolTip("Save");
    buttonZoomIn->setToolTip("Zoom In");
    buttonZoomOut->setToolTip("Zoom Out");

    this->addWidget(buttonNew);
    this->addWidget(buttonOpen);
    this->addWidget(buttonSave);
    this->addWidget(buttonZoomIn);
    this->addWidget(buttonZoomOut);

}

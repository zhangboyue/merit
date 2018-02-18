// Copyright (c) 2016-2017 The Merit Foundation developers
// Copyright (c) 2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "modaloverlay.h"
#include "ui_modaloverlay.h"

#include "guiutil.h"

#include "chainparams.h"

#include <QResizeEvent>
#include <QPropertyAnimation>

//Defined in validation
extern std::atomic_bool fImporting;

ModalOverlay::ModalOverlay(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ModalOverlay),
    bestHeaderHeight(0),
    bestHeaderDate(QDateTime()),
    layerIsVisible(false),
    userClosed(false)
{
    ui->setupUi(this);
    ui->closeButton->setEnabled(false);
    ui->closeButton->setHidden(true);

    connect(ui->closeButton, SIGNAL(clicked()), this, SLOT(closeClicked()));

    #ifdef ALLOW_HIDE_SYNC
        allowHide();
    #endif

    if (parent) {
        parent->installEventFilter(this);
        raise();
    }

    block_time_samples.clear();
    setVisible(false);
}

ModalOverlay::~ModalOverlay()
{
    delete ui;
}

bool ModalOverlay::eventFilter(QObject * obj, QEvent * ev) {
    if (obj == parent()) {
        if (ev->type() == QEvent::Resize) {
            QResizeEvent * rev = static_cast<QResizeEvent*>(ev);
            resize(rev->size());
            if (!layerIsVisible)
                setGeometry(0, height(), width(), height());

        }
        else if (ev->type() == QEvent::ChildAdded) {
            raise();
        }
    }
    return QWidget::eventFilter(obj, ev);
}

//! Tracks parent widget changes
bool ModalOverlay::event(QEvent* ev) {
    if (ev->type() == QEvent::ParentAboutToChange) {
        if (parent()) parent()->removeEventFilter(this);
    }
    else if (ev->type() == QEvent::ParentChange) {
        if (parent()) {
            parent()->installEventFilter(this);
            raise();
        }
    }
    return QWidget::event(ev);
}

void ModalOverlay::setKnownBestHeight(int count, const QDateTime& blockDate)
{
    if (count > bestHeaderHeight) {
        bestHeaderHeight = count;
        bestHeaderDate = blockDate;
    }
}

void ModalOverlay::tipUpdate(int count, const QDateTime& blockDate)
{
    QDateTime currentDate = QDateTime::currentDateTime();
    setKnownBestHeight(count, blockDate);

    //We want to change progress text if importing so the
    //user knows we are in reindexing stage. 
    static bool prev_importing = false; 
    if(prev_importing != fImporting) {
        if(fImporting) {
            ui->labelSyncDone->setText(tr("Reindexing Progress"));
        } else {
            ui->labelSyncDone->setText(tr("Download Progress"));
        }
        prev_importing = fImporting;
    }


    // keep a vector of samples of verification progress at height
    double verificationProgress = bestHeaderHeight == 0 ? 0 : 
        static_cast<double>(count) / static_cast<double>(bestHeaderHeight);

    qint64 current_millis = currentDate.toMSecsSinceEpoch();
    block_time_samples.push_front(qMakePair(current_millis, count));

    // show progress speed if we have more then one sample
    if (block_time_samples.size() == AVG_WINDOW_LENGTH)
    {
        qint64 time_delta = 0;
        qint64 remaining_msecs = 0;

        QPair<qint64, double> sample = block_time_samples.takeLast();
        time_delta = current_millis - sample.first;

        const int blocks_delta = count - sample.second;
        if(blocks_delta > 0) {

            const int blocks_per_hour = static_cast<int>(blocks_delta/static_cast<double>(time_delta)*1000*3600);
            remaining_msecs = (bestHeaderHeight - count) * time_delta / blocks_delta;

            // show progress increase per hour
            ui->blocksPerH->setText(QString::number(blocks_per_hour)+tr(" (blocks/h)"));

            // show expected remaining time
            ui->expectedTimeLeft->setText(GUIUtil::formatNiceTimeOffset(remaining_msecs/1000));
        }
    }

    // show the last block date
    ui->newestBlockDate->setText(blockDate.toString());

    // show the percentage done according to verificationProgress
    if(bestHeaderHeight == 0) {
        ui->percentageProgress->setText(tr("Connecting..."));
    } else {
        ui->percentageProgress->setText(QString::number(verificationProgress*100, 'f', 2)+"%");
    }

    ui->progressBar->setValue(verificationProgress*100);

    if (!bestHeaderDate.isValid())
        // not syncing
        return;

    // estimate the number of headers left based on nPowTargetSpacing
    // and check if the gui is not aware of the best header (happens rarely)
    int estimateNumHeadersLeft = bestHeaderDate.secsTo(currentDate) / Params().GetConsensus().nPowTargetSpacing;
    bool hasBestHeader = bestHeaderHeight >= count;

    // show remaining number of blocks
   const auto blocks_left = bestHeaderHeight - count;
   if (estimateNumHeadersLeft < HEADER_HEIGHT_DELTA_SYNC && hasBestHeader) {
       ui->numberOfBlocksLeft->setText(tr("%1 out of %2 left...").arg(blocks_left).arg(bestHeaderHeight));
   } else {
       if(fImporting) {
           ui->numberOfBlocksLeft->setText(tr("%1 out of %2 left...").arg(blocks_left).arg(bestHeaderHeight));
       } else {
           ui->numberOfBlocksLeft->setText(tr("Unknown. Syncing Headers (%1)...").arg(bestHeaderHeight));
       }
   }
}

void ModalOverlay::toggleVisibility()
{
    showHide(layerIsVisible, true);
    if (!layerIsVisible)
        userClosed = true;
}

void ModalOverlay::showHide(bool hide, bool userRequested)
{
    if (
            (layerIsVisible && !hide) || 
            (!layerIsVisible && hide) || 
            (!hide && userClosed && !userRequested))
        return;

    if (!isVisible() && !hide)
        setVisible(true);

    setGeometry(0, hide ? 0 : height(), width(), height());

    QPropertyAnimation* animation = new QPropertyAnimation(this, "pos");
    animation->setDuration(300);
    animation->setStartValue(QPoint(0, hide ? 0 : this->height()));
    animation->setEndValue(QPoint(0, hide ? this->height() : 0));
    animation->setEasingCurve(QEasingCurve::OutQuad);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
    layerIsVisible = !hide;
}

void ModalOverlay::closeClicked()
{
    showHide(true);
    userClosed = true;
}

void ModalOverlay::allowHide()
{
    ui->closeButton->setEnabled(true);
    ui->closeButton->setHidden(false);
    canHide = true;
}

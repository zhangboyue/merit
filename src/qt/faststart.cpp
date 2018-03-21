#include "faststart.h"
#include "ui_faststart.h"
#include "util.h"

#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QTimer>
#include <QStringList>
#include <QProgressBar>
#include <QUrl>
#include <archive.h>
#include <archive_entry.h>

namespace 
{
    const int SLIDE_TRANSITION_SECONDS = 15;
    const int ERROR_WAIT = 5 * 1000; //Wait 5 seconds to quit if there was an error
    const std::string DEFAULT_URL_URL = "https://mempko.com/merit/current";
}

QString StatusText(QString status)
{
    return 
        QString{"<html><head/><body><p align=\"center\"><span style=\" color:#7a90a7;\">"} +
        status +
        QString{"</span></p></body></html>"};
}

QString ErrorText(QString status)
{
    return 
        QString{"<html><head/><body><p align=\"center\"><span style=\" color:red;\">"} +
        status +
        QString{"</span></p></body></html>"};
}

int CopyData(struct archive *ar, struct archive *aw)
{
  const void* buff = nullptr;
  size_t size = 0;
  la_int64_t offset = 0;

  while (true) {
    auto r = archive_read_data_block(ar, &buff, &size, &offset);
    if (r == ARCHIVE_EOF) {
      return ARCHIVE_OK;
    }

    if (r < ARCHIVE_OK) {
      return (r);
    }

    r = archive_write_data_block(aw, buff, size, offset);

    if (r < ARCHIVE_OK) {
      return r;
    }
  }
}

void ArchiveReadDelete(archive* a)
{
    archive_read_close(a);
    archive_read_free(a);
}

void ArchiveWriteDelete(archive* a)
{
    archive_write_close(a);
    archive_write_free(a);
}

bool ExtractArchive(QLabel* status, const std::string& snapshot, const std::string& dest)
{
    std::unique_ptr<archive, decltype(&ArchiveReadDelete)> a{
        archive_read_new(), &ArchiveReadDelete};

    std::unique_ptr<archive, decltype(&ArchiveWriteDelete)> ext{
        archive_write_disk_new(), &ArchiveWriteDelete};

    archive_read_support_format_all(a.get());
    archive_read_support_compression_all(a.get());

    int flags = ARCHIVE_EXTRACT_TIME
        | ARCHIVE_EXTRACT_PERM
        | ARCHIVE_EXTRACT_ACL
        | ARCHIVE_EXTRACT_FFLAGS;

    if(archive_read_open_filename(a.get(), snapshot.c_str(), 10240)) {
        return false;
    }

    while(true) {
        archive_entry* entry = nullptr;
        auto r = archive_read_next_header(a.get(), &entry);
        if (r == ARCHIVE_EOF) {
            break;
        }

        if (r < ARCHIVE_OK) {
            return false;
        }

        if (r < ARCHIVE_WARN) {
            return false;
        }

        std::string path = archive_entry_pathname(entry);
        status->setText(StatusText(QString{"Extracting: "} + QString::fromStdString(path)));

        std::string dest_path = dest + path;
        archive_entry_set_pathname(entry, dest_path.c_str());

        r = archive_write_header(ext.get(), entry);
        if (r < ARCHIVE_OK) {
            return false;
        } 

        r = CopyData(a.get(), ext.get());
        if (r < ARCHIVE_OK) {
            return false;
        }

        if (r < ARCHIVE_WARN) {
            return false;
        }

        r = archive_write_finish_entry(ext.get());
        if (r < ARCHIVE_OK) {
            return false;
        }

        if (r < ARCHIVE_WARN) {
            return false;
        }
    }
    return true;
}

bool FastStart::DoDownloadSnapshot()
{
    QSettings settings;
    const auto data_dir = QString::fromStdString(gArgs.GetArg("-datadir", GetDefaultDataDir().string()));
    const auto state = settings.value("snapshotstate", 0).toInt();
    if(state == SnapshotInfo::DONE || QFile::exists(data_dir + "/wallet.dat")) {
        settings.setValue("snapshotstate", static_cast<int>(SnapshotInfo::DONE));
        return true;
    }

    FastStart fastart{data_dir};
    if(!fastart.exec()) {
        /* Cancel clicked */
        return false;
    }
    return true;
}

FastStart::FastStart(const QString& data_dir,  QWidget *parent) :
    data_dir{data_dir},
    QDialog{parent},
    ui{new Ui::FastStart}
{
    ui->setupUi(this);

    Start();
}

FastStart::~FastStart()
{
    snapshot_output.close();
    delete ui;
}
void FastStart::ShowDownload()
{
    ui->stack->setCurrentIndex(1);
    connect(
            &info_manager, SIGNAL(finished(QNetworkReply*)),
            this, SLOT(SnapshotUrlDownloaded(QNetworkReply*)));

    //start slideshow
    ui->overviewSlides->setCurrentIndex(0);
    QTimer::singleShot(1000 * SLIDE_TRANSITION_SECONDS, this, SLOT(endSlide()));
}

void FastStart::ShowChoice()
{
    ui->stack->setCurrentIndex(0);
    connect(ui->snapshotButton, SIGNAL(clicked()), this, SLOT(SnapshotChoiceClicked()));
    connect(ui->peersButton, SIGNAL(clicked()), this, SLOT(PeersChoiceClicked()));
}

void FastStart::SnapshotChoiceClicked()
{
    settings.setValue("snapshotstate", static_cast<int>(SnapshotInfo::GETINFO));
    ShowDownload();
    DownloadSnapshotUrl();
}

void FastStart::PeersChoiceClicked()
{
    settings.setValue("snapshotstate", static_cast<int>(SnapshotInfo::DONE));
    accept();
}

void FastStart::Start() 
{
    ui->progressBar->setMaximum(0);
    snapshot.state = static_cast<SnapshotInfo::State>(settings.value(
            "snapshotstate", static_cast<int>(SnapshotInfo::CHOICE)).toInt());
    snapshot.url = settings.value("snapshoturl", "").toString();
    snapshot.size = settings.value("snapshotsize", qulonglong{0}).toULongLong();

    if(snapshot.state != SnapshotInfo::CHOICE) {
        ShowDownload();
    }

    switch(snapshot.state) {
        case SnapshotInfo::CHOICE: ShowChoice(); break;
        case SnapshotInfo::GETINFO: DownloadSnapshotUrl(); break;
        case SnapshotInfo::DOWNLOAD: DownloadSnapshot(); break;
        case SnapshotInfo::EXTRACT: ExtractSnapshot(); break;
        case SnapshotInfo::DONE: accept(); break;                                    
    }
}

void FastStart::DownloadSnapshotUrl() 
{
    settings.setValue("snapshotstate", static_cast<int>(SnapshotInfo::GETINFO));

    auto url_url = QString::fromStdString(gArgs.GetArg("-snapshoturl", DEFAULT_URL_URL));

    QNetworkRequest request{url_url};
    info_manager.get(request);
}

void FastStart::SnapshotUrlDownloaded(QNetworkReply* reply) 
{
    if(reply->error()) {
        ui->statusLabel->setText(
                StatusText(tr("Error Downloading Snapshot info from") + 
                " " + reply->url().toString()));
        QTimer::singleShot(ERROR_WAIT, this, SLOT(accept()));
    } else {
        snapshot.url = QString::fromStdString(reply->readAll().toStdString()).trimmed();
        DownloadSnapshot();
    }
    reply->deleteLater();
}

void FastStart::DownloadSnapshot() 
{
    settings.setValue("snapshotstate", static_cast<int>(SnapshotInfo::DOWNLOAD));

    QUrl url = snapshot.url;
    ui->statusLabel->setText(StatusText(tr("Downloading:") + " " + url.toString()));

    QString file_name = data_dir + "/snapshot.zip";
    snapshot_output.setFileName(file_name);
    if(!snapshot_output.open(QIODevice::ReadWrite)) {
        ui->statusLabel->setText(StatusText(tr("Unable to open the snapshot file")));
        QTimer::singleShot(ERROR_WAIT, this, SLOT(accept()));
        return;
    }

    snapshot.pos = snapshot_output.size();

    QNetworkRequest request{url};

    if(snapshot.pos > 0) {
        ui->progressBar->setMaximum(snapshot.size);
        ui->progressBar->setValue(snapshot.pos);

        snapshot_output.seek(snapshot.pos);
        QString range("bytes=" + QString::number(snapshot.pos) + "-");
        request.setRawHeader("Range", range.toLatin1());

    }

    if(snapshot.pos == 0 || snapshot.pos < snapshot.size) {
        download_time.start();
        snapshot_download = snapshot_manager.get(request);

        connect(snapshot_download, SIGNAL(downloadProgress(qint64,qint64)),
                SLOT(SnapshotProgress(qint64,qint64)));
        connect(snapshot_download, SIGNAL(finished()),
                SLOT(SnapshotFinished()));
        connect(snapshot_download, SIGNAL(readyRead()),
                SLOT(SnapshotReadyRead()));
    } else {
        ExtractSnapshot();
    }
}

QString ComputeUnit(double& speed) 
{
    QString unit;
    if (speed < 1024) {
        unit = "bytes/sec";
    } else if (speed < 1024*1024) {
        speed /= 1024;
        unit = "kB/s";
    } else {
        speed /= 1024*1024;
        unit = "MB/s";
    }

    return unit;
}

void FastStart::SnapshotProgress(qint64 received, qint64 size)
{
    if(snapshot.size == 0) {
        snapshot.size = size;
    }

    received += snapshot.pos;
    size += snapshot.pos;

    ui->progressBar->setMaximum(size);
    ui->progressBar->setValue(received);

    settings.setValue("snapshoturl", snapshot.url);
    settings.setValue("snapshotsize", snapshot.size);

    const double elapsed = download_time.elapsed();

    double speed = elapsed > 0 ? received * 1000.0 / elapsed : 0.0; 
    const auto unit = ComputeUnit(speed);

    ui->statusLabel->setText(
            StatusText(tr("Downloading: %1 at %2 %3")
                .arg(snapshot.url)
                .arg(speed, 3, 'f', 1)
                .arg(unit)));
}

void FastStart::SnapshotFinished()
{
    assert(snapshot_download != nullptr);

    snapshot_output.close();

    if(snapshot_download->error()) {
        ui->statusLabel->setText(ErrorText(tr("There was an error downloading the snapshot")));
        QTimer::singleShot(ERROR_WAIT, this, SLOT(accept()));
    } else {
        ExtractSnapshot();
    }

    snapshot_download->deleteLater();
}

void FastStart::ExtractSnapshot()
{
    settings.setValue("snapshotstate", static_cast<int>(SnapshotInfo::EXTRACT));

    ui->progressBar->setMaximum(0);
    ui->statusLabel->setText(StatusText(tr("Extracting the Snapshot...")));
    QString dest = data_dir + "/";
    QString snapshot_path = dest + "snapshot.zip";
    if(!ExtractArchive(ui->statusLabel, snapshot_path.toStdString(), dest.toStdString())) {
        ui->statusLabel->setText(ErrorText(tr("There was an error extracting the snapshot")));
        settings.setValue("snapshotstate", static_cast<int>(SnapshotInfo::GETINFO));
        snapshot_output.remove();
    }
    settings.setValue("snapshotstate", static_cast<int>(SnapshotInfo::DONE));
    QTimer::singleShot(ERROR_WAIT, this, SLOT(accept()));
}

void FastStart::SnapshotReadyRead()
{
    assert(snapshot_download != nullptr);
    snapshot_output.write(snapshot_download->readAll());
}

void FastStart::nextSlide() 
{
    int next = (ui->overviewSlides->currentIndex() + 1) % ui->overviewSlides->count();
    ui->overviewSlides->setCurrentIndex(next);

    QGraphicsOpacityEffect *e = new QGraphicsOpacityEffect(this);
    ui->overviewSlides->setGraphicsEffect(e);
    QPropertyAnimation* a = new QPropertyAnimation(e, "opacity");
    a->setDuration(500);
    a->setStartValue(0);
    a->setEndValue(1);
    a->setEasingCurve(QEasingCurve::OutQuad);
    a->start(QAbstractAnimation::DeleteWhenStopped);

    QTimer::singleShot(1000 * SLIDE_TRANSITION_SECONDS, this, SLOT(endSlide()));
}

void FastStart::endSlide() 
{
    QGraphicsOpacityEffect *e = new QGraphicsOpacityEffect(this);
    ui->overviewSlides->setGraphicsEffect(e);
    QPropertyAnimation* a = new QPropertyAnimation(e, "opacity");
    a->setDuration(500);
    a->setStartValue(1);
    a->setEndValue(0);
    a->setEasingCurve(QEasingCurve::OutQuad);
    a->start(QAbstractAnimation::DeleteWhenStopped);
    QTimer::singleShot(600, this, SLOT(nextSlide()));
}

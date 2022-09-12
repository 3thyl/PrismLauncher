#include "JavaDownloader.h"
#include <QMessageBox>
#include <QPushButton>
#include <memory>
#include "Application.h"
#include "FileSystem.h"
#include "Json.h"
#include "MMCZip.h"
#include "SysInfo.h"
#include "net/ChecksumValidator.h"
#include "net/NetJob.h"
#include "ui/dialogs/ProgressDialog.h"

// Quick & dirty struct to store files
struct File {
    QString path;
    QString url;
    QByteArray hash;
    bool isExec;
};

void JavaDownloader::executeTask()
{
    auto OS = m_OS;
    auto isLegacy = m_isLegacy;

    downloadMojangJavaList(OS, isLegacy);
}
void JavaDownloader::downloadMojangJavaList(const QString& OS, bool isLegacy)
{
    auto netJob = new NetJob(QString("JRE::QueryVersions"), APPLICATION->network());
    auto response = new QByteArray();
    setStatus(tr("Querying mojang meta"));
    netJob->addNetAction(Net::Download::makeByteArray(
        QUrl("https://piston-meta.mojang.com/v1/products/java-runtime/2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json"), response));

    connect(this, &Task::aborted,
            [isLegacy] { QDir(FS::PathCombine("java", (isLegacy ? "java-legacy" : "java-current"))).removeRecursively(); });

    connect(netJob, &NetJob::finished, [netJob, response, this] {
        // delete so that it's not called on a deleted job
        disconnect(this, &Task::aborted, netJob, &NetJob::abort);
        netJob->deleteLater();
        delete response;
    });
    connect(netJob, &NetJob::progress, this, &JavaDownloader::progress);
    connect(netJob, &NetJob::failed, this, &JavaDownloader::emitFailed);

    connect(this, &Task::aborted, netJob, &NetJob::abort);

    connect(netJob, &NetJob::succeeded, [response, OS, isLegacy, this, netJob] {
        QJsonParseError parse_error{};
        QJsonDocument doc = QJsonDocument::fromJson(*response, &parse_error);
        if (parse_error.error != QJsonParseError::NoError) {
            qWarning() << "Error while parsing JSON response at " << parse_error.offset << " reason: " << parse_error.errorString();
            qWarning() << *response;
            return;
        }
        auto versionArray = Json::ensureArray(Json::ensureObject(doc.object(), OS), isLegacy ? "jre-legacy" : "java-runtime-gamma");
        if (!versionArray.empty()) {
            parseMojangManifest(isLegacy, versionArray);

        } else {
            // mojang does not have a JRE for us, let's get azul zulu
            downloadAzulMeta(OS, isLegacy, netJob);
        }
    });

    netJob->start();
}
void JavaDownloader::parseMojangManifest(bool isLegacy, const QJsonArray& versionArray)
{
    setStatus(tr("Downloading java from Mojang"));
    auto url = Json::ensureString(Json::ensureObject(Json::ensureObject(versionArray[0]), "manifest"), "url");
    auto download = new NetJob(QString("JRE::DownloadJava"), APPLICATION->network());
    auto files = new QByteArray();

    download->addNetAction(Net::Download::makeByteArray(QUrl(url), files));

    connect(download, &NetJob::finished, [download, files, this] {
        disconnect(this, &Task::aborted, download, &NetJob::abort);
        download->deleteLater();
        delete files;
    });
    connect(download, &NetJob::progress, this, &JavaDownloader::progress);
    connect(download, &NetJob::failed, this, &JavaDownloader::emitFailed);
    connect(this, &Task::aborted, download, &NetJob::abort);

    connect(download, &NetJob::succeeded, [files, isLegacy, this] {
        QJsonParseError parse_error{};
        QJsonDocument doc = QJsonDocument::fromJson(*files, &parse_error);
        if (parse_error.error != QJsonParseError::NoError) {
            qWarning() << "Error while parsing JSON response at " << parse_error.offset << " reason: " << parse_error.errorString();
            qWarning() << *files;
            return;
        }
        downloadMojangJava(isLegacy, doc);
    });
    download->start();
}
void JavaDownloader::downloadMojangJava(bool isLegacy, const QJsonDocument& doc)
{  // valid json doc, begin making jre spot
    auto output = FS::PathCombine(QString("java"), (isLegacy ? "java-legacy" : "java-current"));
    FS::ensureFolderPathExists(output);
    std::vector<File> toDownload;
    auto list = Json::ensureObject(Json::ensureObject(doc.object()), "files");
    for (const auto& paths : list.keys()) {
        auto file = FS::PathCombine(output, paths);

        auto type = Json::requireString(Json::requireObject(list, paths), "type");
        if (type == "directory") {
            FS::ensureFolderPathExists(file);
        } else if (type == "link") {
            // this is linux only !
            auto target = FS::PathCombine(file, "../" + Json::requireString(Json::requireObject(list, paths), "target"));
            QFile(target).link(file);
        } else if (type == "file") {
            // TODO download compressed version if it exists ?
            auto raw = Json::requireObject(Json::requireObject(Json::requireObject(list, paths), "downloads"), "raw");
            auto isExec = Json::ensureBoolean(Json::requireObject(list, paths), "executable", false);
            auto f = File{ file, Json::requireString(raw, "url"), QByteArray::fromHex(Json::ensureString(raw, "sha1").toLatin1()), isExec };
            toDownload.push_back(f);
        }
    }
    auto elementDownload = new NetJob("JRE::FileDownload", APPLICATION->network());
    for (const auto& file : toDownload) {
        auto dl = Net::Download::makeFile(file.url, file.path);
        dl->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, file.hash));
        if (file.isExec) {
            connect(dl.get(), &Net::Download::succeeded,
                    [file] { QFile(file.path).setPermissions(QFile(file.path).permissions() | QFileDevice::Permissions(0x1111)); });
        }
        elementDownload->addNetAction(dl);
    }
    connect(elementDownload, &NetJob::finished, [elementDownload, this] {
        disconnect(this, &Task::aborted, elementDownload, &NetJob::abort);
        elementDownload->deleteLater();
    });
    connect(elementDownload, &NetJob::progress, this, &JavaDownloader::progress);
    connect(elementDownload, &NetJob::failed, this, &JavaDownloader::emitFailed);

    connect(this, &Task::aborted, elementDownload, &NetJob::abort);
    connect(elementDownload, &NetJob::succeeded, [this] { emitSucceeded(); });
    elementDownload->start();
}
void JavaDownloader::downloadAzulMeta(const QString& OS, bool isLegacy, const NetJob* netJob)
{
    setStatus(tr("Querying Azul meta"));
    QString javaVersion = isLegacy ? QString("8.0") : QString("17.0");

    QString azulOS;
    QString arch;
    QString bitness;

    mojangOStoAzul(OS, azulOS, arch, bitness);
    auto metaResponse = new QByteArray();
    auto downloadJob = new NetJob(QString("JRE::QueryAzulMeta"), APPLICATION->network());
    downloadJob->addNetAction(
        Net::Download::makeByteArray(QString("https://api.azul.com/zulu/download/community/v1.0/bundles/?"
                                             "java_version=%1"
                                             "&os=%2"
                                             "&arch=%3"
                                             "&hw_bitness=%4"
                                             "&ext=zip"          // as a zip for all os, even linux NOTE !! Linux ARM is .deb only !!
                                             "&bundle_type=jre"  // jre only
                                             "&latest=true"      // only get the one latest entry
                                             )
                                         .arg(javaVersion, azulOS, arch, bitness),
                                     metaResponse));
    connect(downloadJob, &NetJob::finished, [downloadJob, metaResponse, this] {
        disconnect(this, &Task::aborted, downloadJob, &NetJob::abort);
        downloadJob->deleteLater();
        delete metaResponse;
    });
    connect(this, &Task::aborted, downloadJob, &NetJob::abort);
    connect(netJob, &NetJob::failed, this, &JavaDownloader::emitFailed);
    connect(downloadJob, &NetJob::progress, this, &JavaDownloader::progress);
    connect(downloadJob, &NetJob::succeeded, [metaResponse, isLegacy, this] {
        QJsonParseError parse_error{};
        QJsonDocument doc = QJsonDocument::fromJson(*metaResponse, &parse_error);
        if (parse_error.error != QJsonParseError::NoError) {
            qWarning() << "Error while parsing JSON response at " << parse_error.offset << " reason: " << parse_error.errorString();
            qWarning() << *metaResponse;
            return;
        }
        auto array = Json::ensureArray(doc.array());
        if (!array.empty()) {
            downloadAzulJava(isLegacy, array);
        } else {
            emitFailed(tr("No suitable JRE found"));
        }
    });
    downloadJob->start();
}
void JavaDownloader::mojangOStoAzul(const QString& OS, QString& azulOS, QString& arch, QString& bitness)
{
    if (OS == "mac-os-arm64") {
        // macos arm64
        azulOS = "macos";
        arch = "arm";
        bitness = "64";
    } else if (OS == "linux-arm64") {
        // linux arm64
        azulOS = "linux";
        arch = "arm";
        bitness = "64";
    } else if (OS == "linux-arm") {
        // linux arm (32)
        azulOS = "linux";
        arch = "arm";
        bitness = "32";
    } else if (OS == "linux") {
        // linux x86 64 (used for debugging, should never reach here)
        azulOS = "linux";
        arch = "x86";
        bitness = "64";
    }
}
void JavaDownloader::downloadAzulJava(bool isLegacy, const QJsonArray& array)
{  // JRE found ! download the zip
    setStatus(tr("Downloading java from Azul"));
    auto downloadURL = QUrl(array[0].toObject()["url"].toString());
    auto download = new NetJob(QString("JRE::DownloadJava"), APPLICATION->network());
    auto temp = std::make_unique<QTemporaryFile>(FS::PathCombine(APPLICATION->root(), "temp", "XXXXXX.zip"));
    FS::ensureFolderPathExists(FS::PathCombine(APPLICATION->root(), "temp"));
    // Have to open at least once to generate path
    temp->open();
    temp->close();
    download->addNetAction(Net::Download::makeFile(downloadURL, temp->fileName()));
    connect(download, &NetJob::finished, [download, this] {
        disconnect(this, &Task::aborted, download, &NetJob::abort);
        download->deleteLater();
    });
    connect(download, &NetJob::progress, this, &JavaDownloader::progress);
    connect(download, &NetJob::failed, this, &JavaDownloader::emitFailed);
    connect(this, &Task::aborted, download, &NetJob::abort);
    connect(download, &NetJob::succeeded, [isLegacy, file = std::move(temp), downloadURL, this] {
        setStatus(tr("Extracting java"));
        auto output = FS::PathCombine(QCoreApplication::applicationDirPath(), "java", isLegacy ? "java-legacy" : "java-current");
        // This should do all of the extracting and creating folders
        MMCZip::extractDir(file->fileName(), downloadURL.fileName().chopped(4), output);
        emitSucceeded();
    });
    download->start();
}
void JavaDownloader::showPrompts(QWidget* parent)
{
    QString sys = SysInfo::currentSystem();
    if (sys == "osx") {
        sys = "mac-os";
    }
    QString arch = SysInfo::useQTForArch();
    QString version;
    if (sys == "windows") {
        if (arch == "x86_64") {
            version = "windows-x64";
        } else if (arch == "i386") {
            version = "windows-x86";
        } else {
            // Unknown, maybe arm, appending arch for downloader
            version = "windows-" + arch;
        }
    } else if (sys == "mac-os") {
        if (arch == "arm64") {
            version = "mac-os-arm64";
        } else {
            version = "mac-os";
        }
    } else if (sys == "linux") {
        if (arch == "x86_64") {
            version = "linux";
        } else {
            // will work for i386, and arm(64)
            version = "linux-" + arch;
        }
    } else {
        // ? ? ? ? ? unknown os, at least it won't have a java version on mojang or azul, display warning
        QMessageBox::warning(parent, tr("Unknown OS"),
                             tr("The OS you are running is not supported by Mojang or Azul. Please install Java manually."));
        return;
    }
    // Selection using QMessageBox for java 8 or 17
    QMessageBox box(QMessageBox::Icon::Question, tr("Java version"),
                    tr("Do you want to download Java version 8 or 17?\n Java 8 is recommended for minecraft versions below 1.17\n Java 17 "
                       "is recommended for minecraft versions above or equal to 1.17"),
                    QMessageBox::NoButton, parent);
    auto yes = box.addButton("Java 17", QMessageBox::AcceptRole);
    auto no = box.addButton("Java 8", QMessageBox::AcceptRole);
    auto both = box.addButton(tr("Download both"), QMessageBox::AcceptRole);
    auto cancel = box.addButton(QMessageBox::Cancel);

    if (QFileInfo::exists(FS::PathCombine(QString("java"), "java-legacy"))) {
        no->setEnabled(false);
    }
    if (QFileInfo::exists(FS::PathCombine(QString("java"), "java-current"))) {
        yes->setEnabled(false);
    }
    if (!yes->isEnabled() || !no->isEnabled()) {
        both->setEnabled(false);
    }
    if (!yes->isEnabled() && !no->isEnabled()) {
        QMessageBox::warning(parent, tr("Already installed!"), tr("Both versions of java are already installed!"));
        return;
    }
    box.exec();
    if (box.clickedButton() == nullptr || box.clickedButton() == cancel) {
        return;
    }
    bool isLegacy = box.clickedButton() == no;

    auto down = new JavaDownloader(isLegacy, version);
    ProgressDialog dialog(parent);
    dialog.setSkipButton(true, tr("Abort"));

    if (dialog.execWithTask(down) && box.clickedButton() == both) {
        auto dwn = new JavaDownloader(false, version);
        ProgressDialog dg(parent);
        dg.setSkipButton(true, tr("Abort"));
        dg.execWithTask(dwn);
    }
}

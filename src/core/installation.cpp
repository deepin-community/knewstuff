/*
    This file is part of KNewStuff2.
    SPDX-FileCopyrightText: 2007 Josef Spillner <spillner@kde.org>
    SPDX-FileCopyrightText: 2009 Frederik Gladhorn <gladhorn@kde.org>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include "installation.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryFile>
#include <QUrlQuery>

#include "karchive.h"
#include "qmimedatabase.h"
#include <KRandom>
#include <KShell>
#include <KTar>
#include <KZip>

#include "jobs/kpackagejob.h"
#include <KPackage/Package>
#include <KPackage/PackageLoader>
#include <KPackage/PackageStructure>

#include <KLocalizedString>
#include <knewstuffcore_debug.h>
#include <qstandardpaths.h>

#include "jobs/filecopyjob.h"
#include "question.h"
#ifdef Q_OS_WIN
#include <shlobj.h>
#include <windows.h>
#endif

using namespace KNSCore;

Installation::Installation(QObject *parent)
    : QObject(parent)
{
    // TODO KF6 Make these real properties, when we can refactor this and add a proper dptr
    setProperty("kpackageType", QLatin1String(""));
    setProperty("uncompressSetting", UncompressionOptions::NeverUncompress);
}

bool Installation::readConfig(const KConfigGroup &group)
{
    // FIXME: add support for several categories later on
    // FIXME: read out only when actually installing as a performance improvement?
    uncompression = group.readEntry("Uncompress", QStringLiteral("never"));
    UncompressionOptions opt;
    // support old value of true as equivalent of always
    if (uncompression == QLatin1String("true")) {
        uncompression = QStringLiteral("always");
    }
    if (uncompression == QLatin1String("always")) {
        opt = AlwaysUncompress;
    } else if (uncompression == QLatin1String("archive")) {
        opt = UncompressIfArchive;
    } else if (uncompression == QLatin1String("subdir")) {
        opt = UncompressIntoSubdir;
    } else if (uncompression == QLatin1String("kpackage")) {
        opt = UseKPackageUncompression;
    } else if (uncompression == QLatin1String("subdir-archive")) {
        opt = UncompressIntoSubdirIfArchive;
    } else if (uncompression == QLatin1String("never")) {
        opt = NeverUncompress;
    } else {
        qCCritical(KNEWSTUFFCORE) << "invalid Uncompress setting chosen, must be one of: subdir, always, archive, never, or kpackage";
        return false;
    }
    setProperty("uncompressSetting", opt);
    setProperty("kpackageType", group.readEntry("KPackageType"));
    postInstallationCommand = group.readEntry("InstallationCommand");
    uninstallCommand = group.readEntry("UninstallCommand");
    standardResourceDirectory = group.readEntry("StandardResource");
    targetDirectory = group.readEntry("TargetDir");
    xdgTargetDirectory = group.readEntry("XdgTargetDir");

#if KNEWSTUFFCORE_BUILD_DEPRECATED_SINCE(5, 77)
    // Provide some compatibility
    if (standardResourceDirectory == QLatin1String("wallpaper")) {
        xdgTargetDirectory = QStringLiteral("wallpapers");
    }

    // also, ensure wallpapers are decompressed into subdirectories
    // this ensures that wallpapers with multiple resolutions continue to function
    // as expected
    if (xdgTargetDirectory == QLatin1String("wallpapers")) {
        uncompression = QStringLiteral("subdir");
    }

    // A touch of special treatment for the various old kpackage based knsrc files, so they work
    // with the new, internal stuff. The result unfortunately is that all entries marked as
    // installed in knewstuff no longer will be, but since it never worked right anyway... we'll
    // simply have to live with that.
    // clang-format off
    if (postInstallationCommand.startsWith(QLatin1String("kpackagetool5 -t")) &&
            postInstallationCommand.endsWith(QLatin1String("-i %f")) &&
            uninstallCommand.startsWith(QLatin1String("kpackagetool5 -t")) &&
            uninstallCommand.endsWith(QLatin1String("-r %f"))) {
        uncompression = QStringLiteral("kpackage");
        postInstallationCommand = QLatin1String("");
        // Not clearing uninstallCommand, as this is used for the fallback situation
        setProperty("kpackageType", uninstallCommand.mid(17, uninstallCommand.length() - 17 - 6));
        qCWarning(KNEWSTUFFCORE) << "Your configuration file uses an old version of the kpackage support, and should be converted. Please report this to the author of the software you are currently using. The package type, we assume, is" << property("kpackageType").toString();
    } else if (postInstallationCommand.startsWith(QLatin1String("kpackagetool5 --type")) &&
            postInstallationCommand.endsWith(QLatin1String("--install %f")) &&
            uninstallCommand.startsWith(QLatin1String("kpackagetool5 --type")) &&
            uninstallCommand.endsWith(QLatin1String("--remove %f"))) {
        uncompression = QStringLiteral("kpackage");
        postInstallationCommand = QLatin1String("");
        // Not clearing uninstallCommand, as this is used for the fallback situation
        setProperty("kpackageType", uninstallCommand.mid(21, uninstallCommand.length() - 21 - 12));
        qCWarning(KNEWSTUFFCORE) << "Your configuration file uses an old version of the kpackage support, and should be converted. Please report this to the author of the software you are currently using. The package type, we assume, is" << property("kpackageType").toString();
    }
    // clang-format on
#endif
#if KNEWSTUFFCORE_BUILD_DEPRECATED_SINCE(5, 79)
    customName = group.readEntry("CustomName", false);
    if (customName) {
        qWarning(KNEWSTUFFCORE) << "The CustomName property is deprecated and will be removed in KF6";
    }
    QString scopeString = group.readEntry("Scope");
    if (!scopeString.isEmpty()) {
        qWarning(KNEWSTUFFCORE) << "Setting the scope is deprecated, it will default to user";
        if (scopeString == QLatin1String("user")) {
            scope = ScopeUser;
        } else if (scopeString == QLatin1String("system")) {
            scope = ScopeSystem;
        } else {
            qCCritical(KNEWSTUFFCORE) << QStringLiteral("The scope '") + scopeString + QStringLiteral("' is unknown.");
            return false;
        }

        if (scope == ScopeSystem) {
            if (!installPath.isEmpty()) {
                qCCritical(KNEWSTUFFCORE) << "System installation cannot be mixed with InstallPath.";
                return false;
            }
        }
    }

    QString checksumpolicy = group.readEntry("ChecksumPolicy");
    if (!checksumpolicy.isEmpty()) {
        qWarning(KNEWSTUFFCORE) << "The ChecksumPolicy feature is defunct";
        if (checksumpolicy == QLatin1String("never")) {
            checksumPolicy = Installation::CheckNever;
        } else if (checksumpolicy == QLatin1String("ifpossible")) {
            checksumPolicy = Installation::CheckIfPossible;
        } else if (checksumpolicy == QLatin1String("always")) {
            checksumPolicy = Installation::CheckAlways;
        } else {
            qCCritical(KNEWSTUFFCORE) << QStringLiteral("The checksum policy '") + checksumpolicy + QStringLiteral("' is unknown.");
            return false;
        }
    }

    QString signaturepolicy = group.readEntry("SignaturePolicy");
    if (!signaturepolicy.isEmpty()) {
        qWarning(KNEWSTUFFCORE) << "The SignaturePolicy feature is defunct";
        if (signaturepolicy == QLatin1String("never")) {
            signaturePolicy = Installation::CheckNever;
        } else if (signaturepolicy == QLatin1String("ifpossible")) {
            signaturePolicy = Installation::CheckIfPossible;
        } else if (signaturepolicy == QLatin1String("always")) {
            signaturePolicy = Installation::CheckAlways;
        } else {
            qCCritical(KNEWSTUFFCORE) << QStringLiteral("The signature policy '") + signaturepolicy + QStringLiteral("' is unknown.");
            return false;
        }
    }
    acceptHtml = group.readEntry("AcceptHtmlDownloads", false);
    if (acceptHtml) {
        qWarning(KNEWSTUFFCORE) << "The AcceptHtmlDownload property is deprecated and will default to false. If there"
                                   "is a HTML download link the user will be prompted if the installation should proceed";
    }
#endif

    installPath = group.readEntry("InstallPath");
    absoluteInstallPath = group.readEntry("AbsoluteInstallPath");

    if (standardResourceDirectory.isEmpty() && targetDirectory.isEmpty() && xdgTargetDirectory.isEmpty() && installPath.isEmpty()
        && absoluteInstallPath.isEmpty()) {
        qCCritical(KNEWSTUFFCORE) << "No installation target set";
        return false;
    }
    return true;
}

#if KNEWSTUFFCORE_BUILD_DEPRECATED_SINCE(5, 71)
bool Installation::isRemote() const
{
    return false;
}
#endif

void Installation::install(const EntryInternal &entry)
{
    downloadPayload(entry);
}

void Installation::downloadPayload(const KNSCore::EntryInternal &entry)
{
    if (!entry.isValid()) {
        Q_EMIT signalInstallationFailed(i18n("Invalid item."));
        return;
    }
    QUrl source = QUrl(entry.payload());

    if (!source.isValid()) {
        qCCritical(KNEWSTUFFCORE) << "The entry doesn't have a payload.";
        Q_EMIT signalInstallationFailed(i18n("Download of item failed: no download URL for \"%1\".", entry.name()));
        return;
    }

    QString fileName(source.fileName());
    QTemporaryFile tempFile(QDir::tempPath() + QStringLiteral("/XXXXXX-") + fileName);
    tempFile.setAutoRemove(false);
    if (!tempFile.open()) {
        return; // ERROR
    }
    QUrl destination = QUrl::fromLocalFile(tempFile.fileName());
    qCDebug(KNEWSTUFFCORE) << "Downloading payload" << source << "to" << destination;
#ifdef Q_OS_WIN // can't write to the file if it's open, on Windows
    tempFile.close();
#endif

    // FIXME: check for validity
    FileCopyJob *job = FileCopyJob::file_copy(source, destination, -1, JobFlag::Overwrite | JobFlag::HideProgressInfo);
    connect(job, &KJob::result, this, &Installation::slotPayloadResult);

    entry_jobs[job] = entry;
}

void Installation::slotPayloadResult(KJob *job)
{
    // for some reason this slot is getting called 3 times on one job error
    if (entry_jobs.contains(job)) {
        EntryInternal entry = entry_jobs[job];
        entry_jobs.remove(job);

        if (job->error()) {
            const QString errorMessage = i18n("Download of \"%1\" failed, error: %2", entry.name(), job->errorString());
            qCWarning(KNEWSTUFFCORE) << errorMessage;
            Q_EMIT signalInstallationFailed(errorMessage);
        } else {
            FileCopyJob *fcjob = static_cast<FileCopyJob *>(job);
            qCDebug(KNEWSTUFFCORE) << "Copied to" << fcjob->destUrl();
#if KNEWSTUFFCORE_BUILD_DEPRECATED_SINCE(5, 79)
            // check if the app likes html files - disabled by default as too many bad links have been submitted to opendesktop.org
            if (!acceptHtml) {
#endif
                QMimeDatabase db;
                QMimeType mimeType = db.mimeTypeForFile(fcjob->destUrl().toLocalFile());
                if (mimeType.inherits(QStringLiteral("text/html")) || mimeType.inherits(QStringLiteral("application/x-php"))) {
                    const auto error = i18n("Cannot install '%1' because it points to a web page. Click <a href='%2'>here</a> to finish the installation.",
                                            entry.name(),
                                            fcjob->srcUrl().toString());
                    Q_EMIT signalInstallationFailed(error);
                    entry.setStatus(KNS3::Entry::Invalid);
                    Q_EMIT signalEntryChanged(entry);
                    return;
                }
#if KNEWSTUFFCORE_BUILD_DEPRECATED_SINCE(5, 79)
            }
#endif

            Q_EMIT signalPayloadLoaded(fcjob->destUrl());
            install(entry, fcjob->destUrl().toLocalFile());
        }
    }
}

void KNSCore::Installation::install(KNSCore::EntryInternal entry, const QString &downloadedFile)
{
    qCDebug(KNEWSTUFFCORE) << "Install:" << entry.name() << "from" << downloadedFile;
    Q_ASSERT(QFileInfo::exists(downloadedFile));

    if (entry.payload().isEmpty()) {
        qCDebug(KNEWSTUFFCORE) << "No payload associated with:" << entry.name();
        return;
    }

    // TODO Add async checksum verification

    QString targetPath = targetInstallationPath();
    QStringList installedFiles = installDownloadedFileAndUncompress(entry, downloadedFile, targetPath);

    if (uncompressionSetting() != UseKPackageUncompression) {
        if (installedFiles.isEmpty()) {
            if (entry.status() == KNS3::Entry::Installing) {
                entry.setStatus(KNS3::Entry::Downloadable);
            } else if (entry.status() == KNS3::Entry::Updating) {
                entry.setStatus(KNS3::Entry::Updateable);
            }
            Q_EMIT signalEntryChanged(entry);
            Q_EMIT signalInstallationFailed(i18n("Could not install \"%1\": file not found.", entry.name()));
            return;
        }

        entry.setInstalledFiles(installedFiles);

        auto installationFinished = [this, entry]() {
            EntryInternal newentry = entry;
            if (!newentry.updateVersion().isEmpty()) {
                newentry.setVersion(newentry.updateVersion());
            }
            if (newentry.updateReleaseDate().isValid()) {
                newentry.setReleaseDate(newentry.updateReleaseDate());
            }
            newentry.setStatus(KNS3::Entry::Installed);
            Q_EMIT signalEntryChanged(newentry);
            Q_EMIT signalInstallationFinished();
        };
        if (!postInstallationCommand.isEmpty()) {
            QString scriptArgPath = !installedFiles.isEmpty() ? installedFiles.first() : targetPath;
            if (scriptArgPath.endsWith(QLatin1Char('*'))) {
                scriptArgPath = scriptArgPath.left(scriptArgPath.lastIndexOf(QLatin1Char('*')));
            }
            QProcess *p = runPostInstallationCommand(scriptArgPath);
            connect(p, &QProcess::finished, this, [entry, installationFinished, this](int exitCode) {
                if (exitCode) {
                    EntryInternal newEntry = entry;
                    newEntry.setStatus(KNS3::Entry::Invalid);
                    Q_EMIT signalEntryChanged(newEntry);
                } else {
                    installationFinished();
                }
            });
        } else {
            installationFinished();
        }
    }
}

QString Installation::targetInstallationPath() const
{
    // installdir is the target directory
    QString installdir;

#if KNEWSTUFFCORE_BUILD_DEPRECATED_SINCE(5, 79)
    const bool userScope = scope == ScopeUser;
#else
    const bool userScope = true;
#endif
    // installpath also contains the file name if it's a single file, otherwise equal to installdir
    int pathcounter = 0;
    // wallpaper is already managed in the case of !xdgTargetDirectory.isEmpty()
    if (!standardResourceDirectory.isEmpty() && standardResourceDirectory != QLatin1String("wallpaper")) {
        QStandardPaths::StandardLocation location = QStandardPaths::TempLocation;
        // crude translation KStandardDirs names -> QStandardPaths enum
        if (standardResourceDirectory == QLatin1String("tmp")) {
            location = QStandardPaths::TempLocation;
        } else if (standardResourceDirectory == QLatin1String("config")) {
            location = QStandardPaths::ConfigLocation;
        }

        if (userScope) {
            installdir = QStandardPaths::writableLocation(location);
        } else { // system scope
            installdir = QStandardPaths::standardLocations(location).constLast();
        }
        pathcounter++;
    }
    if (!targetDirectory.isEmpty() && targetDirectory != QLatin1String("/")) {
        if (userScope) {
            installdir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1Char('/') + targetDirectory + QLatin1Char('/');
        } else { // system scope
            installdir = QStandardPaths::locate(QStandardPaths::GenericDataLocation, targetDirectory, QStandardPaths::LocateDirectory) + QLatin1Char('/');
        }
        pathcounter++;
    }
    if (!xdgTargetDirectory.isEmpty() && xdgTargetDirectory != QLatin1String("/")) {
        installdir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1Char('/') + xdgTargetDirectory + QLatin1Char('/');
        pathcounter++;
    }
    if (!installPath.isEmpty()) {
#if defined(Q_OS_WIN)
        WCHAR wPath[MAX_PATH + 1];
        if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, wPath) == S_OK) {
            installdir = QString::fromUtf16((const ushort *)wPath) + QLatin1Char('/') + installPath + QLatin1Char('/');
        } else {
            installdir = QDir::homePath() + QLatin1Char('/') + installPath + QLatin1Char('/');
        }
#else
        installdir = QDir::homePath() + QLatin1Char('/') + installPath + QLatin1Char('/');
#endif
        pathcounter++;
    }
    if (!absoluteInstallPath.isEmpty()) {
        installdir = absoluteInstallPath + QLatin1Char('/');
        pathcounter++;
    }

    if (pathcounter != 1) {
        qCCritical(KNEWSTUFFCORE) << "Wrong number of installation directories given.";
        return QString();
    }

    qCDebug(KNEWSTUFFCORE) << "installdir: " << installdir;

    // create the dir if it doesn't exist (QStandardPaths doesn't create it, unlike KStandardDirs!)
    QDir().mkpath(installdir);

    return installdir;
}

QStringList Installation::installDownloadedFileAndUncompress(const KNSCore::EntryInternal &entry, const QString &payloadfile, const QString installdir)
{
    // Collect all files that were installed
    QStringList installedFiles;
    bool isarchive = true;
    UncompressionOptions uncompressionOpt = uncompressionSetting();

    // respect the uncompress flag in the knsrc
    if (uncompressionOpt == UseKPackageUncompression) {
        qCDebug(KNEWSTUFFCORE) << "Using KPackage for installation";
        KPackage::PackageStructure structure;
        KPackage::Package package(&structure);
        package.setPath(payloadfile);
        auto resetEntryStatus = [this, entry]() {
            KNSCore::EntryInternal changedEntry(entry);
            if (changedEntry.status() == KNS3::Entry::Installing || changedEntry.status() == KNS3::Entry::Installed) {
                changedEntry.setStatus(KNS3::Entry::Downloadable);
            } else if (changedEntry.status() == KNS3::Entry::Updating) {
                changedEntry.setStatus(KNS3::Entry::Updateable);
            }
            Q_EMIT signalEntryChanged(changedEntry);
        };
        if (package.isValid() && package.metadata().isValid()) {
            qCDebug(KNEWSTUFFCORE) << "Package metadata is valid";
#if KNEWSTUFFCORE_BUILD_DEPRECATED_SINCE(5, 90)
            QString serviceType;
            serviceType = package.metadata().value(QStringLiteral("X-Plasma-ServiceType"));
            const auto serviceTypes =
                package.metadata().rawData().value(QLatin1String("KPlugin")).toObject().value(QLatin1String("ServiceTypes")).toVariant().toStringList();
            if (serviceType.isEmpty() && !serviceTypes.isEmpty()) {
                serviceType = serviceTypes.first();
            }
            if (serviceType.isEmpty()) {
                serviceType = property("kpackageType").toString();
            } else if (serviceType != property("kpackageType").toString()) {
                qCWarning(KNEWSTUFFCORE) << "The package" << package.metadata().fileName()
                                         << "defines a different kpackage type than the one defined by the app."
                                         << "Please report this to the author of the addon.";
            }
#else
            const QString serviceType = property("kpackageType").toString();
#endif

            if (!serviceType.isEmpty()) {
                qCDebug(KNEWSTUFFCORE) << "Service type discovered as" << serviceType;
                KPackage::PackageStructure *structure = KPackage::PackageLoader::self()->loadPackageStructure(serviceType);
                if (structure) {
                    KPackage::Package installer = KPackage::Package(structure);
                    if (installer.hasValidStructure()) {
                        QString packageRoot =
                            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1Char('/') + installer.defaultPackageRoot();
                        qCDebug(KNEWSTUFFCORE) << "About to attempt to install" << package.metadata().pluginId() << "into" << packageRoot;
                        const QString expectedDir{packageRoot + package.metadata().pluginId()};
                        KJob *installJob = KPackageJob::update(payloadfile, packageRoot, serviceType);
                        // TODO KF6 Really, i would prefer to make more functions to handle this, but as this is
                        // an exported class, i'd rather not pollute the public namespace with internal functions,
                        // and we don't have a pimpl, so... we'll just have to deal with it for now
                        connect(installJob, &KJob::result, this, [this, entry, payloadfile, expectedDir, resetEntryStatus](KJob *job) {
                            if (job->error() == KJob::NoError) {
                                if (QFile::exists(expectedDir)) {
                                    EntryInternal newentry = entry;
                                    newentry.setInstalledFiles(QStringList{expectedDir});
                                    // update version and release date to the new ones
                                    if (newentry.status() == KNS3::Entry::Updating) {
                                        if (!newentry.updateVersion().isEmpty()) {
                                            newentry.setVersion(newentry.updateVersion());
                                        }
                                        if (newentry.updateReleaseDate().isValid()) {
                                            newentry.setReleaseDate(newentry.updateReleaseDate());
                                        }
                                    }
                                    newentry.setStatus(KNS3::Entry::Installed);
                                    // We can remove the downloaded file, because we don't save its location and don't need it to uninstall the entry
                                    QFile::remove(payloadfile);
                                    Q_EMIT signalEntryChanged(newentry);
                                    Q_EMIT signalInstallationFinished();
                                    qCDebug(KNEWSTUFFCORE) << "Install job finished with no error and we now have files" << expectedDir;
                                } else {
                                    Q_EMIT signalInstallationFailed(
                                        i18n("The installation of %1 failed to create the expected new directory %2").arg(payloadfile, expectedDir));
                                    resetEntryStatus();
                                    qCDebug(KNEWSTUFFCORE)
                                        << "Install job finished with no error, but we do not have the expected new directory" << expectedDir;
                                }
                            } else {
                                if (job->error() == KPackage::Package::JobError::NewerVersionAlreadyInstalledError) {
                                    EntryInternal newentry = entry;
                                    newentry.setStatus(KNS3::Entry::Installed);
                                    newentry.setInstalledFiles(QStringList{expectedDir});
                                    // update version and release date to the new ones
                                    if (!newentry.updateVersion().isEmpty()) {
                                        newentry.setVersion(newentry.updateVersion());
                                    }
                                    if (newentry.updateReleaseDate().isValid()) {
                                        newentry.setReleaseDate(newentry.updateReleaseDate());
                                    }
                                    Q_EMIT signalEntryChanged(newentry);
                                    Q_EMIT signalInstallationFinished();
                                    qCDebug(KNEWSTUFFCORE) << "Install job finished telling us this item was already installed with this version, so... let's "
                                                              "just make a small fib and say we totally installed that, honest, and we now have files"
                                                           << expectedDir;
                                } else {
                                    Q_EMIT signalInstallationFailed(i18n("Installation of %1 failed: %2", payloadfile, job->errorText()));
                                    resetEntryStatus();
                                    qCDebug(KNEWSTUFFCORE) << "Install job finished with error state" << job->error() << "and description" << job->error();
                                }
                            }
                        });
                        installJob->start();
                    } else {
                        Q_EMIT signalInstallationFailed(
                            i18n("The installation of %1 failed, as the service type %2 was not accepted by the system (did you forget to install the KPackage "
                                 "support plugin for this type of package?)",
                                 payloadfile,
                                 serviceType));
                        resetEntryStatus();
                        qCWarning(KNEWSTUFFCORE) << "Package serviceType" << serviceType << "not found";
                    }
                } else {
                    // no package structure
                    Q_EMIT signalInstallationFailed(
                        i18n("The installation of %1 failed, as the downloaded package does not contain a correct KPackage structure.", payloadfile));
                    resetEntryStatus();
                    qCWarning(KNEWSTUFFCORE) << "Could not load the package structure for KPackage service type" << serviceType;
                }
            } else {
                // no service type
                Q_EMIT signalInstallationFailed(i18n("The installation of %1 failed, as the downloaded package does not list a service type.", payloadfile));
                resetEntryStatus();
                qCWarning(KNEWSTUFFCORE) << "No service type listed in" << payloadfile;
            }
        } else {
            // package or package metadata is invalid
            Q_EMIT signalInstallationFailed(
                i18n("The installation of %1 failed, as the downloaded package does not contain any useful meta information, which means it is not a valid "
                     "KPackage.",
                     payloadfile));
            resetEntryStatus();
            qCWarning(KNEWSTUFFCORE) << "No valid meta information (which suggests no valid KPackage) found in" << payloadfile;
        }
    } else {
        if (uncompressionOpt == AlwaysUncompress || uncompressionOpt == UncompressIntoSubdirIfArchive || uncompressionOpt == UncompressIfArchive
            || uncompressionOpt == UncompressIntoSubdir) {
            // this is weird but a decompression is not a single name, so take the path instead
            QMimeDatabase db;
            QMimeType mimeType = db.mimeTypeForFile(payloadfile);
            qCDebug(KNEWSTUFFCORE) << "Postinstallation: uncompress the file";

            // FIXME: check for overwriting, malicious archive entries (../foo) etc.
            // FIXME: KArchive should provide "safe mode" for this!
            QScopedPointer<KArchive> archive;

            if (mimeType.inherits(QStringLiteral("application/zip"))) {
                archive.reset(new KZip(payloadfile));
                // clang-format off
            } else if (mimeType.inherits(QStringLiteral("application/tar"))
                    || mimeType.inherits(QStringLiteral("application/x-tar")) // BUG 450662
                    || mimeType.inherits(QStringLiteral("application/x-gzip"))
                    || mimeType.inherits(QStringLiteral("application/x-bzip"))
                    || mimeType.inherits(QStringLiteral("application/x-lzma"))
                    || mimeType.inherits(QStringLiteral("application/x-xz"))
                    || mimeType.inherits(QStringLiteral("application/x-bzip-compressed-tar"))
                    || mimeType.inherits(QStringLiteral("application/x-compressed-tar"))) {
                // clang-format on
                archive.reset(new KTar(payloadfile));
            } else {
                qCCritical(KNEWSTUFFCORE) << "Could not determine type of archive file '" << payloadfile << "'";
                if (uncompressionOpt == AlwaysUncompress) {
                    Q_EMIT signalInstallationError(i18n("Could not determine the type of archive of the downloaded file %1", payloadfile));
                    return QStringList();
                }
                isarchive = false;
            }

            if (isarchive) {
                bool success = archive->open(QIODevice::ReadOnly);
                if (!success) {
                    qCCritical(KNEWSTUFFCORE) << "Cannot open archive file '" << payloadfile << "'";
                    if (uncompressionOpt == AlwaysUncompress) {
                        Q_EMIT signalInstallationError(
                            i18n("Failed to open the archive file %1. The reported error was: %2", payloadfile, archive->errorString()));
                        return QStringList();
                    }
                    // otherwise, just copy the file
                    isarchive = false;
                }

                if (isarchive) {
                    const KArchiveDirectory *dir = archive->directory();
                    // if there is more than an item in the file, and we are requested to do so
                    // put contents in a subdirectory with the same name as the file
                    QString installpath;
                    const bool isSubdir =
                        (uncompressionOpt == UncompressIntoSubdir || uncompressionOpt == UncompressIntoSubdirIfArchive) && dir->entries().count() > 1;
                    if (isSubdir) {
                        installpath = installdir + QLatin1Char('/') + QFileInfo(archive->fileName()).baseName();
                    } else {
                        installpath = installdir;
                    }

                    if (dir->copyTo(installpath)) {
                        // If we extracted the subdir we want to save it using the /* notation like we would when using the "archive" option
                        // Also if we use an (un)install command we only call it once with the folder as argument and not for each file
                        if (isSubdir) {
                            installedFiles << QDir(installpath).absolutePath() + QLatin1String("/*");
                        } else {
                            installedFiles << archiveEntries(installpath, dir);
                        }
                    } else {
                        qCWarning(KNEWSTUFFCORE) << "could not install" << entry.name() << "to" << installpath;
                    }

                    archive->close();
                    QFile::remove(payloadfile);
                }
            }
        }

        qCDebug(KNEWSTUFFCORE) << "isarchive:" << isarchive;

        // some wallpapers are compressed, some aren't
        if ((!isarchive && standardResourceDirectory == QLatin1String("wallpaper"))
            || (uncompressionOpt == NeverUncompress || (uncompressionOpt == UncompressIfArchive && !isarchive)
                || (uncompressionOpt == UncompressIntoSubdirIfArchive && !isarchive))) {
            // no decompress but move to target

            /// @todo when using KIO::get the http header can be accessed and it contains a real file name.
            // FIXME: make naming convention configurable through *.knsrc? e.g. for kde-look.org image names
            QUrl source = QUrl(entry.payload());
            qCDebug(KNEWSTUFFCORE) << "installing non-archive from" << source;
#if KNEWSTUFF_BUILD_DEPRECATED_SINCE(5, 79)
            QString installfile;
            QString ext = source.fileName().section(QLatin1Char('.'), -1);
            if (customName) {
                // Otherwise name can be interpreted as path
                installfile = entry.name().remove(QLatin1Char('/'));
                if (!entry.version().isEmpty()) {
                    installfile += QLatin1Char('-') + entry.version();
                }
                if (!ext.isEmpty()) {
                    installfile += QLatin1Char('.') + ext;
                }
            } else {
                installfile = source.fileName();
            }
            const QString installpath = QDir(installdir).filePath(installfile);
#else
            const QString installpath = QDir(installdir).filePath(source.fileName());
#endif

            qCDebug(KNEWSTUFFCORE) << "Install to file" << installpath;
            // FIXME: copy goes here (including overwrite checking)
            // FIXME: what must be done now is to update the cache *again*
            //        in order to set the new payload filename (on root tag only)
            //        - this might or might not need to take uncompression into account
            // FIXME: for updates, we might need to force an overwrite (that is, deleting before)
            QFile file(payloadfile);
            bool success = true;
            const bool update = ((entry.status() == KNS3::Entry::Updateable) || (entry.status() == KNS3::Entry::Updating));

            if (QFile::exists(installpath) && QDir::tempPath() != installdir) {
                if (!update) {
                    Question question(Question::YesNoQuestion);
                    question.setEntry(entry);
                    question.setQuestion(i18n("This file already exists on disk (possibly due to an earlier failed download attempt). Continuing means "
                                              "overwriting it. Do you wish to overwrite the existing file?")
                                         + QStringLiteral("\n'") + installpath + QLatin1Char('\''));
                    question.setTitle(i18n("Overwrite File"));
                    if (question.ask() != Question::YesResponse) {
                        return QStringList();
                    }
                }
                success = QFile::remove(installpath);
            }
            if (success) {
                // remove in case it's already present and in a temporary directory, so we get to actually use the path again
                if (installpath.startsWith(QDir::tempPath())) {
                    QFile::remove(installpath);
                }
                success = file.rename(installpath);
                qCDebug(KNEWSTUFFCORE) << "move:" << file.fileName() << "to" << installpath;
                if (!success) {
                    qCWarning(KNEWSTUFFCORE) << file.errorString();
                }
            }
            if (!success) {
                Q_EMIT signalInstallationError(i18n("Unable to move the file %1 to the intended destination %2", payloadfile, installpath));
                qCCritical(KNEWSTUFFCORE) << "Cannot move file '" << payloadfile << "' to destination '" << installpath << "'";
                return QStringList();
            }
            installedFiles << installpath;
        }
    }

    return installedFiles;
}

QProcess *Installation::runPostInstallationCommand(const QString &installPath)
{
    QString command(postInstallationCommand);
    QString fileArg(KShell::quoteArg(installPath));
    command.replace(QLatin1String("%f"), fileArg);

    qCDebug(KNEWSTUFFCORE) << "Run command:" << command;

    QProcess *ret = new QProcess(this);
    auto onProcessFinished = [this, command, ret](int exitcode, QProcess::ExitStatus status) {
        const QString output{QString::fromLocal8Bit(ret->readAllStandardError())};
        if (status == QProcess::CrashExit) {
            QString errorMessage = i18n("The installation failed while attempting to run the command:\n%1\n\nThe returned output was:\n%2", command, output);
            Q_EMIT signalInstallationError(errorMessage);
            qCCritical(KNEWSTUFFCORE) << "Process crashed with command: " << command;
        } else if (exitcode) {
            // 130 means Ctrl+C as an exit code this is interpreted by KNewStuff as cancel operation
            // and no error will be displayed to the user, BUG: 436355
            if (exitcode == 130) {
                qCCritical(KNEWSTUFFCORE) << "Command '" << command << "' failed was aborted by the user";
            } else {
                Q_EMIT signalInstallationError(
                    i18n("The installation failed with code %1 while attempting to run the command:\n%2\n\nThe returned output was:\n%3",
                         exitcode,
                         command,
                         output));
                qCCritical(KNEWSTUFFCORE) << "Command '" << command << "' failed with code" << exitcode;
            }
        }
        sender()->deleteLater();
    };
    connect(ret, &QProcess::finished, this, onProcessFinished);

    QStringList args = KShell::splitArgs(command);
    ret->setProgram(args.takeFirst());
    ret->setArguments(args);
    ret->start();
    return ret;
}

void Installation::uninstall(EntryInternal entry)
{
    // TODO Put this in pimpl or job
    const auto deleteFilesAndMarkAsUninstalled = [entry, this]() {
        KNS3::Entry::Status newStatus{KNS3::Entry::Deleted};
        const auto lst = entry.installedFiles();
        for (const QString &file : lst) {
            // This is used to delete the download location if there are no more entries
            QFileInfo info(file);
            if (info.isDir()) {
                QDir().rmdir(file);
            } else if (file.endsWith(QLatin1String("/*"))) {
                QDir dir(file.left(file.size() - 2));
                bool worked = dir.removeRecursively();
                if (!worked) {
                    qCWarning(KNEWSTUFFCORE) << "Couldn't remove" << dir.path();
                    continue;
                }
            } else {
                if (info.exists() || info.isSymLink()) {
                    bool worked = QFile::remove(file);
                    if (!worked) {
                        qWarning() << "unable to delete file " << file;
                        Q_EMIT signalInstallationFailed(
                            i18n("The removal of %1 failed, as the installed file %2 could not be automatically removed. You can attempt to manually delete "
                                 "this file, if you believe this is an error.",
                                 entry.name(),
                                 file));
                        // Assume that the uninstallation has failed, and reset the entry to an installed state
                        newStatus = KNS3::Entry::Installed;
                        break;
                    }
                } else {
                    qWarning() << "unable to delete file " << file << ". file does not exist.";
                }
            }
        }
        EntryInternal newEntry = entry;
        if (newStatus == KNS3::Entry::Deleted) {
            newEntry.setUnInstalledFiles(entry.installedFiles());
            newEntry.setInstalledFiles(QStringList());
        }
        newEntry.setStatus(newStatus);
        Q_EMIT signalEntryChanged(newEntry);
    };

    if (uncompressionSetting() == UseKPackageUncompression) {
        const auto lst = entry.installedFiles();
        if (lst.length() == 1) {
            const QString installedFile{lst.first()};
            if (QFileInfo(installedFile).isDir()) {
                KPackage::PackageStructure structure;
                KPackage::Package package(&structure);
                package.setPath(installedFile);
                if (package.isValid() && package.metadata().isValid()) {
#if KNEWSTUFFCORE_BUILD_DEPRECATED_SINCE(5, 90)
                    QString serviceType = package.metadata().value(QStringLiteral("X-Plasma-ServiceType"));
                    const auto serviceTypes =
                        package.metadata().rawData().value(QLatin1String("KPlugin")).toObject().value(QLatin1String("ServiceTypes")).toVariant().toStringList();
                    if (serviceType.isEmpty() && !serviceTypes.isEmpty()) {
                        serviceType = serviceTypes.first();
                    }
                    if (serviceType.isEmpty()) {
                        serviceType = property("kpackageType").toString();
                    } else if (serviceType != property("kpackageType").toString()) {
                        qCWarning(KNEWSTUFFCORE) << "The package" << package.metadata().fileName()
                                                 << "defines a different kpackage type than the one defined by the app."
                                                 << "Please report this to the author of the addon.";
                    }
#else
                    const QString serviceType = property("kpackageType").toString();
#endif
                    if (!serviceType.isEmpty()) {
                        KPackage::PackageStructure *structure = KPackage::PackageLoader::self()->loadPackageStructure(serviceType);
                        if (structure) {
                            KPackage::Package installer = KPackage::Package(structure);
                            if (!installer.hasValidStructure()) {
                                qWarning() << "Package serviceType" << serviceType << "not found";
                            }
                            QString packageRoot =
                                QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1Char('/') + installer.defaultPackageRoot();
                            KJob *removalJob = KPackageJob::uninstall(package.metadata().pluginId(), packageRoot, serviceType);
                            connect(removalJob, &KJob::result, this, [this, installedFile, installer, entry](KJob *job) {
                                EntryInternal newEntry = entry;
                                if (job->error() == KJob::NoError) {
                                    newEntry.setStatus(KNS3::Entry::Deleted);
                                    newEntry.setUnInstalledFiles(newEntry.installedFiles());
                                    newEntry.setInstalledFiles(QStringList());
                                    Q_EMIT signalEntryChanged(newEntry);
                                } else {
                                    Q_EMIT signalInstallationFailed(i18n("Installation of %1 failed: %2", installedFile, job->errorText()));
                                }
                            });
                            removalJob->start();
                        } else {
                            // no package structure
                            Q_EMIT signalInstallationFailed(
                                i18n("The removal of %1 failed, as the installed package does not contain a correct KPackage structure.", installedFile));
                        }
                    } else {
                        // no service type
                        Q_EMIT signalInstallationFailed(
                            i18n("The removal of %1 failed, as the installed package is not a supported type (did you forget to install the KPackage support "
                                 "plugin for this type of package?)",
                                 installedFile));
                    }
                } else {
                    // package or package metadata is invalid
                    Q_EMIT signalInstallationFailed(
                        i18n("The removal of %1 failed, as the installed package does not contain any useful meta information, which means it is not a valid "
                             "KPackage.",
                             entry.name()));
                }
            } else {
                QMimeDatabase db;
                QMimeType mimeType = db.mimeTypeForFile(installedFile);
                if (mimeType.inherits(QStringLiteral("application/zip")) || mimeType.inherits(QStringLiteral("application/x-compressed-tar"))
                    || mimeType.inherits(QStringLiteral("application/x-gzip")) || mimeType.inherits(QStringLiteral("application/x-tar"))
                    || mimeType.inherits(QStringLiteral("application/x-bzip-compressed-tar")) || mimeType.inherits(QStringLiteral("application/x-xz"))
                    || mimeType.inherits(QStringLiteral("application/x-lzma"))) {
                    // Then it's one of the downloaded files installed with an old version of knewstuff prior to
                    // the native kpackage support being added, and we need to do some inspection-and-removal work...
                    KPackage::PackageStructure structure;
                    KPackage::Package package(&structure);
                    const QString serviceType{property("kpackageType").toString()};
                    package.setPath(installedFile);
                    if (package.isValid() && package.metadata().isValid()) {
                        // try and load the kpackage and sniff the expected location of its installation, and ask KPackage to remove that thing, if it's there
                        KPackage::PackageStructure *structure = KPackage::PackageLoader::self()->loadPackageStructure(serviceType);
                        if (structure) {
                            KPackage::Package installer = KPackage::Package(structure);
                            if (installer.hasValidStructure()) {
                                QString packageRoot =
                                    QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1Char('/') + installer.defaultPackageRoot();
                                qCDebug(KNEWSTUFFCORE) << "About to attempt to uninstall" << package.metadata().pluginId() << "from" << packageRoot;
                                // Frankly, we don't care whether or not this next step succeeds, and it can just fizzle if it wants
                                // to. This is a cleanup step, and if it fails, it's just not really important.
                                KPackageJob::uninstall(package.metadata().pluginId(), packageRoot, serviceType);
                            }
                        }
                    }
                    // Also get rid of the downloaded file, and tell everything they've gone
                    if (QFile::remove(installedFile)) {
                        entry.setStatus(KNS3::Entry::Deleted);
                        entry.setUnInstalledFiles(entry.installedFiles());
                        entry.setInstalledFiles(QStringList());
                        Q_EMIT signalEntryChanged(entry);
                    } else {
                        Q_EMIT signalInstallationFailed(
                            i18n("The removal of %1 failed, as the downloaded file %2 could not be automatically removed.", entry.name(), installedFile));
                    }
                } else {
                    // Not sure what's installed, but it's not a KPackage, not a lot we can do with this...
                    Q_EMIT signalInstallationFailed(
                        i18n("The removal of %1 failed, due to the installed file not being a KPackage. The file in question was %2, and you can attempt to "
                             "delete it yourself, if you are certain that it is not needed.",
                             entry.name(),
                             installedFile));
                }
            }
        } else {
            Q_EMIT signalInstallationFailed(
                i18n("The removal of %1 failed, as there seems to somehow be more than one thing installed, which is not supposed to be possible for KPackage "
                     "based entries.",
                     entry.name()));
        }
        deleteFilesAndMarkAsUninstalled();
    } else {
        const auto lst = entry.installedFiles();
        // If there is an uninstall script, make sure it runs without errors
        if (!uninstallCommand.isEmpty()) {
            bool validFileExisted = false;
            for (const QString &file : lst) {
                QString filePath = file;
                bool validFile = QFileInfo::exists(filePath);
                // If we have uncompressed a subdir we write <path>/* in the config, but when calling a script
                // we want to convert this to a normal path
                if (!validFile && file.endsWith(QLatin1Char('*'))) {
                    filePath = filePath.left(filePath.lastIndexOf(QLatin1Char('*')));
                    validFile = QFileInfo::exists(filePath);
                }
                if (validFile) {
                    validFileExisted = true;
                    QString fileArg(KShell::quoteArg(filePath));
                    QString command(uninstallCommand);
                    command.replace(QLatin1String("%f"), fileArg);

                    QStringList args = KShell::splitArgs(command);
                    const QString program = args.takeFirst();
                    QProcess *process = new QProcess(this);
                    process->start(program, args);
                    auto onProcessFinished = [this, command, process, entry, deleteFilesAndMarkAsUninstalled](int, QProcess::ExitStatus status) {
                        if (status == QProcess::CrashExit) {
                            const QString processOutput = QString::fromLocal8Bit(process->readAllStandardError());
                            const QString err = i18n(
                                "The uninstallation process failed to successfully run the command %1\n"
                                "The output of was: \n%2\n"
                                "If you think this is incorrect, you can continue or cancel the uninstallation process",
                                KShell::quoteArg(command),
                                processOutput);
                            Q_EMIT signalInstallationError(err);
                            // Ask the user if he wants to continue, even though the script failed
                            Question question(Question::ContinueCancelQuestion);
                            question.setEntry(entry);
                            question.setQuestion(err);
                            Question::Response response = question.ask();
                            if (response == Question::CancelResponse) {
                                // Use can delete files manually
                                EntryInternal newEntry = entry;
                                newEntry.setStatus(KNS3::Entry::Installed);
                                Q_EMIT signalEntryChanged(newEntry);
                                return;
                            }
                        } else {
                            qCDebug(KNEWSTUFFCORE) << "Command executed successfully:" << command;
                        }
                        deleteFilesAndMarkAsUninstalled();
                    };
                    connect(process, &QProcess::finished, this, onProcessFinished);
                }
            }
            // If the entry got deleted, but the RemoveDeadEntries option was not selected this case can happen
            if (!validFileExisted) {
                deleteFilesAndMarkAsUninstalled();
            }
        } else {
            deleteFilesAndMarkAsUninstalled();
        }
    }
}

Installation::UncompressionOptions Installation::uncompressionSetting() const
{
    return property("uncompressSetting").value<UncompressionOptions>();
}

#if KNEWSTUFFCORE_BUILD_DEPRECATED_SINCE(5, 31)
void Installation::slotInstallationVerification(int result)
{
    Q_UNUSED(result)
    // Deprecated, was wired up to defunct Security class.
}
#endif

QStringList Installation::archiveEntries(const QString &path, const KArchiveDirectory *dir)
{
    QStringList files;
    const auto lst = dir->entries();
    for (const QString &entry : lst) {
        const auto currentEntry = dir->entry(entry);

        const QString childPath = QDir(path).filePath(entry);
        if (currentEntry->isFile()) {
            files << childPath;
        } else if (currentEntry->isDirectory()) {
            files << childPath + QStringLiteral("/*");
        }
    }
    return files;
}

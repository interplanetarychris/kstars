/*  INDI CCD
    Copyright (C) 2012 Jasem Mutlaq <mutlaqja@ikarustech.com>

    This application is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
 */

#include "indiccd.h"

#include "config-kstars.h"

#include "indi_debug.h"

#include "clientmanager.h"
#include "driverinfo.h"
#include "guimanager.h"
#include "kspaths.h"
#include "kstars.h"
#include "kstarsdata.h"
#include "Options.h"
#include "streamwg.h"
//#include "ekos/manager.h"
#ifdef HAVE_CFITSIO
#include "fitsviewer/fitsdata.h"
#endif

#include <KNotifications/KNotification>
#include <QImageReader>
#include <QStatusBar>
#include <QtConcurrent>

#include <basedevice.h>

const QStringList RAWFormats = { "cr2", "cr3", "crw", "nef", "raf", "dng", "arw" };

namespace
{
void addFITSKeywords(const QString &filename, const QString &filter_used)
{
#ifdef HAVE_CFITSIO
    int status = 0;

    if (filter_used.isEmpty() == false)
    {
        QString filt(filter_used);
        QString key_comment("Filter name");
        filt.replace(' ', '_');

        fitsfile *fptr = nullptr;

        // Use open diskfile as it does not use extended file names which has problems opening
        // files with [ ] or ( ) in their names.
        if (fits_open_diskfile(&fptr, filename.toLatin1(), READONLY, &status))
        {
            fits_report_error(stderr, status);
            return;
        }

        if (fits_movabs_hdu(fptr, 1, IMAGE_HDU, &status))
        {
            fits_report_error(stderr, status);
            return;
        }

        if (fits_update_key_str(fptr, "FILTER", filt.toLatin1().data(), key_comment.toLatin1().data(), &status))
        {
            fits_report_error(stderr, status);
            return;
        }

        fits_flush_file(fptr, &status);
        fits_close_file(fptr, &status);
    }
#endif
}

// Internal function to write an image blob to disk.
bool WriteImageFileInternal(const QString &filename, char *buffer, const size_t size,
                            bool add_fits_keywords, const QString &filter)
{
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly))
    {
        qCCritical(KSTARS_INDI) << "ISD:CCD Error: Unable to open write file: " <<
                                filename;
        return false;
    }
    size_t n = 0;
    QDataStream out(&file);
    for (size_t nr = 0; nr < size; nr += n)
        n = out.writeRawData(buffer + nr, size - nr);
    file.flush();
    file.close();
    file.setPermissions(QFileDevice::ReadUser |
                        QFileDevice::WriteUser |
                        QFileDevice::ReadGroup |
                        QFileDevice::ReadOther);
    if (add_fits_keywords)
        addFITSKeywords(filename, filter);
    return true;
}
}

namespace ISD
{
CCDChip::CCDChip(ISD::CCD *ccd, ChipType cType)
{
    baseDevice    = ccd->getBaseDevice();
    clientManager = ccd->getDriverInfo()->getClientManager();
    parentCCD     = ccd;
    type          = cType;
}

FITSView *CCDChip::getImageView(FITSMode imageType)
{
    switch (imageType)
    {
        case FITS_NORMAL:
            return normalImage;

        case FITS_FOCUS:
            return focusImage;

        case FITS_GUIDE:
            return guideImage;

        case FITS_CALIBRATE:
            return calibrationImage;

        case FITS_ALIGN:
            return alignImage;
    }

    return nullptr;
}

void CCDChip::setImageView(FITSView *image, FITSMode imageType)
{
    switch (imageType)
    {
        case FITS_NORMAL:
            normalImage = image;
            break;

        case FITS_FOCUS:
            focusImage = image;
            break;

        case FITS_GUIDE:
            guideImage = image;
            break;

        case FITS_CALIBRATE:
            calibrationImage = image;
            break;

        case FITS_ALIGN:
            alignImage = image;
            break;
    }

    if (image)
        imageData = image->getImageData();
}

bool CCDChip::getFrameMinMax(int *minX, int *maxX, int *minY, int *maxY, int *minW, int *maxW, int *minH, int *maxH)
{
    INumberVectorProperty *frameProp = nullptr;

    switch (type)
    {
        case PRIMARY_CCD:
            frameProp = baseDevice->getNumber("CCD_FRAME");
            break;

        case GUIDE_CCD:
            frameProp = baseDevice->getNumber("GUIDER_FRAME");
            break;
    }

    if (frameProp == nullptr)
        return false;

    INumber *arg = IUFindNumber(frameProp, "X");

    if (arg == nullptr)
        return false;

    if (minX)
        *minX = arg->min;
    if (maxX)
        *maxX = arg->max;

    arg = IUFindNumber(frameProp, "Y");

    if (arg == nullptr)
        return false;

    if (minY)
        *minY = arg->min;
    if (maxY)
        *maxY = arg->max;

    arg = IUFindNumber(frameProp, "WIDTH");

    if (arg == nullptr)
        return false;

    if (minW)
        *minW = arg->min;
    if (maxW)
        *maxW = arg->max;

    arg = IUFindNumber(frameProp, "HEIGHT");

    if (arg == nullptr)
        return false;

    if (minH)
        *minH = arg->min;
    if (maxH)
        *maxH = arg->max;

    return true;
}

bool CCDChip::setImageInfo(uint16_t width, uint16_t height, double pixelX, double pixelY, uint8_t bitdepth)
{
    INumberVectorProperty *ccdInfoProp = nullptr;

    switch (type)
    {
        case PRIMARY_CCD:
            ccdInfoProp = baseDevice->getNumber("CCD_INFO");
            break;

        case GUIDE_CCD:
            ccdInfoProp = baseDevice->getNumber("GUIDER_INFO");
            break;
    }

    if (ccdInfoProp == nullptr)
        return false;

    ccdInfoProp->np[0].value = width;
    ccdInfoProp->np[1].value = height;
    ccdInfoProp->np[2].value = std::hypotf(pixelX, pixelY);
    ccdInfoProp->np[3].value = pixelX;
    ccdInfoProp->np[4].value = pixelY;
    ccdInfoProp->np[5].value = bitdepth;

    clientManager->sendNewNumber(ccdInfoProp);

    return true;
}

bool CCDChip::getImageInfo(uint16_t &width, uint16_t &height, double &pixelX, double &pixelY, uint8_t &bitdepth)
{
    INumberVectorProperty *ccdInfoProp = nullptr;

    switch (type)
    {
        case PRIMARY_CCD:
            ccdInfoProp = baseDevice->getNumber("CCD_INFO");
            break;

        case GUIDE_CCD:
            ccdInfoProp = baseDevice->getNumber("GUIDER_INFO");
            break;
    }

    if (ccdInfoProp == nullptr)
        return false;

    width    = ccdInfoProp->np[0].value;
    height   = ccdInfoProp->np[1].value;
    pixelX   = ccdInfoProp->np[2].value;
    pixelY   = ccdInfoProp->np[3].value;
    bitdepth = ccdInfoProp->np[5].value;

    return true;
}

bool CCDChip::getBayerInfo(uint16_t &offsetX, uint16_t &offsetY, QString &pattern)
{
    ITextVectorProperty * bayerTP = baseDevice->getText("CCD_CFA");
    if (!bayerTP)
        return false;

    offsetX = QString(bayerTP->tp[0].text).toInt();
    offsetY = QString(bayerTP->tp[1].text).toInt();
    pattern = QString(bayerTP->tp[2].text);

    return true;
}

bool CCDChip::getFrame(int *x, int *y, int *w, int *h)
{
    INumberVectorProperty *frameProp = nullptr;

    switch (type)
    {
        case PRIMARY_CCD:
            frameProp = baseDevice->getNumber("CCD_FRAME");
            break;

        case GUIDE_CCD:
            frameProp = baseDevice->getNumber("GUIDER_FRAME");
            break;
    }

    if (frameProp == nullptr)
        return false;

    INumber *arg = IUFindNumber(frameProp, "X");

    if (arg == nullptr)
        return false;

    *x = arg->value;

    arg = IUFindNumber(frameProp, "Y");
    if (arg == nullptr)
        return false;

    *y = arg->value;

    arg = IUFindNumber(frameProp, "WIDTH");
    if (arg == nullptr)
        return false;

    *w = arg->value;

    arg = IUFindNumber(frameProp, "HEIGHT");
    if (arg == nullptr)
        return false;

    *h = arg->value;

    return true;
}

bool CCDChip::resetFrame()
{
    INumberVectorProperty *frameProp = nullptr;

    switch (type)
    {
        case PRIMARY_CCD:
            frameProp = baseDevice->getNumber("CCD_FRAME");
            break;

        case GUIDE_CCD:
            frameProp = baseDevice->getNumber("GUIDER_FRAME");
            break;
    }

    if (frameProp == nullptr)
        return false;

    INumber *xarg = IUFindNumber(frameProp, "X");
    INumber *yarg = IUFindNumber(frameProp, "Y");
    INumber *warg = IUFindNumber(frameProp, "WIDTH");
    INumber *harg = IUFindNumber(frameProp, "HEIGHT");

    if (xarg && yarg && warg && harg)
    {
        if (!std::fabs(xarg->value - xarg->min) &&
                !std::fabs(yarg->value - yarg->min) &&
                !std::fabs(warg->value - warg->max) &&
                !std::fabs(harg->value - harg->max))
            return false;

        xarg->value = xarg->min;
        yarg->value = yarg->min;
        warg->value = warg->max;
        harg->value = harg->max;

        clientManager->sendNewNumber(frameProp);
        return true;
    }

    return false;
}

bool CCDChip::setFrame(int x, int y, int w, int h, bool force)
{
    INumberVectorProperty *frameProp = nullptr;

    switch (type)
    {
        case PRIMARY_CCD:
            frameProp = baseDevice->getNumber("CCD_FRAME");
            break;

        case GUIDE_CCD:
            frameProp = baseDevice->getNumber("GUIDER_FRAME");
            break;
    }

    if (frameProp == nullptr)
        return false;

    INumber *xarg = IUFindNumber(frameProp, "X");
    INumber *yarg = IUFindNumber(frameProp, "Y");
    INumber *warg = IUFindNumber(frameProp, "WIDTH");
    INumber *harg = IUFindNumber(frameProp, "HEIGHT");

    if (xarg && yarg && warg && harg)
    {
        if (!force &&
                !std::fabs(xarg->value - x) &&
                !std::fabs(yarg->value - y) &&
                !std::fabs(warg->value - w) &&
                !std::fabs(harg->value - h))
            return true;

        xarg->value = x;
        yarg->value = y;
        warg->value = w;
        harg->value = h;

        clientManager->sendNewNumber(frameProp);
        return true;
    }

    return false;
}

bool CCDChip::capture(double exposure)
{
    //qCDebug(KSTARS_INDI) << "IndiCCD: capture()" << (type==PRIMARY_CCD?"CCD":"Guide");
    INumberVectorProperty *expProp = nullptr;

    switch (type)
    {
        case PRIMARY_CCD:
            expProp = baseDevice->getNumber("CCD_EXPOSURE");
            break;

        case GUIDE_CCD:
            expProp = baseDevice->getNumber("GUIDER_EXPOSURE");
            break;
    }

    if (expProp == nullptr)
        return false;

    // If we have exposure presets, let's limit the exposure value
    // to the preset values if it falls within their range of max/min
    if (Options::forceDSLRPresets())
    {
        QMap<QString, double> exposurePresets = parentCCD->getExposurePresets();
        if (!exposurePresets.isEmpty())
        {
            double min, max;
            QPair<double, double> minmax = parentCCD->getExposurePresetsMinMax();
            min = minmax.first;
            max = minmax.second;
            if (exposure > min && exposure < max)
            {
                double diff = 1e6;
                double closestMatch = exposure;
                for (auto oneValue : exposurePresets.values())
                {
                    double newDiff = std::fabs(exposure - oneValue);
                    if (newDiff < diff)
                    {
                        closestMatch = oneValue;
                        diff = newDiff;
                    }
                }

                qCDebug(KSTARS_INDI) << "Requested exposure" << exposure << "closes match is" << closestMatch;
                exposure = closestMatch;
            }
        }
    }

    // clone the INumberVectorProperty, to avoid modifications to the same
    // property from two threads
    INumber n;
    strcpy(n.name, expProp->np[0].name);
    n.value = exposure;

    std::unique_ptr<INumberVectorProperty> newExpProp(new INumberVectorProperty());
    strncpy(newExpProp->device, expProp->device, MAXINDIDEVICE);
    strncpy(newExpProp->name, expProp->name, MAXINDINAME);
    strncpy(newExpProp->label, expProp->label, MAXINDILABEL);
    newExpProp->np = &n;
    newExpProp->nnp = 1;

    clientManager->sendNewNumber(newExpProp.get());

    return true;
}

bool CCDChip::abortExposure()
{
    ISwitchVectorProperty *abortProp = nullptr;

    switch (type)
    {
        case PRIMARY_CCD:
            abortProp = baseDevice->getSwitch("CCD_ABORT_EXPOSURE");
            break;

        case GUIDE_CCD:
            abortProp = baseDevice->getSwitch("GUIDER_ABORT_EXPOSURE");
            break;
    }

    if (abortProp == nullptr)
        return false;

    ISwitch *abort = IUFindSwitch(abortProp, "ABORT");

    if (abort == nullptr)
        return false;

    abort->s = ISS_ON;

    clientManager->sendNewSwitch(abortProp);

    return true;
}
bool CCDChip::canBin() const
{
    return CanBin;
}

void CCDChip::setCanBin(bool value)
{
    CanBin = value;
}
bool CCDChip::canSubframe() const
{
    return CanSubframe;
}

void CCDChip::setCanSubframe(bool value)
{
    CanSubframe = value;
}
bool CCDChip::canAbort() const
{
    return CanAbort;
}

void CCDChip::setCanAbort(bool value)
{
    CanAbort = value;
}

FITSData *CCDChip::getImageData() const
{
    return imageData;
}

int CCDChip::getISOIndex() const
{
    ISwitchVectorProperty *isoProp = baseDevice->getSwitch("CCD_ISO");

    if (isoProp == nullptr)
        return -1;

    return IUFindOnSwitchIndex(isoProp);
}

bool CCDChip::setISOIndex(int value)
{
    ISwitchVectorProperty *isoProp = baseDevice->getSwitch("CCD_ISO");

    if (isoProp == nullptr)
        return false;

    IUResetSwitch(isoProp);
    isoProp->sp[value].s = ISS_ON;

    clientManager->sendNewSwitch(isoProp);

    return true;
}

QStringList CCDChip::getISOList() const
{
    QStringList isoList;

    ISwitchVectorProperty *isoProp = baseDevice->getSwitch("CCD_ISO");

    if (isoProp == nullptr)
        return isoList;

    for (int i = 0; i < isoProp->nsp; i++)
        isoList << isoProp->sp[i].label;

    return isoList;
}

bool CCDChip::isCapturing()
{
    INumberVectorProperty *expProp = nullptr;

    switch (type)
    {
        case PRIMARY_CCD:
            expProp = baseDevice->getNumber("CCD_EXPOSURE");
            break;

        case GUIDE_CCD:
            expProp = baseDevice->getNumber("GUIDER_EXPOSURE");
            break;
    }

    if (expProp == nullptr)
        return false;

    return (expProp->s == IPS_BUSY);
}

bool CCDChip::setFrameType(const QString &name)
{
    CCDFrameType fType = FRAME_LIGHT;

    if (name == "FRAME_LIGHT" || name == "Light")
        fType = FRAME_LIGHT;
    else if (name == "FRAME_DARK" || name == "Dark")
        fType = FRAME_DARK;
    else if (name == "FRAME_BIAS" || name == "Bias")
        fType = FRAME_BIAS;
    else if (name == "FRAME_FLAT" || name == "Flat")
        fType = FRAME_FLAT;
    else
    {
        qCWarning(KSTARS_INDI) << name << " frame type is unknown." ;
        return false;
    }

    return setFrameType(fType);
}

bool CCDChip::setFrameType(CCDFrameType fType)
{
    ISwitchVectorProperty *frameProp = nullptr;

    if (type == PRIMARY_CCD)
        frameProp = baseDevice->getSwitch("CCD_FRAME_TYPE");
    else
        frameProp = baseDevice->getSwitch("GUIDER_FRAME_TYPE");
    if (frameProp == nullptr)
        return false;

    ISwitch *ccdFrame = nullptr;

    if (fType == FRAME_LIGHT)
        ccdFrame = IUFindSwitch(frameProp, "FRAME_LIGHT");
    else if (fType == FRAME_DARK)
        ccdFrame = IUFindSwitch(frameProp, "FRAME_DARK");
    else if (fType == FRAME_BIAS)
        ccdFrame = IUFindSwitch(frameProp, "FRAME_BIAS");
    else if (fType == FRAME_FLAT)
        ccdFrame = IUFindSwitch(frameProp, "FRAME_FLAT");

    if (ccdFrame == nullptr)
        return false;

    if (ccdFrame->s == ISS_ON)
        return true;

    if (fType != FRAME_LIGHT)
        captureMode = FITS_CALIBRATE;

    IUResetSwitch(frameProp);
    ccdFrame->s = ISS_ON;

    clientManager->sendNewSwitch(frameProp);

    return true;
}

CCDFrameType CCDChip::getFrameType()
{
    CCDFrameType fType               = FRAME_LIGHT;
    ISwitchVectorProperty *frameProp = nullptr;

    if (type == PRIMARY_CCD)
        frameProp = baseDevice->getSwitch("CCD_FRAME_TYPE");
    else
        frameProp = baseDevice->getSwitch("GUIDER_FRAME_TYPE");

    if (frameProp == nullptr)
        return fType;

    ISwitch *ccdFrame = nullptr;

    ccdFrame = IUFindOnSwitch(frameProp);

    if (ccdFrame == nullptr)
    {
        qCWarning(KSTARS_INDI) << "ISD:CCD Cannot find active frame in CCD!";
        return fType;
    }

    if (!strcmp(ccdFrame->name, "FRAME_LIGHT"))
        fType = FRAME_LIGHT;
    else if (!strcmp(ccdFrame->name, "FRAME_DARK"))
        fType = FRAME_DARK;
    else if (!strcmp(ccdFrame->name, "FRAME_FLAT"))
        fType = FRAME_FLAT;
    else if (!strcmp(ccdFrame->name, "FRAME_BIAS"))
        fType = FRAME_BIAS;

    return fType;
}

bool CCDChip::setBinning(CCDBinType binType)
{
    switch (binType)
    {
        case SINGLE_BIN:
            return setBinning(1, 1);
        case DOUBLE_BIN:
            return setBinning(2, 2);
        case TRIPLE_BIN:
            return setBinning(3, 3);
        case QUADRAPLE_BIN:
            return setBinning(4, 4);
    }

    return false;
}

CCDBinType CCDChip::getBinning()
{
    CCDBinType binType             = SINGLE_BIN;
    INumberVectorProperty *binProp = nullptr;

    switch (type)
    {
        case PRIMARY_CCD:
            binProp = baseDevice->getNumber("CCD_BINNING");
            break;

        case GUIDE_CCD:
            binProp = baseDevice->getNumber("GUIDER_BINNING");
            break;
    }

    if (binProp == nullptr)
        return binType;

    INumber *horBin = nullptr, *verBin = nullptr;

    horBin = IUFindNumber(binProp, "HOR_BIN");
    verBin = IUFindNumber(binProp, "VER_BIN");

    if (!horBin || !verBin)
        return binType;

    switch (static_cast<int>(horBin->value))
    {
        case 2:
            binType = DOUBLE_BIN;
            break;

        case 3:
            binType = TRIPLE_BIN;
            break;

        case 4:
            binType = QUADRAPLE_BIN;
            break;

        default:
            break;
    }

    return binType;
}

bool CCDChip::getBinning(int *bin_x, int *bin_y)
{
    INumberVectorProperty *binProp = nullptr;
    *bin_x = *bin_y = 1;

    switch (type)
    {
        case PRIMARY_CCD:
            binProp = baseDevice->getNumber("CCD_BINNING");
            break;

        case GUIDE_CCD:
            binProp = baseDevice->getNumber("GUIDER_BINNING");
            break;
    }

    if (binProp == nullptr)
        return false;

    INumber *horBin = nullptr, *verBin = nullptr;

    horBin = IUFindNumber(binProp, "HOR_BIN");
    verBin = IUFindNumber(binProp, "VER_BIN");

    if (!horBin || !verBin)
        return false;

    *bin_x = horBin->value;
    *bin_y = verBin->value;

    return true;
}

bool CCDChip::getMaxBin(int *max_xbin, int *max_ybin)
{
    if (!max_xbin || !max_ybin)
        return false;

    INumberVectorProperty *binProp = nullptr;

    *max_xbin = *max_ybin = 1;

    switch (type)
    {
        case PRIMARY_CCD:
            binProp = baseDevice->getNumber("CCD_BINNING");
            break;

        case GUIDE_CCD:
            binProp = baseDevice->getNumber("GUIDER_BINNING");
            break;
    }

    if (binProp == nullptr)
        return false;

    INumber *horBin = nullptr, *verBin = nullptr;

    horBin = IUFindNumber(binProp, "HOR_BIN");
    verBin = IUFindNumber(binProp, "VER_BIN");

    if (!horBin || !verBin)
        return false;

    *max_xbin = horBin->max;
    *max_ybin = verBin->max;

    return true;
}

bool CCDChip::setBinning(int bin_x, int bin_y)
{
    INumberVectorProperty *binProp = nullptr;

    switch (type)
    {
        case PRIMARY_CCD:
            binProp = baseDevice->getNumber("CCD_BINNING");
            break;

        case GUIDE_CCD:
            binProp = baseDevice->getNumber("GUIDER_BINNING");
            break;
    }

    if (binProp == nullptr)
        return false;

    INumber *horBin = IUFindNumber(binProp, "HOR_BIN");
    INumber *verBin = IUFindNumber(binProp, "VER_BIN");

    if (!horBin || !verBin)
        return false;

    if (!std::fabs(horBin->value - bin_x) && !std::fabs(verBin->value - bin_y))
        return true;

    if (bin_x > horBin->max || bin_y > verBin->max)
        return false;

    horBin->value = bin_x;
    verBin->value = bin_y;

    clientManager->sendNewNumber(binProp);

    return true;
}

CCD::CCD(GDInterface *iPtr) : DeviceDecorator(iPtr)
{
    dType = KSTARS_CCD;
    primaryChip.reset(new CCDChip(this, CCDChip::PRIMARY_CCD));

    readyTimer.reset(new QTimer());
    readyTimer.get()->setInterval(250);
    readyTimer.get()->setSingleShot(true);
    connect(readyTimer.get(), &QTimer::timeout, this, &CCD::ready);

    m_Media.reset(new WSMedia(this));
    connect(m_Media.get(), &WSMedia::newFile, this, &CCD::setWSBLOB);

    connect(clientManager, &ClientManager::newBLOBManager, this, &CCD::setBLOBManager, Qt::UniqueConnection);
    m_LastNotificationTS = QDateTime::currentDateTime();
}

CCD::~CCD()
{
    if (m_ImageViewerWindow)
        m_ImageViewerWindow->close();
    if (fileWriteThread.isRunning())
        fileWriteThread.waitForFinished();
    if (fileWriteBuffer != nullptr)
        delete [] fileWriteBuffer;
}

void CCD::setBLOBManager(const char *device, INDI::Property *prop)
{
    if (!prop->getRegistered())
        return;

    if (device == getDeviceName())
        emit newBLOBManager(prop);
}

void CCD::registerProperty(INDI::Property *prop)
{
    if (isConnected())
        readyTimer.get()->start();

    if (!strcmp(prop->getName(), "GUIDER_EXPOSURE"))
    {
        HasGuideHead = true;
        guideChip.reset(new CCDChip(this, CCDChip::GUIDE_CCD));
    }
    else if (!strcmp(prop->getName(), "CCD_FRAME_TYPE"))
    {
        ISwitchVectorProperty *ccdFrame = prop->getSwitch();

        primaryChip->clearFrameTypes();

        for (int i = 0; i < ccdFrame->nsp; i++)
            primaryChip->addFrameLabel(ccdFrame->sp[i].label);
    }
    else if (!strcmp(prop->getName(), "CCD_FRAME"))
    {
        INumberVectorProperty *np = prop->getNumber();
        if (np && np->p != IP_RO)
            primaryChip->setCanSubframe(true);
    }
    else if (!strcmp(prop->getName(), "GUIDER_FRAME"))
    {
        INumberVectorProperty *np = prop->getNumber();
        if (np && np->p != IP_RO)
            guideChip->setCanSubframe(true);
    }
    else if (!strcmp(prop->getName(), "CCD_BINNING"))
    {
        INumberVectorProperty *np = prop->getNumber();
        if (np && np->p != IP_RO)
            primaryChip->setCanBin(true);
    }
    else if (!strcmp(prop->getName(), "GUIDER_BINNING"))
    {
        INumberVectorProperty *np = prop->getNumber();
        if (np && np->p != IP_RO)
            guideChip->setCanBin(true);
    }
    else if (!strcmp(prop->getName(), "CCD_ABORT_EXPOSURE"))
    {
        ISwitchVectorProperty *sp = prop->getSwitch();
        if (sp && sp->p != IP_RO)
            primaryChip->setCanAbort(true);
    }
    else if (!strcmp(prop->getName(), "GUIDER_ABORT_EXPOSURE"))
    {
        ISwitchVectorProperty *sp = prop->getSwitch();
        if (sp && sp->p != IP_RO)
            guideChip->setCanAbort(true);
    }
    else if (!strcmp(prop->getName(), "CCD_TEMPERATURE"))
    {
        INumberVectorProperty *np = prop->getNumber();
        HasCooler                 = true;
        CanCool                   = (np->p != IP_RO);
        if (np)
            emit newTemperatureValue(np->np[0].value);
    }
    else if (!strcmp(prop->getName(), "CCD_COOLER"))
    {
        // Can turn cooling on/off
        HasCoolerControl = true;
    }
    else if (!strcmp(prop->getName(), "CCD_VIDEO_STREAM"))
    {
        // Has Video Stream
        HasVideoStream = true;
    }
    else if (!strcmp(prop->getName(), "CCD_TRANSFER_FORMAT"))
    {
        ISwitchVectorProperty *sp = prop->getSwitch();
        if (sp)
        {
            ISwitch *format = IUFindSwitch(sp, "FORMAT_NATIVE");
            if (format && format->s == ISS_ON)
                transferFormat = FORMAT_NATIVE;
            else
                transferFormat = FORMAT_FITS;
        }
    }
    else if (!strcmp(prop->getName(), "CCD_EXPOSURE_PRESETS"))
    {
        ISwitchVectorProperty *svp = prop->getSwitch();
        if (svp)
        {
            bool ok = false;
            for (int i = 0; i < svp->nsp; i++)
            {
                QString key = QString(svp->sp[i].label);
                double value = key.toDouble(&ok);
                if (!ok)
                {
                    QStringList parts = key.split("/");
                    if (parts.count() == 2)
                    {
                        bool numOk = false, denOk = false;
                        double numerator = parts[0].toDouble(&numOk);
                        double denominator = parts[1].toDouble(&denOk);
                        if (numOk && denOk && denominator > 0)
                        {
                            ok = true;
                            value = numerator / denominator;
                        }
                    }
                }
                if (ok)
                    m_ExposurePresets.insert(key, value);

                double min = 1e6, max = 1e-6;
                for (auto oneValue : m_ExposurePresets.values())
                {
                    if (oneValue < min)
                        min = oneValue;
                    if (oneValue > max)
                        max = oneValue;
                }
                m_ExposurePresetsMinMax = qMakePair<double, double>(min, max);
            }
        }
    }
    else if (!strcmp(prop->getName(), "CCD_EXPOSURE_LOOP"))
    {
        ISwitchVectorProperty *sp = prop->getSwitch();
        if (sp)
        {
            ISwitch *looping = IUFindSwitch(sp, "LOOP_ON");
            if (looping && looping->s == ISS_ON)
                IsLooping = true;
            else
                IsLooping = false;
        }
    }
    else if (!strcmp(prop->getName(), "TELESCOPE_TYPE"))
    {
        ISwitchVectorProperty *sp = prop->getSwitch();
        if (sp)
        {
            ISwitch *format = IUFindSwitch(sp, "TELESCOPE_PRIMARY");
            if (format && format->s == ISS_ON)
                telescopeType = TELESCOPE_PRIMARY;
            else
                telescopeType = TELESCOPE_GUIDE;
        }
    }
    else if (!strcmp(prop->getName(), "CCD_WEBSOCKET_SETTINGS"))
    {
        INumberVectorProperty *np = prop->getNumber();
        m_Media->setURL(QUrl(QString("ws://%1:%2").arg(clientManager->getHost()).arg(np->np[0].value)));
        m_Media->connectServer();
    }
    else if (!strcmp(prop->getName(), "CCD1"))
    {
        IBLOBVectorProperty *bp = prop->getBLOB();
        primaryCCDBLOB = bp->bp;
        primaryCCDBLOB->bvp = bp;
    }
    // try to find gain and/or offset property, if any
    else if ( (gainN == nullptr || offsetN == nullptr) && prop->getType() == INDI_NUMBER)
    {
        // Since gain is spread among multiple property depending on the camera providing it
        // we need to search in all possible number properties
        INumberVectorProperty *controlNP = prop->getNumber();
        if (controlNP)
        {
            for (int i = 0; i < controlNP->nnp; i++)
            {
                QString name  = QString(controlNP->np[i].name).toLower();
                QString label = QString(controlNP->np[i].label).toLower();

                if (name == "gain" || label == "gain")
                {
                    gainN = controlNP->np + i;
                    gainPerm = controlNP->p;
                }
                else if (name == "offset" || label == "offset")
                {
                    offsetN = controlNP->np + i;
                    offsetPerm = controlNP->p;
                }
            }
        }
    }

    DeviceDecorator::registerProperty(prop);
}

void CCD::removeProperty(const QString &name)
{
    if (name == "CCD_WEBSOCKET_SETTINGS")
    {
        m_Media->disconnectServer();
    }

    DeviceDecorator::removeProperty(name);
}

void CCD::processLight(ILightVectorProperty *lvp)
{
    DeviceDecorator::processLight(lvp);
}

void CCD::processNumber(INumberVectorProperty *nvp)
{
    if (!strcmp(nvp->name, "CCD_EXPOSURE"))
    {
        INumber *np = IUFindNumber(nvp, "CCD_EXPOSURE_VALUE");
        if (np)
            emit newExposureValue(primaryChip.get(), np->value, nvp->s);
        if (nvp->s == IPS_ALERT)
            emit captureFailed();
    }
    else if (!strcmp(nvp->name, "CCD_TEMPERATURE"))
    {
        HasCooler   = true;
        INumber *np = IUFindNumber(nvp, "CCD_TEMPERATURE_VALUE");
        if (np)
            emit newTemperatureValue(np->value);
    }
    else if (!strcmp(nvp->name, "GUIDER_EXPOSURE"))
    {
        INumber *np = IUFindNumber(nvp, "GUIDER_EXPOSURE_VALUE");
        if (np)
            emit newExposureValue(guideChip.get(), np->value, nvp->s);
    }
    else if (!strcmp(nvp->name, "FPS"))
    {
        emit newFPS(nvp->np[0].value, nvp->np[1].value);
    }
    else if (!strcmp(nvp->name, "CCD_RAPID_GUIDE_DATA"))
    {
        double dx = -1, dy = -1, fit = -1;
        INumber *np = nullptr;

        if (nvp->s == IPS_ALERT)
        {
            emit newGuideStarData(primaryChip.get(), -1, -1, -1);
        }
        else
        {
            np = IUFindNumber(nvp, "GUIDESTAR_X");
            if (np)
                dx = np->value;
            np = IUFindNumber(nvp, "GUIDESTAR_Y");
            if (np)
                dy = np->value;
            np = IUFindNumber(nvp, "GUIDESTAR_FIT");
            if (np)
                fit = np->value;

            if (dx >= 0 && dy >= 0 && fit >= 0)
                emit newGuideStarData(primaryChip.get(), dx, dy, fit);
        }
    }
    else if (!strcmp(nvp->name, "GUIDER_RAPID_GUIDE_DATA"))
    {
        double dx = -1, dy = -1, fit = -1;
        INumber *np = nullptr;

        if (nvp->s == IPS_ALERT)
        {
            emit newGuideStarData(guideChip.get(), -1, -1, -1);
        }
        else
        {
            np = IUFindNumber(nvp, "GUIDESTAR_X");
            if (np)
                dx = np->value;
            np = IUFindNumber(nvp, "GUIDESTAR_Y");
            if (np)
                dy = np->value;
            np = IUFindNumber(nvp, "GUIDESTAR_FIT");
            if (np)
                fit = np->value;

            if (dx >= 0 && dy >= 0 && fit >= 0)
                emit newGuideStarData(guideChip.get(), dx, dy, fit);
        }
    }

    DeviceDecorator::processNumber(nvp);
}

void CCD::processSwitch(ISwitchVectorProperty *svp)
{
    if (!strcmp(svp->name, "CCD_COOLER"))
    {
        // Can turn cooling on/off
        HasCoolerControl = true;
        emit coolerToggled(svp->sp[0].s == ISS_ON);
    }
    else if (QString(svp->name).endsWith("VIDEO_STREAM"))
    {
        // If BLOB is not enabled for this camera, then ignore all VIDEO_STREAM calls.
        if (isBLOBEnabled() == false)
            return;

        HasVideoStream = true;

        if (!streamWindow && svp->sp[0].s == ISS_ON)
        {
            streamWindow.reset(new StreamWG(this));

            INumberVectorProperty *streamFrame = baseDevice->getNumber("CCD_STREAM_FRAME");
            INumber *w = nullptr, *h = nullptr;

            if (streamFrame)
            {
                w = IUFindNumber(streamFrame, "WIDTH");
                h = IUFindNumber(streamFrame, "HEIGHT");
            }

            if (w && h)
            {
                streamW = w->value;
                streamH = h->value;
            }
            else
            {
                // Only use CCD dimensions if we are receiving raw stream and not stream of images (i.e. mjpeg..etc)
                IBLOBVectorProperty *rawBP = baseDevice->getBLOB("CCD1");
                if (rawBP)
                {
                    int x = 0, y = 0, w = 0, h = 0;
                    int binx = 0, biny = 0;

                    primaryChip->getFrame(&x, &y, &w, &h);
                    primaryChip->getBinning(&binx, &biny);
                    streamW = w / binx;
                    streamH = h / biny;
                }
            }

            streamWindow->setSize(streamW, streamH);
        }

        if (streamWindow)
        {
            connect(streamWindow.get(), &StreamWG::hidden, this, &CCD::StreamWindowHidden, Qt::UniqueConnection);
            connect(streamWindow.get(), &StreamWG::imageChanged, this, &CCD::newVideoFrame, Qt::UniqueConnection);

            streamWindow->enableStream(svp->sp[0].s == ISS_ON);
            emit videoStreamToggled(svp->sp[0].s == ISS_ON);
        }
    }
    else if (!strcmp(svp->name, "CCD_TRANSFER_FORMAT"))
    {
        ISwitch *format = IUFindSwitch(svp, "FORMAT_NATIVE");

        if (format && format->s == ISS_ON)
            transferFormat = FORMAT_NATIVE;
        else
            transferFormat = FORMAT_FITS;
    }
    else if (!strcmp(svp->name, "RECORD_STREAM"))
    {
        ISwitch *recordOFF = IUFindSwitch(svp, "RECORD_OFF");

        if (recordOFF && recordOFF->s == ISS_ON)
        {
            emit videoRecordToggled(false);
            KNotification::event(QLatin1String("RecordingStopped"), i18n("Video Recording Stopped"));
        }
        else
        {
            emit videoRecordToggled(true);
            KNotification::event(QLatin1String("RecordingStarted"), i18n("Video Recording Started"));
        }
    }
    else if (!strcmp(svp->name, "TELESCOPE_TYPE"))
    {
        ISwitch *format = IUFindSwitch(svp, "TELESCOPE_PRIMARY");
        if (format && format->s == ISS_ON)
            telescopeType = TELESCOPE_PRIMARY;
        else
            telescopeType = TELESCOPE_GUIDE;
    }
    else if (!strcmp(svp->name, "CCD_EXPOSURE_LOOP"))
    {
        ISwitch *looping = IUFindSwitch(svp, "LOOP_ON");
        if (looping && looping->s == ISS_ON)
            IsLooping = true;
        else
            IsLooping = false;
    }
    else if (streamWindow && !strcmp(svp->name, "CONNECTION"))
    {
        ISwitch *dSwitch = IUFindSwitch(svp, "DISCONNECT");

        if (dSwitch && dSwitch->s == ISS_ON)
        {
            streamWindow->enableStream(false);
            emit videoStreamToggled(false);
            streamWindow->close();
            streamWindow.reset();
        }

        //emit switchUpdated(svp);
        //return;
    }

    DeviceDecorator::processSwitch(svp);
}

void CCD::processText(ITextVectorProperty *tvp)
{
    if (!strcmp(tvp->name, "CCD_FILE_PATH"))
    {
        IText *filepath = IUFindText(tvp, "FILE_PATH");
        if (filepath)
            emit newRemoteFile(QString(filepath->text));
    }

    DeviceDecorator::processText(tvp);
}

void CCD::setWSBLOB(const QByteArray &message, const QString &extension)
{
    if (!primaryCCDBLOB)
        return;

    primaryCCDBLOB->blob = const_cast<char *>(message.data());
    primaryCCDBLOB->size = message.size();
    strncpy(primaryCCDBLOB->format, extension.toLatin1().constData(), MAXINDIFORMAT - 1);
    processBLOB(primaryCCDBLOB);

    // Disassociate
    primaryCCDBLOB->blob = nullptr;
}

void CCD::processStream(IBLOB *bp)
{
    if (!streamWindow || streamWindow->isStreamEnabled() == false)
        return;

    INumberVectorProperty *streamFrame = baseDevice->getNumber("CCD_STREAM_FRAME");
    INumber *w = nullptr, *h = nullptr;

    if (streamFrame)
    {
        w = IUFindNumber(streamFrame, "WIDTH");
        h = IUFindNumber(streamFrame, "HEIGHT");
    }

    if (w && h)
    {
        streamW = w->value;
        streamH = h->value;
    }
    else
    {
        int x, y, w, h;
        int binx, biny;

        primaryChip->getFrame(&x, &y, &w, &h);
        primaryChip->getBinning(&binx, &biny);
        streamW = w / binx;
        streamH = h / biny;
    }

    streamWindow->setSize(streamW, streamH);

    streamWindow->show();
    streamWindow->newFrame(bp);
}

bool CCD::generateFilename(const QString &format, bool batch_mode, QString *filename)
{
    QString currentDir;
    if (batch_mode)
        currentDir = fitsDir.isEmpty() ? Options::fitsDir() : fitsDir;
    else
        currentDir = KSPaths::writableLocation(QStandardPaths::TempLocation);

    if (QDir(currentDir).exists() == false)
        QDir().mkpath(currentDir);

    if (currentDir.endsWith('/') == false)
        currentDir.append('/');

    // IS8601 contains colons but they are illegal under Windows OS, so replacing them with '-'
    // The timestamp is no longer ISO8601 but it should solve interoperality issues
    // between different OS hosts
    QString ts = QDateTime::currentDateTime().toString("yyyy-MM-ddThh-mm-ss");

    if (seqPrefix.contains("_ISO8601"))
    {
        QString finalPrefix = seqPrefix;
        finalPrefix.replace("ISO8601", ts);
        *filename = currentDir + finalPrefix +
                    QString("_%1%2").arg(QString().asprintf("%03d", nextSequenceID), format);
    }
    else
        *filename = currentDir + seqPrefix + (seqPrefix.isEmpty() ? "" : "_") +
                    QString("%1%2").arg(QString().asprintf("%03d", nextSequenceID), format);

    QFile test_file(*filename);
    if (!test_file.open(QIODevice::WriteOnly))
    {
        qCCritical(KSTARS_INDI) << "ISD:CCD Error: Unable to open " << test_file.fileName();
        return false;
    }
    test_file.flush();
    test_file.close();
    return true;
}

bool CCD::writeImageFile(const QString &filename, IBLOB *bp, bool is_fits)
{
    // TODO: Not yet threading the writes for non-fits files.
    // Would need to deal with the raw conversion, etc.
    if (is_fits)
    {
        // Check if the last write is still ongoing, and if so wait.
        // It is using the fileWriteBuffer.
        if (fileWriteThread.isRunning())
        {
            fileWriteThread.waitForFinished();
        }

        // Wait until the file is written before overwritting the filename.
        fileWriteFilename = filename;

        // Will write blob data in a separate thread, and can't depend on the blob
        // memory, so copy it first.

        // Check buffer size.
        if (fileWriteBufferSize != bp->size)
        {
            if (fileWriteBuffer != nullptr)
                delete [] fileWriteBuffer;
            fileWriteBufferSize = bp->size;
            fileWriteBuffer = new char[fileWriteBufferSize];
        }

        // Copy memory, and write file on a separate thread.
        // Probably too late to return an error if the file couldn't write.
        memcpy(fileWriteBuffer, bp->blob, bp->size);
        fileWriteThread = QtConcurrent::run(WriteImageFileInternal, fileWriteFilename,
                                            fileWriteBuffer, bp->size, is_fits, filter);
        filter = "";
    }
    else
    {
        if (!WriteImageFileInternal(filename, static_cast<char*>(bp->blob), bp->size,
                                    false, filter))
            return false;
    }
    return true;
}

void CCD::setupFITSViewerWindows()
{
    normalTabID = calibrationTabID = focusTabID = guideTabID = alignTabID = -1;

    m_FITSViewerWindow = KStars::Instance()->createFITSViewer();

    connect(m_FITSViewerWindow, &FITSViewer::closed, [&](int tabIndex)
    {
        if (tabIndex == normalTabID)
            normalTabID = -1;
        else if (tabIndex == calibrationTabID)
            calibrationTabID = -1;
        else if (tabIndex == focusTabID)
            focusTabID = -1;
        else if (tabIndex == guideTabID)
            guideTabID = -1;
        else if (tabIndex == alignTabID)
            alignTabID = -1;
    });
}

void CCD::processBLOB(IBLOB *bp)
{
    // Ignore write-only BLOBs since we only receive it for state-change
    if (bp->bvp->p == IP_WO || bp->size == 0)
        return;

    BType = BLOB_OTHER;

    QString format = QString(bp->format).toLower();

    // If stream, process it first
    if (format.contains("stream") && streamWindow)
    {
        processStream(bp);
        return;
    }

    // Format without leading . (.jpg --> jpg)
    QString shortFormat = format.mid(1);

    // If it's not FITS or an image, don't process it.
    if ((QImageReader::supportedImageFormats().contains(shortFormat.toLatin1())))
        BType = BLOB_IMAGE;
    else if (format.contains("fits"))
        BType = BLOB_FITS;
    else if (RAWFormats.contains(shortFormat))
        BType = BLOB_RAW;

    if (BType == BLOB_OTHER)
    {
        DeviceDecorator::processBLOB(bp);
        return;
    }

    CCDChip *targetChip = nullptr;

    if (!strcmp(bp->name, "CCD2"))
        targetChip = guideChip.get();
    else
    {
        targetChip = primaryChip.get();
        qCDebug(KSTARS_INDI) << "processBLOB() mode " << targetChip->getCaptureMode();
    }

    // Create temporary name if ANY of the following conditions are met:
    // 1. file is preview or batch mode is not enabled
    // 2. file type is not FITS_NORMAL (focus, guide..etc)
    QString filename;
#if 0

    if (targetChip->isBatchMode() == false || targetChip->getCaptureMode() != FITS_NORMAL)
    {
        if (!writeTempImageFile(format, static_cast<char *>(bp->blob), bp->size, &filename))
        {
            emit BLOBUpdated(nullptr);
            return;
        }
        if (BType == BLOB_FITS)
            addFITSKeywords(filename, filter);

    }
#endif
    // Create file name for sequences.
    if (targetChip->isBatchMode())
    {
        // If either generating file name or writing the image file fails
        // then return
        if (!generateFilename(format, targetChip->isBatchMode(), &filename) ||
                !writeImageFile(filename, bp, BType == BLOB_FITS))
        {
            emit BLOBUpdated(nullptr);
            return;
        }
    }
    else
        filename = QDir::tempPath() + QDir::separator() + "image" + format;

    // store file name
    //    strncpy(BLOBFilename, filename.toLatin1(), MAXINDIFILENAME);
    //    bp->aux0 = targetChip;
    //    bp->aux1 = &BType;
    //    bp->aux2 = BLOBFilename;

    if (targetChip->getCaptureMode() == FITS_NORMAL && targetChip->isBatchMode() == true)
    {
        KStars::Instance()->statusBar()->showMessage(i18n("%1 file saved to %2", shortFormat.toUpper(), filename), 0);
        qCInfo(KSTARS_INDI) << shortFormat.toUpper() << "file saved to" << filename;
    }

    // Don't spam, just one notification per 3 seconds
    if (QDateTime::currentDateTime().secsTo(m_LastNotificationTS) <= -3)
    {
        KNotification::event(QLatin1String("FITSReceived"), i18n("Image file is received"));
        m_LastNotificationTS = QDateTime::currentDateTime();
    }

    // Check if we need to process RAW or regular image. Anything but FITS.
#if 0
    if (BType == BLOB_IMAGE || BType == BLOB_RAW)
    {
        bool useFITSViewer = Options::autoImageToFITS() &&
                             (Options::useFITSViewer() || (Options::useDSLRImageViewer() == false && targetChip->isBatchMode() == false));
        bool useDSLRViewer = (Options::useDSLRImageViewer() || targetChip->isBatchMode() == false);
        // For raw image, we only process them to JPG if we need to open them in the image viewer
        if (BType == BLOB_RAW && (useFITSViewer || useDSLRViewer))
        {
            QString rawFileName  = filename;
            rawFileName          = rawFileName.remove(0, rawFileName.lastIndexOf(QLatin1String("/")));

            QString templateName = QString("%1/%2.XXXXXX").arg(QDir::tempPath(), rawFileName);
            QTemporaryFile imgPreview(templateName);

            imgPreview.setAutoRemove(false);
            imgPreview.open();
            imgPreview.close();
            QString preview_filename = imgPreview.fileName();
            QString errorMessage;

            if (KSUtils::RAWToJPEG(filename, preview_filename, errorMessage) == false)
            {
                KStars::Instance()->statusBar()->showMessage(errorMessage);
                emit BLOBUpdated(bp);
                return;
            }

            // Remove tempeorary CR2 files
            if (targetChip->isBatchMode() == false)
                QFile::remove(filename);

            filename = preview_filename;
            format = ".jpg";
            shortFormat = "jpg";
        }

        // Convert to FITS if checked.
        QString output;
        if (useFITSViewer && (FITSData::ImageToFITS(filename, shortFormat, output)))
        {
            if (BType == BLOB_RAW || targetChip->isBatchMode() == false)
                QFile::remove(filename);

            filename = output;
            BType = BLOB_FITS;

            emit previewFITSGenerated(output);

            FITSData *blob_fits_data = new FITSData(targetChip->getCaptureMode());

            QFuture<bool> fitsloader = blob_fits_data->loadFromFile(filename, false);
            fitsloader.waitForFinished();
            if (!fitsloader.result())
            {
                // If reading the blob fails, we treat it the same as exposure failure
                // and recapture again if possible
                delete (blob_fits_data);
                qCCritical(KSTARS_INDI) << "failed reading FITS memory buffer";
                emit newExposureValue(targetChip, 0, IPS_ALERT);
                return;
            }
            displayFits(targetChip, filename, bp, blob_fits_data);
            return;
        }
        else if (useDSLRViewer)
        {
            if (m_ImageViewerWindow.isNull())
                m_ImageViewerWindow = new ImageViewer(getDeviceName(), KStars::Instance());

            m_ImageViewerWindow->loadImage(filename);

            emit previewJPEGGenerated(filename, m_ImageViewerWindow->metadata());
        }
    }
#endif

    // Load FITS if either:
    // #1 FITS Viewer is set to enabled.
    // #2 This is a preview, so we MUST open FITS Viewer even if disabled.
    //    if (BType == BLOB_FITS)
    //    {
    // Don't display image if the following conditions are met:
    // 1. Mode is NORMAL or CALIBRATE; and
    // 2. FITS Viewer is disabled; and
    // 3. Batch mode is enabled.
    // 4. Summary view is false.
    if ((targetChip->getCaptureMode() == FITS_NORMAL || targetChip->getCaptureMode() == FITS_CALIBRATE) &&
            Options::useFITSViewer() == false &&
            Options::useSummaryPreview() == false &&
            targetChip->isBatchMode())
    {
        emit BLOBUpdated(bp);
        emit newImage(nullptr);
        return;
    }

    QSharedPointer<FITSData> blob_data;
    QByteArray buffer = QByteArray::fromRawData(reinterpret_cast<char *>(bp->blob), bp->size);
    blob_data.reset(new FITSData(targetChip->getCaptureMode()), &QObject::deleteLater);
    if (!blob_data->loadFromBuffer(buffer, shortFormat, filename, false))
    {
        // If reading the blob fails, we treat it the same as exposure failure
        // and recapture again if possible
        qCCritical(KSTARS_INDI) << "failed reading FITS memory buffer";
        emit newExposureValue(targetChip, 0, IPS_ALERT);
        return;
    }

    handleImage(targetChip, filename, bp, blob_data);
    //    else
    //        emit BLOBUpdated(bp);
}

void CCD::handleImage(CCDChip *targetChip, const QString &filename, IBLOB *bp, QSharedPointer<FITSData> data)
{
    FITSMode captureMode = targetChip->getCaptureMode();

    // Get or Create FITSViewer if we are using FITSViewer
    // or if capture mode is calibrate since for now we are forced to open the file in the viewer
    // this should be fixed in the future and should only use FITSData
    if (Options::useFITSViewer() || targetChip->isBatchMode() == false)
    {
        if (m_FITSViewerWindow.isNull() && (captureMode == FITS_NORMAL || captureMode == FITS_CALIBRATE))
            setupFITSViewerWindows();
    }

    // Add metadata
    data->setProperty("device", getDeviceName());
    data->setProperty("blobVector", bp->bvp->name);
    data->setProperty("blobElement", bp->name);
    data->setProperty("chip", targetChip->getType());

    switch (captureMode)
    {
        case FITS_NORMAL:
        case FITS_CALIBRATE:
        {
            if (Options::useFITSViewer() || targetChip->isBatchMode() == false)
            {
                bool success = false;
                int tabIndex = -1;
                int *tabID = (captureMode == FITS_NORMAL) ? &normalTabID : &calibrationTabID;
                QUrl fileURL = QUrl::fromLocalFile(filename);
                FITSScale captureFilter = targetChip->getCaptureFilter();
                if (*tabID == -1 || Options::singlePreviewFITS() == false)
                {
                    // If image is preview and we should display all captured images in a
                    // single tab called "Preview", then set the title to "Preview",
                    // Otherwise, the title will be the captured image name
                    QString previewTitle;
                    if (targetChip->isBatchMode() == false && Options::singlePreviewFITS())
                    {
                        // If we are displaying all images from all cameras in a single FITS
                        // Viewer window, then we prefix the camera name to the "Preview" string
                        if (Options::singleWindowCapturedFITS())
                            previewTitle = i18n("%1 Preview", getDeviceName());
                        else
                            // Otherwise, just use "Preview"
                            previewTitle = i18n("Preview");
                    }

                    success = m_FITSViewerWindow->loadData(data, fileURL, &tabIndex, captureMode, captureFilter, previewTitle);
                }
                else
                    success = m_FITSViewerWindow->updateData(data, fileURL, *tabID, &tabIndex, captureFilter);

                if (!success)
                {
                    // If opening file fails, we treat it the same as exposure failure
                    // and recapture again if possible
                    qCCritical(KSTARS_INDI) << "error adding/updating FITS";
                    emit newExposureValue(targetChip, 0, IPS_ALERT);
                    return;
                }
                *tabID = tabIndex;
                targetChip->setImageView(m_FITSViewerWindow->getView(tabIndex), captureMode);
                if (Options::focusFITSOnNewImage())
                    m_FITSViewerWindow->raise();
            }

            emit BLOBUpdated(bp);
            emit newImage(data);
        }
        break;

        case FITS_FOCUS:
        case FITS_GUIDE:
        case FITS_ALIGN:
            loadImageInView(bp, targetChip, data);
            break;
    }
}

void CCD::loadImageInView(IBLOB *bp, ISD::CCDChip *targetChip, const QSharedPointer<FITSData> &data)
{
    FITSMode mode = targetChip->getCaptureMode();
    FITSView *view = targetChip->getImageView(mode);
    //QString filename = QString(static_cast<const char *>(bp->aux2));

    if (view)
    {
        view->setFilter(targetChip->getCaptureFilter());
        //if (!view->loadFITSFromData(data, filename))
        if (!view->loadData(data))
        {
            emit newExposureValue(targetChip, 0, IPS_ALERT);
            return;

        }
        // FITSViewer is shown if:
        // Image in preview mode, or useFITSViewer is true; AND
        // Image type is either NORMAL or CALIBRATION since the rest have their dedicated windows.
        // NORMAL is used for raw INDI drivers without Ekos.
        if ( (Options::useFITSViewer() || targetChip->isBatchMode() == false) &&
                (mode == FITS_NORMAL || mode == FITS_CALIBRATE))
            m_FITSViewerWindow->show();

        emit BLOBUpdated(bp);
        emit newImage(data);
    }
}

CCD::TransferFormat CCD::getTargetTransferFormat() const
{
    return targetTransferFormat;
}

void CCD::setTargetTransferFormat(const TransferFormat &value)
{
    targetTransferFormat = value;
}

//void CCD::FITSViewerDestroyed()
//{
//    normalTabID = calibrationTabID = focusTabID = guideTabID = alignTabID = -1;
//}

void CCD::StreamWindowHidden()
{
    if (baseDevice->isConnected())
    {
        // We can have more than one *_VIDEO_STREAM property active so disable them all
        ISwitchVectorProperty *streamSP = baseDevice->getSwitch("CCD_VIDEO_STREAM");
        if (streamSP)
        {
            IUResetSwitch(streamSP);
            streamSP->sp[0].s = ISS_OFF;
            streamSP->sp[1].s = ISS_ON;
            streamSP->s       = IPS_IDLE;
            clientManager->sendNewSwitch(streamSP);
        }

        streamSP = baseDevice->getSwitch("VIDEO_STREAM");
        if (streamSP)
        {
            IUResetSwitch(streamSP);
            streamSP->sp[0].s = ISS_OFF;
            streamSP->sp[1].s = ISS_ON;
            streamSP->s       = IPS_IDLE;
            clientManager->sendNewSwitch(streamSP);
        }

        streamSP = baseDevice->getSwitch("AUX_VIDEO_STREAM");
        if (streamSP)
        {
            IUResetSwitch(streamSP);
            streamSP->sp[0].s = ISS_OFF;
            streamSP->sp[1].s = ISS_ON;
            streamSP->s       = IPS_IDLE;
            clientManager->sendNewSwitch(streamSP);
        }
    }

    if (streamWindow)
        streamWindow->disconnect();
}

bool CCD::hasGuideHead()
{
    return HasGuideHead;
}

bool CCD::hasCooler()
{
    return HasCooler;
}

bool CCD::hasCoolerControl()
{
    return HasCoolerControl;
}

bool CCD::setCoolerControl(bool enable)
{
    if (HasCoolerControl == false)
        return false;

    ISwitchVectorProperty *coolerSP = baseDevice->getSwitch("CCD_COOLER");

    if (coolerSP == nullptr)
        return false;

    // Cooler ON/OFF
    ISwitch *coolerON  = IUFindSwitch(coolerSP, "COOLER_ON");
    ISwitch *coolerOFF = IUFindSwitch(coolerSP, "COOLER_OFF");
    if (coolerON == nullptr || coolerOFF == nullptr)
        return false;

    coolerON->s = enable ? ISS_ON : ISS_OFF;
    coolerOFF->s = enable ? ISS_OFF : ISS_ON;
    clientManager->sendNewSwitch(coolerSP);

    return true;
}

CCDChip *CCD::getChip(CCDChip::ChipType cType)
{
    switch (cType)
    {
        case CCDChip::PRIMARY_CCD:
            return primaryChip.get();

        case CCDChip::GUIDE_CCD:
            return guideChip.get();
    }

    return nullptr;
}

bool CCD::setRapidGuide(CCDChip *targetChip, bool enable)
{
    ISwitchVectorProperty *rapidSP = nullptr;
    ISwitch *enableS               = nullptr;

    if (targetChip == primaryChip.get())
        rapidSP = baseDevice->getSwitch("CCD_RAPID_GUIDE");
    else
        rapidSP = baseDevice->getSwitch("GUIDER_RAPID_GUIDE");

    if (rapidSP == nullptr)
        return false;

    enableS = IUFindSwitch(rapidSP, "ENABLE");

    if (enableS == nullptr)
        return false;

    // Already updated, return OK
    if ((enable && enableS->s == ISS_ON) || (!enable && enableS->s == ISS_OFF))
        return true;

    IUResetSwitch(rapidSP);
    rapidSP->sp[0].s = enable ? ISS_ON : ISS_OFF;
    rapidSP->sp[1].s = enable ? ISS_OFF : ISS_ON;

    clientManager->sendNewSwitch(rapidSP);

    return true;
}

bool CCD::configureRapidGuide(CCDChip *targetChip, bool autoLoop, bool sendImage, bool showMarker)
{
    ISwitchVectorProperty *rapidSP = nullptr;
    ISwitch *autoLoopS = nullptr, *sendImageS = nullptr, *showMarkerS = nullptr;

    if (targetChip == primaryChip.get())
        rapidSP = baseDevice->getSwitch("CCD_RAPID_GUIDE_SETUP");
    else
        rapidSP = baseDevice->getSwitch("GUIDER_RAPID_GUIDE_SETUP");

    if (rapidSP == nullptr)
        return false;

    autoLoopS   = IUFindSwitch(rapidSP, "AUTO_LOOP");
    sendImageS  = IUFindSwitch(rapidSP, "SEND_IMAGE");
    showMarkerS = IUFindSwitch(rapidSP, "SHOW_MARKER");

    if (!autoLoopS || !sendImageS || !showMarkerS)
        return false;

    // If everything is already set, let's return.
    if (((autoLoop && autoLoopS->s == ISS_ON) || (!autoLoop && autoLoopS->s == ISS_OFF)) &&
            ((sendImage && sendImageS->s == ISS_ON) || (!sendImage && sendImageS->s == ISS_OFF)) &&
            ((showMarker && showMarkerS->s == ISS_ON) || (!showMarker && showMarkerS->s == ISS_OFF)))
        return true;

    autoLoopS->s   = autoLoop ? ISS_ON : ISS_OFF;
    sendImageS->s  = sendImage ? ISS_ON : ISS_OFF;
    showMarkerS->s = showMarker ? ISS_ON : ISS_OFF;

    clientManager->sendNewSwitch(rapidSP);

    return true;
}

void CCD::updateUploadSettings(const QString &remoteDir)
{
    QString filename = seqPrefix + (seqPrefix.isEmpty() ? "" : "_") + QString("XXX");

    ITextVectorProperty *uploadSettingsTP = nullptr;
    IText *uploadT                        = nullptr;

    uploadSettingsTP = baseDevice->getText("UPLOAD_SETTINGS");
    if (uploadSettingsTP)
    {
        uploadT = IUFindText(uploadSettingsTP, "UPLOAD_DIR");
        if (uploadT && remoteDir.isEmpty() == false)
            IUSaveText(uploadT, remoteDir.toLatin1().constData());

        uploadT = IUFindText(uploadSettingsTP, "UPLOAD_PREFIX");
        if (uploadT)
            IUSaveText(uploadT, filename.toLatin1().constData());

        clientManager->sendNewText(uploadSettingsTP);
    }
}

CCD::UploadMode CCD::getUploadMode()
{
    ISwitchVectorProperty *uploadModeSP = nullptr;

    uploadModeSP = baseDevice->getSwitch("UPLOAD_MODE");

    if (uploadModeSP == nullptr)
    {
        qWarning() << "No UPLOAD_MODE in CCD driver. Please update driver to INDI compliant CCD driver.";
        return UPLOAD_CLIENT;
    }

    if (uploadModeSP)
    {
        ISwitch *modeS = nullptr;

        modeS = IUFindSwitch(uploadModeSP, "UPLOAD_CLIENT");
        if (modeS && modeS->s == ISS_ON)
            return UPLOAD_CLIENT;
        modeS = IUFindSwitch(uploadModeSP, "UPLOAD_LOCAL");
        if (modeS && modeS->s == ISS_ON)
            return UPLOAD_LOCAL;
        modeS = IUFindSwitch(uploadModeSP, "UPLOAD_BOTH");
        if (modeS && modeS->s == ISS_ON)
            return UPLOAD_BOTH;
    }

    // Default
    return UPLOAD_CLIENT;
}

bool CCD::setUploadMode(UploadMode mode)
{
    ISwitchVectorProperty *uploadModeSP = nullptr;
    ISwitch *modeS                      = nullptr;

    uploadModeSP = baseDevice->getSwitch("UPLOAD_MODE");

    if (uploadModeSP == nullptr)
    {
        qWarning() << "No UPLOAD_MODE in CCD driver. Please update driver to INDI compliant CCD driver.";
        return false;
    }

    switch (mode)
    {
        case UPLOAD_CLIENT:
            modeS = IUFindSwitch(uploadModeSP, "UPLOAD_CLIENT");
            if (modeS == nullptr)
                return false;
            if (modeS->s == ISS_ON)
                return true;
            break;

        case UPLOAD_BOTH:
            modeS = IUFindSwitch(uploadModeSP, "UPLOAD_BOTH");
            if (modeS == nullptr)
                return false;
            if (modeS->s == ISS_ON)
                return true;
            break;

        case UPLOAD_LOCAL:
            modeS = IUFindSwitch(uploadModeSP, "UPLOAD_LOCAL");
            if (modeS == nullptr)
                return false;
            if (modeS->s == ISS_ON)
                return true;
            break;
    }

    IUResetSwitch(uploadModeSP);
    modeS->s = ISS_ON;

    clientManager->sendNewSwitch(uploadModeSP);

    return true;
}

bool CCD::getTemperature(double *value)
{
    if (HasCooler == false)
        return false;

    INumberVectorProperty *temperatureNP = baseDevice->getNumber("CCD_TEMPERATURE");

    if (temperatureNP == nullptr)
        return false;

    *value = temperatureNP->np[0].value;

    return true;
}

bool CCD::setTemperature(double value)
{
    INumberVectorProperty *nvp = baseDevice->getNumber("CCD_TEMPERATURE");

    if (nvp == nullptr)
        return false;

    INumber *np = IUFindNumber(nvp, "CCD_TEMPERATURE_VALUE");

    if (np == nullptr)
        return false;

    np->value = value;

    clientManager->sendNewNumber(nvp);

    return true;
}

bool CCD::setTransformFormat(CCD::TransferFormat format)
{
    if (format == transferFormat)
        return true;

    ISwitchVectorProperty *svp = baseDevice->getSwitch("CCD_TRANSFER_FORMAT");

    if (svp == nullptr)
        return false;

    ISwitch *formatFITS   = IUFindSwitch(svp, "FORMAT_FITS");
    ISwitch *formatNative = IUFindSwitch(svp, "FORMAT_NATIVE");

    if (formatFITS == nullptr || formatNative == nullptr)
        return false;

    transferFormat = format;

    formatFITS->s   = (transferFormat == FORMAT_FITS) ? ISS_ON : ISS_OFF;
    formatNative->s = (transferFormat == FORMAT_FITS) ? ISS_OFF : ISS_ON;

    clientManager->sendNewSwitch(svp);

    return true;
}

bool CCD::setTelescopeType(TelescopeType type)
{
    if (type == telescopeType)
        return true;

    ISwitchVectorProperty *svp = baseDevice->getSwitch("TELESCOPE_TYPE");

    if (svp == nullptr)
        return false;

    ISwitch *typePrimary = IUFindSwitch(svp, "TELESCOPE_PRIMARY");
    ISwitch *typeGuide   = IUFindSwitch(svp, "TELESCOPE_GUIDE");

    if (typePrimary == nullptr || typeGuide == nullptr)
        return false;

    telescopeType = type;

    typePrimary->s = (telescopeType == TELESCOPE_PRIMARY) ? ISS_ON : ISS_OFF;
    typeGuide->s   = (telescopeType == TELESCOPE_PRIMARY) ? ISS_OFF : ISS_ON;

    clientManager->sendNewSwitch(svp);

    setConfig(SAVE_CONFIG);

    return true;
}

bool CCD::setVideoStreamEnabled(bool enable)
{
    if (HasVideoStream == false)
        return false;

    ISwitchVectorProperty *svp = baseDevice->getSwitch("CCD_VIDEO_STREAM");

    if (svp == nullptr)
        return false;

    // If already on and enable is set or vice versa no need to change anything we return true
    if ((enable && svp->sp[0].s == ISS_ON) || (!enable && svp->sp[1].s == ISS_ON))
        return true;

    svp->sp[0].s = enable ? ISS_ON : ISS_OFF;
    svp->sp[1].s = enable ? ISS_OFF : ISS_ON;

    clientManager->sendNewSwitch(svp);

    return true;
}

bool CCD::resetStreamingFrame()
{
    INumberVectorProperty *frameProp = baseDevice->getNumber("CCD_STREAM_FRAME");

    if (frameProp == nullptr)
        return false;

    INumber *xarg = IUFindNumber(frameProp, "X");
    INumber *yarg = IUFindNumber(frameProp, "Y");
    INumber *warg = IUFindNumber(frameProp, "WIDTH");
    INumber *harg = IUFindNumber(frameProp, "HEIGHT");

    if (xarg && yarg && warg && harg)
    {
        if (!std::fabs(xarg->value - xarg->min) &&
                !std::fabs(yarg->value - yarg->min) &&
                !std::fabs(warg->value - warg->max) &&
                !std::fabs(harg->value - harg->max))
            return false;

        xarg->value = xarg->min;
        yarg->value = yarg->min;
        warg->value = warg->max;
        harg->value = harg->max;

        clientManager->sendNewNumber(frameProp);
        return true;
    }

    return false;
}

bool CCD::setStreamLimits(uint16_t maxBufferSize, uint16_t maxPreviewFPS)
{
    INumberVectorProperty *limitsProp = baseDevice->getNumber("CCD_STREAM_FRAME");

    if (limitsProp == nullptr)
        return false;

    INumber *bufferMax = IUFindNumber(limitsProp, "LIMITS_BUFFER_MAX");
    INumber *previewFPS = IUFindNumber(limitsProp, "LIMITS_PREVIEW_FPS");

    if (bufferMax && previewFPS)
    {
        if(std::fabs(bufferMax->value - maxBufferSize) == 0 && std::fabs(previewFPS->value - maxPreviewFPS) == 0)
            return true;

        bufferMax->value = maxBufferSize;
        previewFPS->value = maxPreviewFPS;
        clientManager->sendNewNumber(limitsProp);
        return true;
    }

    return false;
}

bool CCD::setStreamingFrame(int x, int y, int w, int h)
{
    INumberVectorProperty *frameProp = baseDevice->getNumber("CCD_STREAM_FRAME");

    if (frameProp == nullptr)
        return false;

    INumber *xarg = IUFindNumber(frameProp, "X");
    INumber *yarg = IUFindNumber(frameProp, "Y");
    INumber *warg = IUFindNumber(frameProp, "WIDTH");
    INumber *harg = IUFindNumber(frameProp, "HEIGHT");

    if (xarg && yarg && warg && harg)
    {
        if (!std::fabs(xarg->value - x) && !std::fabs(yarg->value - y) && !std::fabs(warg->value - w)
                && !std::fabs(harg->value - h))
            return true;

        // N.B. We add offset since the X, Y are relative to whatever streaming frame is currently active
        xarg->value = qBound(xarg->min, static_cast<double>(x) + xarg->value, xarg->max);
        yarg->value = qBound(yarg->min, static_cast<double>(y) + yarg->value, yarg->max);
        warg->value = qBound(warg->min, static_cast<double>(w), warg->max);
        harg->value = qBound(harg->min, static_cast<double>(h), harg->max);

        clientManager->sendNewNumber(frameProp);
        return true;
    }

    return false;
}

bool CCD::isStreamingEnabled()
{
    if (HasVideoStream == false || !streamWindow)
        return false;

    return streamWindow->isStreamEnabled();
}

bool CCD::setSERNameDirectory(const QString &filename, const QString &directory)
{
    ITextVectorProperty *tvp = baseDevice->getText("RECORD_FILE");

    if (tvp == nullptr)
        return false;

    IText *filenameT = IUFindText(tvp, "RECORD_FILE_NAME");
    IText *dirT      = IUFindText(tvp, "RECORD_FILE_DIR");

    if (filenameT == nullptr || dirT == nullptr)
        return false;

    IUSaveText(filenameT, filename.toLatin1().data());
    IUSaveText(dirT, directory.toLatin1().data());

    clientManager->sendNewText(tvp);

    return true;
}

bool CCD::getSERNameDirectory(QString &filename, QString &directory)
{
    ITextVectorProperty *tvp = baseDevice->getText("RECORD_FILE");

    if (tvp == nullptr)
        return false;

    IText *filenameT = IUFindText(tvp, "RECORD_FILE_NAME");
    IText *dirT      = IUFindText(tvp, "RECORD_FILE_DIR");

    if (filenameT == nullptr || dirT == nullptr)
        return false;

    filename  = QString(filenameT->text);
    directory = QString(dirT->text);

    return true;
}

bool CCD::startRecording()
{
    ISwitchVectorProperty *svp = baseDevice->getSwitch("RECORD_STREAM");

    if (svp == nullptr)
        return false;

    ISwitch *recordON = IUFindSwitch(svp, "RECORD_ON");

    if (recordON == nullptr)
        return false;

    if (recordON->s == ISS_ON)
        return true;

    IUResetSwitch(svp);
    recordON->s = ISS_ON;

    clientManager->sendNewSwitch(svp);

    return true;
}

bool CCD::startDurationRecording(double duration)
{
    INumberVectorProperty *nvp = baseDevice->getNumber("RECORD_OPTIONS");

    if (nvp == nullptr)
        return false;

    INumber *durationN = IUFindNumber(nvp, "RECORD_DURATION");

    if (durationN == nullptr)
        return false;

    ISwitchVectorProperty *svp = baseDevice->getSwitch("RECORD_STREAM");

    if (svp == nullptr)
        return false;

    ISwitch *recordON = IUFindSwitch(svp, "RECORD_DURATION_ON");

    if (recordON == nullptr)
        return false;

    if (recordON->s == ISS_ON)
        return true;

    durationN->value = duration;
    clientManager->sendNewNumber(nvp);

    IUResetSwitch(svp);
    recordON->s = ISS_ON;

    clientManager->sendNewSwitch(svp);

    return true;
}

bool CCD::startFramesRecording(uint32_t frames)
{
    INumberVectorProperty *nvp = baseDevice->getNumber("RECORD_OPTIONS");

    if (nvp == nullptr)
        return false;

    INumber *frameN            = IUFindNumber(nvp, "RECORD_FRAME_TOTAL");
    ISwitchVectorProperty *svp = baseDevice->getSwitch("RECORD_STREAM");

    if (frameN == nullptr || svp == nullptr)
        return false;

    ISwitch *recordON = IUFindSwitch(svp, "RECORD_FRAME_ON");

    if (recordON == nullptr)
        return false;

    if (recordON->s == ISS_ON)
        return true;

    frameN->value = frames;
    clientManager->sendNewNumber(nvp);

    IUResetSwitch(svp);
    recordON->s = ISS_ON;

    clientManager->sendNewSwitch(svp);

    return true;
}

bool CCD::stopRecording()
{
    ISwitchVectorProperty *svp = baseDevice->getSwitch("RECORD_STREAM");

    if (svp == nullptr)
        return false;

    ISwitch *recordOFF = IUFindSwitch(svp, "RECORD_OFF");

    if (recordOFF == nullptr)
        return false;

    // If already set
    if (recordOFF->s == ISS_ON)
        return true;

    IUResetSwitch(svp);
    recordOFF->s = ISS_ON;

    clientManager->sendNewSwitch(svp);

    return true;
}

bool CCD::setFITSHeader(const QMap<QString, QString> &values)
{
    ITextVectorProperty *tvp = baseDevice->getText("FITS_HEADER");

    if (tvp == nullptr)
        return false;

    QMapIterator<QString, QString> i(values);

    while (i.hasNext())
    {
        i.next();

        IText *headerT = IUFindText(tvp, i.key().toLatin1().data());

        if (headerT == nullptr)
            continue;

        IUSaveText(headerT, i.value().toLatin1().data());
    }

    clientManager->sendNewText(tvp);

    return true;
}

bool CCD::setGain(double value)
{
    if (gainN == nullptr)
        return false;

    gainN->value = value;
    clientManager->sendNewNumber(gainN->nvp);
    return true;
}

bool CCD::getGain(double *value)
{
    if (gainN == nullptr)
        return false;

    *value = gainN->value;

    return true;
}

bool CCD::getGainMinMaxStep(double *min, double *max, double *step)
{
    if (gainN == nullptr)
        return false;

    *min  = gainN->min;
    *max  = gainN->max;
    *step = gainN->step;

    return true;
}

bool CCD::setOffset(double value)
{
    if (offsetN == nullptr)
        return false;

    offsetN->value = value;
    clientManager->sendNewNumber(offsetN->nvp);
    return true;
}

bool CCD::getOffset(double *value)
{
    if (offsetN == nullptr)
        return false;

    *value = offsetN->value;

    return true;
}

bool CCD::getOffsetMinMaxStep(double *min, double *max, double *step)
{
    if (offsetN == nullptr)
        return false;

    *min  = offsetN->min;
    *max  = offsetN->max;
    *step = offsetN->step;

    return true;
}

bool CCD::isBLOBEnabled()
{
    return (clientManager->isBLOBEnabled(getDeviceName(), "CCD1"));
}

bool CCD::setBLOBEnabled(bool enable, const QString &prop)
{
    clientManager->setBLOBEnabled(enable, getDeviceName(), prop);

    return true;
}

bool CCD::setExposureLoopingEnabled(bool enable)
{
    // Set value immediately
    IsLooping = enable;

    ISwitchVectorProperty *svp = baseDevice->getSwitch("CCD_EXPOSURE_LOOP");

    if (svp == nullptr)
        return false;

    svp->sp[0].s = enable ? ISS_ON : ISS_OFF;
    svp->sp[1].s = enable ? ISS_OFF : ISS_ON;
    clientManager->sendNewSwitch(svp);

    return true;
}

bool CCD::setExposureLoopCount(uint32_t count)
{
    INumberVectorProperty *nvp = baseDevice->getNumber("CCD_EXPOSURE_LOOP_COUNT");

    if (nvp == nullptr)
        return false;

    nvp->np[0].value = count;

    clientManager->sendNewNumber(nvp);

    return true;
}

bool CCD::setStreamExposure(double duration)
{
    INumberVectorProperty *nvp = baseDevice->getNumber("STREAMING_EXPOSURE");

    if (nvp == nullptr)
        return false;

    nvp->np[0].value = duration;

    clientManager->sendNewNumber(nvp);

    return true;
}

bool CCD::getStreamExposure(double *duration)
{
    INumberVectorProperty *nvp = baseDevice->getNumber("STREAMING_EXPOSURE");

    if (nvp == nullptr)
        return false;

    *duration = nvp->np[0].value;

    return true;
}

bool CCD::isCoolerOn()
{
    ISwitchVectorProperty *svp = baseDevice->getSwitch("CCD_COOLER");

    if (svp == nullptr)
        return false;

    return (svp->sp[0].s == ISS_ON);
}

}

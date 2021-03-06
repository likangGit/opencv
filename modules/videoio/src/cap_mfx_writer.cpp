// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html

#include "cap_mfx_writer.hpp"
#include "opencv2/core/base.hpp"
#include "cap_mfx_common.hpp"

using namespace std;
using namespace cv;

inline mfxU32 codecIdByFourCC(int fourcc)
{
    if (fourcc == CC_X264 || fourcc == CC_H264 || fourcc == CC_AVC)
        return MFX_CODEC_AVC;
    else if (fourcc == CC_H265 || fourcc == CC_HEVC)
        return MFX_CODEC_HEVC;
    else if (fourcc == CC_MPG2)
        return MFX_CODEC_MPEG2;
    else
        return (mfxU32)-1;
}

VideoWriter_IntelMFX::VideoWriter_IntelMFX(const String &filename, int _fourcc, double fps, Size frameSize_, bool)
    : session(0), plugin(0), deviceHandler(0), bs(0), encoder(0), pool(0), frameSize(frameSize_), good(false)
{
    mfxStatus res = MFX_ERR_NONE;

    if (frameSize.width % 2 || frameSize.height % 2)
    {
        MSG(cerr << "MFX: Invalid frame size passed to encoder" << endl);
        return;
    }

    // Init device and session

    deviceHandler = new VAHandle();
    session = new MFXVideoSession();
    if (!deviceHandler->init(*session))
    {
        MSG(cerr << "MFX: Can't initialize session" << endl);
        return;
    }

    // Load appropriate plugin

    mfxU32 codecId = codecIdByFourCC(_fourcc);
    if (codecId == (mfxU32)-1)
    {
        MSG(cerr << "MFX: Unsupported FourCC: " << FourCC(_fourcc) << endl);
        return;
    }
    plugin = Plugin::loadEncoderPlugin(*session, codecId);
    if (plugin && !plugin->isGood())
    {
        MSG(cerr << "MFX: LoadPlugin failed for codec: " << codecId << " (" << FourCC(_fourcc) << ")" << endl);
        return;
    }

    // Init encoder

    encoder = new MFXVideoENCODE(*session);
    mfxVideoParam params;
    memset(&params, 0, sizeof(params));
    params.mfx.CodecId = codecId;
    params.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    params.mfx.TargetKbps = frameSize.area() * fps / 500; // TODO: set in options
    params.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
    params.mfx.FrameInfo.FrameRateExtN = cvRound(fps * 1000);
    params.mfx.FrameInfo.FrameRateExtD = 1000;
    params.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    params.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    params.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    params.mfx.FrameInfo.CropX = 0;
    params.mfx.FrameInfo.CropY = 0;
    params.mfx.FrameInfo.CropW = frameSize.width;
    params.mfx.FrameInfo.CropH = frameSize.height;
    params.mfx.FrameInfo.Width = alignSize(frameSize.width, 32);
    params.mfx.FrameInfo.Height = alignSize(frameSize.height, 32);
    params.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
    res = encoder->Query(&params, &params);
    DBG(cout << "MFX Query: " << res << endl << params.mfx << params.mfx.FrameInfo);
    if (res < MFX_ERR_NONE)
    {
        MSG(cerr << "MFX: Query failed: " << res << endl);
        return;
    }

    // Init surface pool
    pool = SurfacePool::create(encoder, params);
    if (!pool)
    {
        MSG(cerr << "MFX: Failed to create surface pool" << endl);
        return;
    }

    // Init encoder
    res = encoder->Init(&params);
    DBG(cout << "MFX Init: " << res << endl << params.mfx.FrameInfo);
    if (res < MFX_ERR_NONE)
    {
        MSG(cerr << "MFX: Failed to init encoder: " << res << endl);
        return;
    }

    // Open output bitstream
    {
        mfxVideoParam par;
        memset(&par, 0, sizeof(par));
        res = encoder->GetVideoParam(&par);
        DBG(cout << "MFX GetVideoParam: " << res << endl << "requested " << par.mfx.BufferSizeInKB << " kB" << endl);
        CV_Assert(res >= MFX_ERR_NONE);
        bs = new WriteBitstream(filename.c_str(), par.mfx.BufferSizeInKB * 1024 * 2);
        if (!bs->isOpened())
        {
            MSG(cerr << "MFX: Failed to open output file: " << filename << endl);
            return;
        }
    }

    good = true;
}

VideoWriter_IntelMFX::~VideoWriter_IntelMFX()
{
    if (isOpened())
    {
        DBG(cout << "====== Drain bitstream..." << endl);
        Mat dummy;
        while (write_one(dummy)) {}
        DBG(cout << "====== Drain Finished" << endl);
    }
    cleanup(bs);
    cleanup(pool);
    cleanup(encoder);
    cleanup(plugin);
    cleanup(session);
    cleanup(deviceHandler);
}

double VideoWriter_IntelMFX::getProperty(int) const
{
    MSG(cerr << "MFX: getProperty() is not implemented" << endl);
    return 0;
}

bool VideoWriter_IntelMFX::setProperty(int, double)
{
    MSG(cerr << "MFX: setProperty() is not implemented" << endl);
    return false;
}

bool VideoWriter_IntelMFX::isOpened() const
{
    return good;
}

void VideoWriter_IntelMFX::write(cv::InputArray input)
{
    write_one(input);
}

inline static void to_nv12(cv::InputArray bgr, cv::Mat & Y, cv::Mat & UV)
{
    const int height = bgr.rows();
    const int width = bgr.cols();
    Mat yuv;
    cvtColor(bgr, yuv, CV_BGR2YUV_I420);
    CV_Assert(yuv.isContinuous());
    Mat Y_(Y, Rect(0, 0, width, height));
    yuv.rowRange(0, height).copyTo(Y_);
    Mat UV_planar(height, width / 2, CV_8UC1, yuv.ptr(height));
    Mat u_and_v[2] = {
        UV_planar.rowRange(0, height / 2),
        UV_planar.rowRange(height / 2, height),
    };
    Mat uv;
    cv::merge(u_and_v, 2, uv);
    Mat UV_(UV, Rect(0, 0, width, height / 2));
    uv.reshape(1).copyTo(UV_);
}

bool VideoWriter_IntelMFX::write_one(cv::InputArray bgr)
{
    mfxStatus res;
    mfxFrameSurface1 *workSurface = 0;
    mfxSyncPoint sync;

    if (!bgr.empty() && (bgr.dims() != 2 || bgr.type() != CV_8UC3 || bgr.size() != frameSize))
    {
        MSG(cerr << "MFX: invalid frame passed to encoder: "
            << "dims/depth/cn=" << bgr.dims() << "/" << bgr.depth() << "/" << bgr.channels()
            << ", size=" << bgr.size() << endl);
        return false;

    }
    if (!bgr.empty())
    {
        workSurface = pool->getFreeSurface();
        if (!workSurface)
        {
            // not enough surfaces
            MSG(cerr << "MFX: Failed to get free surface" << endl);
            return false;
        }
        const int rows = workSurface->Info.Height;
        const int cols = workSurface->Info.Width;
        Mat Y(rows, cols, CV_8UC1, workSurface->Data.Y, workSurface->Data.Pitch);
        Mat UV(rows / 2, cols, CV_8UC1, workSurface->Data.UV, workSurface->Data.Pitch);
        to_nv12(bgr, Y, UV);
        CV_Assert(Y.ptr() == workSurface->Data.Y);
        CV_Assert(UV.ptr() == workSurface->Data.UV);
    }

    while (true)
    {
        outSurface = 0;
        DBG(cout << "Calling with surface: " << workSurface << endl);
        res = encoder->EncodeFrameAsync(NULL, workSurface, &bs->stream, &sync);
        if (res == MFX_ERR_NONE)
        {
            res = session->SyncOperation(sync, 1000); // 1 sec, TODO: provide interface to modify timeout
            if (res == MFX_ERR_NONE)
            {
                // ready to write
                if (!bs->write())
                {
                    MSG(cerr << "MFX: Failed to write bitstream" << endl);
                    return false;
                }
                else
                {
                    DBG(cout << "Write bitstream" << endl);
                    return true;
                }
            }
            else
            {
                MSG(cerr << "MFX: Sync error: " << res << endl);
                return false;
            }
        }
        else if (res == MFX_ERR_MORE_DATA)
        {
            DBG(cout << "ERR_MORE_DATA" << endl);
            return false;
        }
        else if (res == MFX_WRN_DEVICE_BUSY)
        {
            DBG(cout << "Waiting for device" << endl);
            sleep(1);
            continue;
        }
        else
        {
            MSG(cerr << "MFX: Bad status: " << res << endl);
            return false;
        }
        return true;
    }
}

Ptr<VideoWriter_IntelMFX> VideoWriter_IntelMFX::create(const String &filename, int _fourcc, double fps, Size frameSize, bool isColor)
{
    if (codecIdByFourCC(_fourcc) > 0)
    {
        Ptr<VideoWriter_IntelMFX> a = makePtr<VideoWriter_IntelMFX>(filename, _fourcc, fps, frameSize, isColor);
        if (a->isOpened())
            return a;
    }
    return Ptr<VideoWriter_IntelMFX>();
}

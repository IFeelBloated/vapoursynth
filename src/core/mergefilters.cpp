/*
* Copyright (c) 2012-2019 Fredrik Mellbin
*
* This file is part of VapourSynth.
*
* VapourSynth is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* VapourSynth is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with VapourSynth; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include <stdlib.h>
#include "cpufeatures.h"
#include "filtershared.h"
#include "filtersharedcpp.h"
#include "internalfilters.h"
#include "kernel/cpulevel.h"
#include "kernel/merge.h"
#include "VSHelper4.h"
#include <memory>
#include <algorithm>

//////////////////////////////////////////
// PreMultiply

typedef struct { // FIXME, do something about > 2 node filters, implement a template taking a fixed array size?
    VSNodeRef *node1;
    VSNodeRef *node2;
    VSNodeRef *node2_23;
    const VSVideoInfo *vi;
} PreMultiplyData;

static int getLimitedRangeOffset(const VSFrameRef *f, const VSVideoInfo *vi, const VSAPI *vsapi) {
    int err;
    int limited = !!vsapi->propGetInt(vsapi->getFramePropsRO(f), "_ColorRange", 0, &err);
    if (err)
        limited = (vi->format.colorFamily == cfGray || vi->format.colorFamily == cfYUV || vi->format.colorFamily == cfYCoCg);
    return (limited ? (16 << (vi->format.bitsPerSample - 8)) : 0);
}

static const VSFrameRef *VS_CC preMultiplyGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    PreMultiplyData *d = reinterpret_cast<PreMultiplyData *>(instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node1, frameCtx);
        vsapi->requestFrameFilter(n, d->node2, frameCtx);
        if (d->node2_23)
            vsapi->requestFrameFilter(n, d->node2_23, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src1 = vsapi->getFrameFilter(n, d->node1, frameCtx);
        const VSFrameRef *src2 = vsapi->getFrameFilter(n, d->node2, frameCtx);
        const VSFrameRef *src2_23 = 0;
        if (d->node2_23)
            src2_23 = vsapi->getFrameFilter(n, d->node2_23, frameCtx);
        VSFrameRef *dst = vsapi->newVideoFrame(&d->vi->format, d->vi->width, d->vi->height, src1, core);
        for (int plane = 0; plane < d->vi->format.numPlanes; plane++) {
            int h = vsapi->getFrameHeight(src1, plane);
            int w = vsapi->getFrameWidth(src1, plane);
            ptrdiff_t stride = vsapi->getStride(src1, plane);
            const uint8_t *srcp1 = vsapi->getReadPtr(src1, plane);
            const uint8_t *srcp2 = vsapi->getReadPtr(plane > 0 ? src2_23 : src2, 0);
            uint8_t * VS_RESTRICT dstp = vsapi->getWritePtr(dst, plane);
            bool yuvhandling = (plane > 0) && (d->vi->format.colorFamily == cfYUV || d->vi->format.colorFamily == cfYCoCg);
            int offset = getLimitedRangeOffset(src1, d->vi, vsapi);

            if (d->vi->format.sampleType == stInteger) {
                if (d->vi->format.bytesPerSample == 1) {
                    if (yuvhandling) {
                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < w; x++) {
                                uint8_t s1 = srcp1[x];
                                uint8_t s2 = srcp2[x];
                                dstp[x] = ((((s1 - 128) * (((s2 >> 1) & 1) + s2))) >> 8) + 128;
                            }
                            srcp1 += stride;
                            srcp2 += stride;
                            dstp += stride;
                        }
                    } else {
                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < w; x++) {
                                uint8_t s1 = srcp1[x];
                                uint8_t s2 = srcp2[x];
                                dstp[x] = ((((s1 - offset) * (((s2 >> 1) & 1) + s2)) + 128) >> 8) + offset;
                            }
                            srcp1 += stride;
                            srcp2 += stride;
                            dstp += stride;
                        }
                    }
                } else if (d->vi->format.bytesPerSample == 2) {
                    const unsigned shift = d->vi->format.bitsPerSample;
                    const int halfpoint = 1 << (shift - 1);
                    const int maxvalue = (1 << shift) - 1;
                    if (yuvhandling) {
                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < w; x++) {
                                uint16_t s1 = ((const uint16_t *)srcp1)[x];
                                uint16_t s2 = VSMIN(((const uint16_t *)srcp2)[x], maxvalue);
                                ((uint16_t *)dstp)[x] = (((s1 - halfpoint) * (((s2 >> 1) & 1) + s2)) >> shift) + halfpoint;
                            }
                            srcp1 += stride;
                            srcp2 += stride;
                            dstp += stride;
                        }
                    } else {
                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < w; x++) {
                                uint16_t s1 = ((const uint16_t *)srcp1)[x];
                                uint16_t s2 = ((const uint16_t *)srcp2)[x];
                                ((uint16_t *)dstp)[x] = ((((s1 - offset) * (((s2 >> 1) & 1) + s2)) + halfpoint) >> shift) + offset;
                            }
                            srcp1 += stride;
                            srcp2 += stride;
                            dstp += stride;
                        }
                    }
                }
            } else if (d->vi->format.sampleType == stFloat) {
                if (d->vi->format.bytesPerSample == 4) {
                    for (int y = 0; y < h; y++) {
                        for (int x = 0; x < w; x++)
                            ((float *)dstp)[x] = ((const float *)srcp1)[x] * ((const float *)srcp2)[x];
                        srcp1 += stride;
                        srcp2 += stride;
                        dstp += stride;
                    }
                }
            }
        }

        vsapi->freeFrame(src1);
        vsapi->freeFrame(src2);
        vsapi->freeFrame(src2_23);
        return dst;
    }

    return nullptr;
}

static void VS_CC preMultiplyFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    PreMultiplyData *d = reinterpret_cast<PreMultiplyData *>(instanceData);
    vsapi->freeNode(d->node1);
    vsapi->freeNode(d->node2);
    vsapi->freeNode(d->node2_23);
    delete d;
}

static void VS_CC preMultiplyCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<PreMultiplyData> d(new PreMultiplyData());

    d->node1 = vsapi->propGetNode(in, "clip", 0, 0);
    d->node2 = vsapi->propGetNode(in, "alpha", 0, 0);
    d->node2_23 = 0;

    d->vi = vsapi->getVideoInfo(d->node1);

    const VSVideoInfo *alphavi = vsapi->getVideoInfo(d->node2);

    if (alphavi->format.colorFamily != cfGray || alphavi->format.sampleType != d->vi->format.sampleType || alphavi->format.bitsPerSample != d->vi->format.bitsPerSample) {
        vsapi->freeNode(d->node1);
        vsapi->freeNode(d->node2);
        RETERROR("PreMultiply: alpha clip must be grayscale and same sample format and bitdepth as main clip");
    }

    if (!isConstantVideoFormat(d->vi) || !isConstantVideoFormat(alphavi) || d->vi->width != alphavi->width || d->vi->height != alphavi->height) {
        vsapi->freeNode(d->node1);
        vsapi->freeNode(d->node2);
        RETERROR("PreMultiply: both clips must have the same constant format and dimensions");
    }

    if (!is8to16orFloatFormatCheck(d->vi->format)) {
        vsapi->freeNode(d->node1);
        vsapi->freeNode(d->node2);
        RETERROR("PreMultiply: only 8-16 bit integer and 32 bit float input supported");
    }

    // do we need to resample the first mask plane and use it for all the planes?
    if ((d->vi->format.numPlanes > 1) && (d->vi->format.subSamplingH > 0 || d->vi->format.subSamplingW > 0)) {
        VSMap *min = vsapi->createMap();
        vsapi->propSetNode(min, "clip", d->node2, paAppend);
        vsapi->propSetInt(min, "width", d->vi->width >> d->vi->format.subSamplingW, paAppend);
        vsapi->propSetInt(min, "height", d->vi->height >> d->vi->format.subSamplingH, paAppend);
        VSMap *mout = vsapi->invoke(vsapi->getPluginById("com.vapoursynth.resize", core), "Bilinear", min);
        d->node2_23 = vsapi->propGetNode(mout, "clip", 0, 0);
        vsapi->freeMap(mout);
        vsapi->freeMap(min);
    } else if (d->vi->format.numPlanes > 1) {
        d->node2_23 = vsapi->cloneNodeRef(d->node2);
    }

    vsapi->createVideoFilter(out, "PreMultiply", d->vi, 1, preMultiplyGetFrame, preMultiplyFree, fmParallel, 0, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// Merge

typedef struct {
    const VSVideoInfo *vi;
    unsigned weight[3];
    float fweight[3];
    int process[3];
    int cpulevel;
} MergeDataExtra;

typedef DualNodeData<MergeDataExtra> MergeData;

const unsigned MergeShift = 15;

static const VSFrameRef *VS_CC mergeGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    MergeData *d = reinterpret_cast<MergeData *>(instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node1, frameCtx);
        vsapi->requestFrameFilter(n, d->node2, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src1 = vsapi->getFrameFilter(n, d->node1, frameCtx);
        const VSFrameRef *src2 = vsapi->getFrameFilter(n, d->node2, frameCtx);
        const int pl[] = {0, 1, 2};
        const VSFrameRef *fs[] = { 0, src1, src2 };
        const VSFrameRef *fr[] = {fs[d->process[0]], fs[d->process[1]], fs[d->process[2]]};
        VSFrameRef *dst = vsapi->newVideoFrame2(&d->vi->format, d->vi->width, d->vi->height, fr, pl, src1, core);
        for (int plane = 0; plane < d->vi->format.numPlanes; plane++) {
            if (d->process[plane] == 0) {
                int h = vsapi->getFrameHeight(src1, plane);
                int w = vsapi->getFrameWidth(src2, plane);
                ptrdiff_t stride = vsapi->getStride(src1, plane);
                const uint8_t *srcp1 = vsapi->getReadPtr(src1, plane);
                const uint8_t *srcp2 = vsapi->getReadPtr(src2, plane);
                uint8_t * VS_RESTRICT dstp = vsapi->getWritePtr(dst, plane);

                void (*func)(const void *, const void *, void *, union vs_merge_weight, unsigned) = 0;
                union vs_merge_weight weight;

#ifdef VS_TARGET_CPU_X86
                if (getCPUFeatures()->avx2 && d->cpulevel >= VS_CPU_LEVEL_AVX2) {
                    if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 1)
                        func = vs_merge_byte_avx2;
                    else if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 2)
                        func = vs_merge_word_avx2;
                    else if (d->vi->format.sampleType == stFloat && d->vi->format.bytesPerSample == 4)
                        func = vs_merge_float_avx2;
                }
                if (!func && d->cpulevel >= VS_CPU_LEVEL_SSE2) {
                    if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 1)
                        func = vs_merge_byte_sse2;
                    else if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 2)
                        func = vs_merge_word_sse2;
                    else if (d->vi->format.sampleType == stFloat && d->vi->format.bytesPerSample == 4)
                        func = vs_merge_float_sse2;
                }
#endif
                if (!func) {
                    if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 1)
                        func = vs_merge_byte_c;
                    else if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 2)
                        func = vs_merge_word_c;
                    else if (d->vi->format.sampleType == stFloat && d->vi->format.bytesPerSample == 4)
                        func = vs_merge_float_c;
                }

                if (!func)
                    continue;

                if (d->vi->format.sampleType == stInteger)
                    weight.u = d->weight[plane];
                else
                    weight.f = d->fweight[plane];

                for (int y = 0; y < h; ++y) {
                    func(srcp1, srcp2, dstp, weight, w);
                    srcp1 += stride;
                    srcp2 += stride;
                    dstp += stride;
                }
            }
        }

        vsapi->freeFrame(src1);
        vsapi->freeFrame(src2);
        return dst;
    }

    return nullptr;
}

static void VS_CC mergeCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<MergeData> d(new MergeData(vsapi));

    int nweight = vsapi->propNumElements(in, "weight");
    for (int i = 0; i < 3; i++)
        d->fweight[i] = 0.5f;
    for (int i = 0; i < nweight; i++)
        d->fweight[i] = (float)vsapi->propGetFloat(in, "weight", i, 0);

    if (nweight == 2) {
        d->fweight[2] = d->fweight[1];
    } else if (nweight == 1) {
        d->fweight[1] = d->fweight[0];
        d->fweight[2] = d->fweight[0];
    }

    for (int i = 0; i < 3; i++) {
        if (d->fweight[i] < 0 || d->fweight[i] > 1)
            RETERROR("Merge: weights must be between 0 and 1");
        d->weight[i] = std::min<unsigned>((d->fweight[i] * (1 << MergeShift) + 0.5f), (1U << MergeShift) - 1);
    }

    d->node1 = vsapi->propGetNode(in, "clipa", 0, 0);
    d->node2 = vsapi->propGetNode(in, "clipb", 0, 0);
    d->vi = vsapi->getVideoInfo(d->node1);

    for (int i = 0; i < 3; i++) {
        d->process[i] = 0;
        if (d->vi->format.sampleType == stInteger) {
            if (d->weight[i] == 0)
                d->process[i] = 1;
            else if (d->weight[i] == 1 << MergeShift)
                d->process[i] = 2;
        } else if (d->vi->format.sampleType == stFloat) {
            if (d->fweight[i] == 0.0f)
                d->process[i] = 1;
            else if (d->fweight[i] == 1.0f)
                d->process[i] = 2;
        }
    }

    d->cpulevel = vs_get_cpulevel(core);

    if (!isConstantVideoFormat(d->vi) || !isSameVideoInfo(d->vi, vsapi->getVideoInfo(d->node2)))
        RETERROR("Merge: both clips must have constant format and dimensions, and the same format and dimensions");

    if (!is8to16orFloatFormatCheck(d->vi->format))
        RETERROR("Merge: only 8-16 bit integer and 32 bit float input supported");

    if (nweight > d->vi->format.numPlanes)
        RETERROR("Merge: more weights given than the number of planes to merge");

    vsapi->createVideoFilter(out, "Merge", d->vi, 1, mergeGetFrame, filterFree<MergeData>, fmParallel, 0, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// MaskedMerge

typedef struct {
    const VSVideoInfo *vi;
    VSNodeRef *node1;
    VSNodeRef *node2;
    VSNodeRef *mask;
    VSNodeRef *mask23;
    bool premultiplied;
    bool first_plane;
    bool process[3];
    int cpulevel;
} MaskedMergeData;

static const VSFrameRef *VS_CC maskedMergeGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    MaskedMergeData *d = reinterpret_cast<MaskedMergeData *>(instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node1, frameCtx);
        vsapi->requestFrameFilter(n, d->node2, frameCtx);
        vsapi->requestFrameFilter(n, d->mask, frameCtx);
        if (d->mask23)
            vsapi->requestFrameFilter(n, d->mask23, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src1 = vsapi->getFrameFilter(n, d->node1, frameCtx);
        const VSFrameRef *src2 = vsapi->getFrameFilter(n, d->node2, frameCtx);
        const VSFrameRef *mask = vsapi->getFrameFilter(n, d->mask, frameCtx);
        const VSFrameRef *mask23 = 0;
        int offset1 = getLimitedRangeOffset(src1, d->vi, vsapi);
        int offset2 = getLimitedRangeOffset(src2, d->vi, vsapi);

        const int pl[] = {0, 1, 2};
        const VSFrameRef *fr[] = {d->process[0] ? 0 : src1, d->process[1] ? 0 : src1, d->process[2] ? 0 : src1};
        VSFrameRef *dst = vsapi->newVideoFrame2(&d->vi->format, d->vi->width, d->vi->height, fr, pl, src1, core);
        if (d->mask23)
           mask23 = vsapi->getFrameFilter(n, d->mask23, frameCtx);
        for (int plane = 0; plane < d->vi->format.numPlanes; plane++) {
            if (d->process[plane]) {
                int h = vsapi->getFrameHeight(src1, plane);
                int w = vsapi->getFrameWidth(src2, plane);
                ptrdiff_t stride = vsapi->getStride(src1, plane);
                const uint8_t *srcp1 = vsapi->getReadPtr(src1, plane);
                const uint8_t *srcp2 = vsapi->getReadPtr(src2, plane);
                const uint8_t *maskp = vsapi->getReadPtr((plane && mask23) ? mask23 : mask, d->first_plane ? 0 : plane);
                uint8_t * VS_RESTRICT dstp = vsapi->getWritePtr(dst, plane);

                void (*func)(const void *, const void *, const void *, void *, unsigned, unsigned, unsigned) = 0;
                int yuvhandling = (plane > 0) && (d->vi->format.colorFamily == cfYUV || d->vi->format.colorFamily == cfYCoCg);

                if (d->premultiplied && d->vi->format.sampleType == stInteger && offset1 != offset2) {
                    vsapi->freeFrame(src1);
                    vsapi->freeFrame(src2);
                    vsapi->freeFrame(mask);
                    vsapi->freeFrame(mask23);
                    vsapi->freeFrame(dst);
                    vsapi->setFilterError("MaskedMerge: Input frames must have the same range", frameCtx);
                    return nullptr;
                }

#ifdef VS_TARGET_CPU_X86
                if (getCPUFeatures()->avx2 && d->cpulevel >= VS_CPU_LEVEL_AVX2) {
                    if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 1)
                        func = d->premultiplied ? vs_mask_merge_premul_byte_avx2 : vs_mask_merge_byte_avx2;
                    else if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 2)
                        func = d->premultiplied ? vs_mask_merge_premul_word_avx2 : vs_mask_merge_word_avx2;
                    else if (d->vi->format.sampleType == stFloat && d->vi->format.bytesPerSample == 4)
                        func = d->premultiplied ? vs_mask_merge_premul_float_avx2 : vs_mask_merge_float_avx2;
                }
                if (!func && d->cpulevel >= VS_CPU_LEVEL_SSE2) {
                    if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 1)
                        func = d->premultiplied ? vs_mask_merge_premul_byte_sse2 : vs_mask_merge_byte_sse2;
                    else if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 2)
                        func = d->premultiplied ? vs_mask_merge_premul_word_sse2 : vs_mask_merge_word_sse2;
                    else if (d->vi->format.sampleType == stFloat && d->vi->format.bytesPerSample == 4)
                        func = d->premultiplied ? vs_mask_merge_premul_float_sse2 : vs_mask_merge_float_sse2;
                }
#endif
                if (!func) {
                    if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 1)
                        func = d->premultiplied ? (yuvhandling ? vs_mask_merge_premul_byte_c : vs_mask_merge_premul_byte_c) : vs_mask_merge_byte_c;
                    else if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 2)
                        func = d->premultiplied ? (yuvhandling ? vs_mask_merge_premul_word_c : vs_mask_merge_premul_word_c) : vs_mask_merge_word_c;
                    else if (d->vi->format.sampleType == stFloat && d->vi->format.bytesPerSample == 4)
                        func = d->premultiplied ? vs_mask_merge_premul_float_c : vs_mask_merge_float_c;
                }

                if (!func)
                    continue;

                int depth = d->vi->format.bitsPerSample;

                for (int y = 0; y < h; y++) {
                    func(srcp1, srcp2, maskp, dstp, depth, yuvhandling ? (1 << (depth - 1)) : offset1, w);
                    srcp1 += stride;
                    srcp2 += stride;
                    maskp += stride;
                    dstp += stride;
                }
            }
        }
        vsapi->freeFrame(src1);
        vsapi->freeFrame(src2);
        vsapi->freeFrame(mask);
        vsapi->freeFrame(mask23);
        return dst;
    }

    return nullptr;
}

static void VS_CC maskedMergeFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    MaskedMergeData *d = reinterpret_cast<MaskedMergeData *>(instanceData);
    vsapi->freeNode(d->node1);
    vsapi->freeNode(d->node2);
    vsapi->freeNode(d->mask);
    vsapi->freeNode(d->mask23);
    delete d;
}

static void VS_CC maskedMergeCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<MaskedMergeData> d(new MaskedMergeData());

    int err;
    d->mask23 = nullptr;
    d->node1 = vsapi->propGetNode(in, "clipa", 0, 0);
    d->node2 = vsapi->propGetNode(in, "clipb", 0, 0);
    d->mask = vsapi->propGetNode(in, "mask", 0, 0);
    d->vi = vsapi->getVideoInfo(d->node1);
    const VSVideoInfo *maskvi = vsapi->getVideoInfo(d->mask);
    d->first_plane = !!vsapi->propGetInt(in, "first_plane", 0, &err);
    d->premultiplied = !!vsapi->propGetInt(in, "premultiplied", 0, &err);
    // always use the first mask plane for all planes when it is the only one
    if (maskvi->format.numPlanes == 1)
        d->first_plane = 1;

    if (!isConstantVideoFormat(d->vi) || !isSameVideoInfo(d->vi, vsapi->getVideoInfo(d->node2))) {
        vsapi->freeNode(d->node1);
        vsapi->freeNode(d->node2);
        vsapi->freeNode(d->mask);
        RETERROR("MaskedMerge: both clips must have constant format and dimensions, and the same format and dimensions");
    }

    if (!is8to16orFloatFormatCheck(d->vi->format)) {
        vsapi->freeNode(d->node1);
        vsapi->freeNode(d->node2);
        vsapi->freeNode(d->mask);
        RETERROR("MaskedMerge: only 8-16 bit integer and 32 bit float input supported");
    }

    if (maskvi->width != d->vi->width || maskvi->height != d->vi->height || maskvi->format.bitsPerSample != d->vi->format.bitsPerSample
        || (!isSameVideoFormat(&maskvi->format, &d->vi->format) && maskvi->format.colorFamily != cfGray && !d->first_plane)) {
        vsapi->freeNode(d->node1);
        vsapi->freeNode(d->node2);
        vsapi->freeNode(d->mask);
        RETERROR("MaskedMerge: mask clip must have same dimensions as main clip and be the same format or equivalent grayscale version");
    }

    if (!getProcessPlanesArg(in, out, "MaskedMerge", d->process, vsapi))
        return;

    // do we need to resample the first mask plane and use it for all the planes?
    if ((d->first_plane && d->vi->format.numPlanes > 1) && (d->vi->format.subSamplingH > 0 || d->vi->format.subSamplingW > 0) && (d->process[1] || d->process[2])) {
        VSMap *min = vsapi->createMap();

        if (maskvi->format.numPlanes > 1) {
            // Don't resize the unused second and third planes.
            vsapi->propSetNode(min, "clips", d->mask, paAppend);
            vsapi->propSetInt(min, "planes", 0, paAppend);
            vsapi->propSetInt(min, "colorfamily", cfGray, paAppend);
            VSMap *mout = vsapi->invoke(vsapi->getPluginById("com.vapoursynth.std", core), "ShufflePlanes", min);
            VSNodeRef *mask_first_plane = vsapi->propGetNode(mout, "clip", 0, 0);
            vsapi->freeMap(mout);
            vsapi->clearMap(min);
            vsapi->propSetNode(min, "clip", mask_first_plane, paAppend);
            vsapi->freeNode(mask_first_plane);
        } else {
            vsapi->propSetNode(min, "clip", d->mask, paAppend);
        }

        vsapi->propSetInt(min, "width", d->vi->width >> d->vi->format.subSamplingW, paAppend);
        vsapi->propSetInt(min, "height", d->vi->height >> d->vi->format.subSamplingH, paAppend);
        VSMap *mout = vsapi->invoke(vsapi->getPluginById("com.vapoursynth.resize", core), "Bilinear", min);
        d->mask23 = vsapi->propGetNode(mout, "clip", 0, 0);
        vsapi->freeMap(mout);
        vsapi->freeMap(min);
    }

    d->cpulevel = vs_get_cpulevel(core);

    vsapi->createVideoFilter(out, "MaskedMerge", d->vi, 1, maskedMergeGetFrame, maskedMergeFree, fmParallel, 0, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// MakeDiff

typedef struct {
    VSNodeRef *node1;
    VSNodeRef *node2;
    const VSVideoInfo *vi;
    bool process[3];
    int cpulevel;
} MakeDiffDataExtra;

typedef DualNodeData<MakeDiffDataExtra> MakeDiffData;

static const VSFrameRef *VS_CC makeDiffGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    MakeDiffData *d = reinterpret_cast<MakeDiffData *>(instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node1, frameCtx);
        vsapi->requestFrameFilter(n, d->node2, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src1 = vsapi->getFrameFilter(n, d->node1, frameCtx);
        const VSFrameRef *src2 = vsapi->getFrameFilter(n, d->node2, frameCtx);
        const int pl[] = { 0, 1, 2 };
        const VSFrameRef *fr[] = { d->process[0] ? 0 : src1, d->process[1] ? 0 : src1, d->process[2] ? 0 : src1 };
        VSFrameRef *dst = vsapi->newVideoFrame2(&d->vi->format, d->vi->width, d->vi->height, fr, pl, src1, core);
        for (int plane = 0; plane < d->vi->format.numPlanes; plane++) {
            if (d->process[plane]) {
                int h = vsapi->getFrameHeight(src1, plane);
                int w = vsapi->getFrameWidth(src2, plane);
                ptrdiff_t stride = vsapi->getStride(src1, plane);
                const uint8_t *srcp1 = vsapi->getReadPtr(src1, plane);
                const uint8_t *srcp2 = vsapi->getReadPtr(src2, plane);
                uint8_t * VS_RESTRICT dstp = vsapi->getWritePtr(dst, plane);

                void (*func)(const void *, const void *, void *, unsigned, unsigned) = 0;

#ifdef VS_TARGET_CPU_X86
                if (getCPUFeatures()->avx2 && d->cpulevel >= VS_CPU_LEVEL_AVX2) {
                    if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 1)
                        func = vs_makediff_byte_avx2;
                    else if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 2)
                        func = vs_makediff_word_avx2;
                    else if (d->vi->format.sampleType == stFloat && d->vi->format.bytesPerSample == 4)
                        func = vs_makediff_float_avx2;
                }
                if (!func && d->cpulevel >= VS_CPU_LEVEL_SSE2) {
                    if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 1)
                        func = vs_makediff_byte_sse2;
                    else if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 2)
                        func = vs_makediff_word_sse2;
                    else if (d->vi->format.sampleType == stFloat && d->vi->format.bytesPerSample == 4)
                        func = vs_makediff_float_sse2;
                }
#endif
                if (!func) {
                    if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 1)
                        func = vs_makediff_byte_c;
                    else if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 2)
                        func = vs_makediff_word_c;
                    else if (d->vi->format.sampleType == stFloat && d->vi->format.bytesPerSample == 4)
                        func = vs_makediff_float_c;
                }

                if (!func)
                    continue;

                int depth = d->vi->format.bitsPerSample;

                for (int y = 0; y < h; ++y) {
                    func(srcp1, srcp2, dstp, depth, w);
                    srcp1 += stride;
                    srcp2 += stride;
                    dstp += stride;
                }
            }
        }

        vsapi->freeFrame(src1);
        vsapi->freeFrame(src2);
        return dst;
    }

    return nullptr;
}

static void VS_CC makeDiffCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<MakeDiffData> d(new MakeDiffData(vsapi));

    d->node1 = vsapi->propGetNode(in, "clipa", 0, 0);
    d->node2 = vsapi->propGetNode(in, "clipb", 0, 0);
    d->vi = vsapi->getVideoInfo(d->node1);

    if (isCompatFormat(&d->vi->format) || isCompatFormat(&vsapi->getVideoInfo(d->node2)->format))
        RETERROR("MakeDiff: compat formats are not supported");

    if (!isConstantVideoFormat(d->vi) || !isSameVideoInfo(d->vi, vsapi->getVideoInfo(d->node2))) 
        RETERROR("MakeDiff: both clips must have constant format and dimensions, and the same format and dimensions");

    if (!is8to16orFloatFormatCheck(d->vi->format))
        RETERROR("MakeDiff: only 8-16 bit integer and 32 bit float input supported");

    if (!getProcessPlanesArg(in, out, "MakeDiff", d->process, vsapi))
        return;

    d->cpulevel = vs_get_cpulevel(core);

    vsapi->createVideoFilter(out, "MakeDiff", d->vi, 1, makeDiffGetFrame, filterFree<MakeDiffData>, fmParallel, 0, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// MergeDiff

struct MergeDiffDataExtra {
    const VSVideoInfo *vi;
    bool process[3];
    int cpulevel;
};

typedef DualNodeData<MergeDiffDataExtra> MergeDiffData;

static const VSFrameRef *VS_CC mergeDiffGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    MergeDiffData *d = reinterpret_cast<MergeDiffData *>(instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node1, frameCtx);
        vsapi->requestFrameFilter(n, d->node2, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src1 = vsapi->getFrameFilter(n, d->node1, frameCtx);
        const VSFrameRef *src2 = vsapi->getFrameFilter(n, d->node2, frameCtx);
        const int pl[] = { 0, 1, 2 };
        const VSFrameRef *fr[] = { d->process[0] ? 0 : src1, d->process[1] ? 0 : src1, d->process[2] ? 0 : src1 };
        VSFrameRef *dst = vsapi->newVideoFrame2(&d->vi->format, d->vi->width, d->vi->height, fr, pl, src1, core);
        for (int plane = 0; plane < d->vi->format.numPlanes; plane++) {
            if (d->process[plane]) {
                int h = vsapi->getFrameHeight(src1, plane);
                int w = vsapi->getFrameWidth(src1, plane);
                ptrdiff_t stride = vsapi->getStride(src1, plane);
                const uint8_t *srcp1 = vsapi->getReadPtr(src1, plane);
                const uint8_t *srcp2 = vsapi->getReadPtr(src2, plane);
                uint8_t * VS_RESTRICT dstp = vsapi->getWritePtr(dst, plane);

                void (*func)(const void *, const void *, void *, unsigned, unsigned) = 0;

#ifdef VS_TARGET_CPU_X86
                if (getCPUFeatures()->avx2 && d->cpulevel >= VS_CPU_LEVEL_AVX2) {
                    if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 1)
                        func = vs_mergediff_byte_avx2;
                    else if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 2)
                        func = vs_mergediff_word_avx2;
                    else if (d->vi->format.sampleType == stFloat && d->vi->format.bytesPerSample == 4)
                        func = vs_mergediff_float_avx2;
                }
                if (!func && d->cpulevel >= VS_CPU_LEVEL_SSE2) {
                    if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 1)
                        func = vs_mergediff_byte_sse2;
                    else if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 2)
                        func = vs_mergediff_word_sse2;
                    else if (d->vi->format.sampleType == stFloat && d->vi->format.bytesPerSample == 4)
                        func = vs_mergediff_float_sse2;
                }
#endif
                if (!func) {
                    if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 1)
                        func = vs_mergediff_byte_c;
                    else if (d->vi->format.sampleType == stInteger && d->vi->format.bytesPerSample == 2)
                        func = vs_mergediff_word_c;
                    else if (d->vi->format.sampleType == stFloat && d->vi->format.bytesPerSample == 4)
                        func = vs_mergediff_float_c;
                }

                if (!func)
                    continue;

                int depth = d->vi->format.bitsPerSample;

                for (int y = 0; y < h; ++y) {
                    func(srcp1, srcp2, dstp, depth, w);
                    srcp1 += stride;
                    srcp2 += stride;
                    dstp += stride;
                }
            }
        }

        vsapi->freeFrame(src1);
        vsapi->freeFrame(src2);
        return dst;
    }

    return nullptr;
}

static void VS_CC mergeDiffCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<MergeDiffData> d(new MergeDiffData(vsapi));

    d->node1 = vsapi->propGetNode(in, "clipa", 0, 0);
    d->node2 = vsapi->propGetNode(in, "clipb", 0, 0);
    d->vi = vsapi->getVideoInfo(d->node1);

    if (!isConstantVideoFormat(d->vi) || !isSameVideoInfo(d->vi, vsapi->getVideoInfo(d->node2)))
        RETERROR("MergeDiff: both clips must have constant format and dimensions, and the same format and dimensions");

    if (!is8to16orFloatFormatCheck(d->vi->format))
        RETERROR("MergeDiff: only 8-16 bit integer and 32 bit float input supported");

    if (!getProcessPlanesArg(in, out, "MergeDiff", d->process, vsapi))
        return;

    d->cpulevel = vs_get_cpulevel(core);

    vsapi->createVideoFilter(out, "MergeDiff", d->vi, 1, mergeDiffGetFrame, filterFree<MergeDiffData>, fmParallel, 0, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// Init

void VS_CC mergeInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    //configFunc("com.vapoursynth.std", "std", "VapourSynth Core Functions", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("PreMultiply", "clip:clip;alpha:clip;", preMultiplyCreate, 0, plugin);
    registerFunc("Merge", "clipa:clip;clipb:clip;weight:float[]:opt;", mergeCreate, 0, plugin);
    registerFunc("MaskedMerge", "clipa:clip;clipb:clip;mask:clip;planes:int[]:opt;first_plane:int:opt;premultiplied:int:opt;", maskedMergeCreate, 0, plugin);
    registerFunc("MakeDiff", "clipa:clip;clipb:clip;planes:int[]:opt;", makeDiffCreate, 0, plugin);
    registerFunc("MergeDiff", "clipa:clip;clipb:clip;planes:int[]:opt;", mergeDiffCreate, 0, plugin);
}
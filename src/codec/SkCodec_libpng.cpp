/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkCodec_libpng.h"
#include "SkCodecPriv.h"
#include "SkColorPriv.h"
#include "SkColorTable.h"
#include "SkBitmap.h"
#include "SkMath.h"
#include "SkScanlineDecoder.h"
#include "SkSize.h"
#include "SkStream.h"
#include "SkSwizzler.h"

///////////////////////////////////////////////////////////////////////////////
// Helper macros
///////////////////////////////////////////////////////////////////////////////

#ifndef png_jmpbuf
#  define png_jmpbuf(png_ptr) ((png_ptr)->jmpbuf)
#endif

/* These were dropped in libpng >= 1.4 */
#ifndef png_infopp_NULL
    #define png_infopp_NULL NULL
#endif

#ifndef png_bytepp_NULL
    #define png_bytepp_NULL NULL
#endif

#ifndef int_p_NULL
    #define int_p_NULL NULL
#endif

#ifndef png_flush_ptr_NULL
    #define png_flush_ptr_NULL NULL
#endif

///////////////////////////////////////////////////////////////////////////////
// Callback functions
///////////////////////////////////////////////////////////////////////////////

static void sk_error_fn(png_structp png_ptr, png_const_charp msg) {
    SkDebugf("------ png error %s\n", msg);
    longjmp(png_jmpbuf(png_ptr), 1);
}

static void sk_read_fn(png_structp png_ptr, png_bytep data,
                       png_size_t length) {
    SkStream* stream = static_cast<SkStream*>(png_get_io_ptr(png_ptr));
    const size_t bytes = stream->read(data, length);
    if (bytes != length) {
        // FIXME: We want to report the fact that the stream was truncated.
        // One way to do that might be to pass a enum to longjmp so setjmp can
        // specify the failure.
        png_error(png_ptr, "Read Error!");
    }
}

///////////////////////////////////////////////////////////////////////////////
// Helpers
///////////////////////////////////////////////////////////////////////////////

class AutoCleanPng : public SkNoncopyable {
public:
    AutoCleanPng(png_structp png_ptr)
        : fPng_ptr(png_ptr)
        , fInfo_ptr(NULL) {}

    ~AutoCleanPng() {
        // fInfo_ptr will never be non-NULL unless fPng_ptr is.
        if (fPng_ptr) {
            png_infopp info_pp = fInfo_ptr ? &fInfo_ptr : NULL;
            png_destroy_read_struct(&fPng_ptr, info_pp, png_infopp_NULL);
        }
    }

    void setInfoPtr(png_infop info_ptr) {
        SkASSERT(NULL == fInfo_ptr);
        fInfo_ptr = info_ptr;
    }

    void detach() {
        fPng_ptr = NULL;
        fInfo_ptr = NULL;
    }

private:
    png_structp     fPng_ptr;
    png_infop       fInfo_ptr;
};
#define AutoCleanPng(...) SK_REQUIRE_LOCAL_VAR(AutoCleanPng)

// call only if color_type is PALETTE. Returns true if the ctable has alpha
static bool has_transparency_in_palette(png_structp png_ptr,
                                        png_infop info_ptr) {
    if (!png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        return false;
    }

    png_bytep trans;
    int num_trans;
    png_get_tRNS(png_ptr, info_ptr, &trans, &num_trans, NULL);
    return num_trans > 0;
}

// Method for coverting to either an SkPMColor or a similarly packed
// unpremultiplied color.
typedef uint32_t (*PackColorProc)(U8CPU a, U8CPU r, U8CPU g, U8CPU b);

// Note: SkColorTable claims to store SkPMColors, which is not necessarily
// the case here.
bool SkPngCodec::decodePalette(bool premultiply) {
    int numPalette;
    png_colorp palette;
    png_bytep trans;

    if (!png_get_PLTE(fPng_ptr, fInfo_ptr, &palette, &numPalette)) {
        return false;
    }

    /*  BUGGY IMAGE WORKAROUND

        We hit some images (e.g. fruit_.png) who contain bytes that are == colortable_count
        which is a problem since we use the byte as an index. To work around this we grow
        the colortable by 1 (if its < 256) and duplicate the last color into that slot.
    */
    const int colorCount = numPalette + (numPalette < 256);
    // Note: These are not necessarily SkPMColors.
    SkPMColor colorStorage[256];    // worst-case storage
    SkPMColor* colorPtr = colorStorage;

    int numTrans;
    if (png_get_valid(fPng_ptr, fInfo_ptr, PNG_INFO_tRNS)) {
        png_get_tRNS(fPng_ptr, fInfo_ptr, &trans, &numTrans, NULL);
    } else {
        numTrans = 0;
    }

    // check for bad images that might make us crash
    if (numTrans > numPalette) {
        numTrans = numPalette;
    }

    int index = 0;
    int transLessThanFF = 0;

    // Choose which function to use to create the color table. If the final destination's
    // colortype is unpremultiplied, the color table will store unpremultiplied colors.
    PackColorProc proc;
    if (premultiply) {
        proc = &SkPreMultiplyARGB;
    } else {
        proc = &SkPackARGB32NoCheck;
    }
    for (; index < numTrans; index++) {
        transLessThanFF |= (int)*trans - 0xFF;
        *colorPtr++ = proc(*trans++, palette->red, palette->green, palette->blue);
        palette++;
    }

    fReallyHasAlpha = transLessThanFF < 0;

    for (; index < numPalette; index++) {
        *colorPtr++ = SkPackARGB32(0xFF, palette->red, palette->green, palette->blue);
        palette++;
    }

    // see BUGGY IMAGE WORKAROUND comment above
    if (numPalette < 256) {
        *colorPtr = colorPtr[-1];
    }

    fColorTable.reset(SkNEW_ARGS(SkColorTable, (colorStorage, colorCount)));
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Creation
///////////////////////////////////////////////////////////////////////////////

#define PNG_BYTES_TO_CHECK 4

bool SkPngCodec::IsPng(SkStream* stream) {
    char buf[PNG_BYTES_TO_CHECK];
    if (stream->read(buf, PNG_BYTES_TO_CHECK) != PNG_BYTES_TO_CHECK) {
        return false;
    }
    if (png_sig_cmp((png_bytep) buf, (png_size_t)0, PNG_BYTES_TO_CHECK)) {
        return false;
    }
    return true;
}

SkCodec* SkPngCodec::NewFromStream(SkStream* stream) {
    // The image is known to be a PNG. Decode enough to know the SkImageInfo.
    // FIXME: Allow silencing warnings.
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
                                                 sk_error_fn, NULL);
    if (!png_ptr) {
        return NULL;
    }

    AutoCleanPng autoClean(png_ptr);

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL) {
        return NULL;
    }

    autoClean.setInfoPtr(info_ptr);

    // FIXME: Could we use the return value of setjmp to specify the type of
    // error?
    if (setjmp(png_jmpbuf(png_ptr))) {
        return NULL;
    }

    png_set_read_fn(png_ptr, static_cast<void*>(stream), sk_read_fn);

    // FIXME: This is where the old code hooks up the Peeker. Does it need to
    // be set this early? (i.e. where are the user chunks? early in the stream,
    // potentially?)
    // If it does, we need to figure out a way to set it here.

    // The call to png_read_info() gives us all of the information from the
    // PNG file before the first IDAT (image data chunk).
    png_read_info(png_ptr, info_ptr);
    png_uint_32 origWidth, origHeight;
    int bitDepth, colorType;
    png_get_IHDR(png_ptr, info_ptr, &origWidth, &origHeight, &bitDepth,
                 &colorType, int_p_NULL, int_p_NULL, int_p_NULL);

    // sanity check for size
    {
        int64_t size = sk_64_mul(origWidth, origHeight);
        // now check that if we are 4-bytes per pixel, we also don't overflow
        if (size < 0 || size > (0x7FFFFFFF >> 2)) {
            return NULL;
        }
    }

    // Tell libpng to strip 16 bit/color files down to 8 bits/color
    if (bitDepth == 16) {
        png_set_strip_16(png_ptr);
    }
#ifdef PNG_READ_PACK_SUPPORTED
    // Extract multiple pixels with bit depths of 1, 2, and 4 from a single
    // byte into separate bytes (useful for paletted and grayscale images).
    if (bitDepth < 8) {
        png_set_packing(png_ptr);
    }
#endif
    // Expand grayscale images to the full 8 bits from 1, 2, or 4 bits/pixel.
    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }


    // Now determine the default SkColorType and SkAlphaType.
    SkColorType skColorType;
    SkAlphaType skAlphaType;
    switch (colorType) {
        case PNG_COLOR_TYPE_PALETTE:
            // Technically, this is true of the data, but I don't think we want
            // to support it.
            // skColorType = kIndex8_SkColorType;
            skColorType = kN32_SkColorType;
            skAlphaType = has_transparency_in_palette(png_ptr, info_ptr) ?
                    kUnpremul_SkAlphaType : kOpaque_SkAlphaType;
            break;
        case PNG_COLOR_TYPE_GRAY:
            if (false) {
                // FIXME: Is this the wrong default behavior? This means if the
                // caller supplies the info we gave them, they'll get Alpha 8.
                skColorType = kAlpha_8_SkColorType;
                // FIXME: Strangely, the canonical type for Alpha 8 is Premul.
                skAlphaType = kPremul_SkAlphaType;
            } else {
                skColorType = kN32_SkColorType;
                skAlphaType = kOpaque_SkAlphaType;
            }
            break;
        default:
            // Note: This *almost* mimics the code in SkImageDecoder_libpng.
            // has_transparency_in_palette makes an additional check - whether
            // numTrans is greater than 0. Why does the other code not make that
            // check?
            if (has_transparency_in_palette(png_ptr, info_ptr)
                || PNG_COLOR_TYPE_RGB_ALPHA == colorType
                || PNG_COLOR_TYPE_GRAY_ALPHA == colorType)
            {
                skAlphaType = kUnpremul_SkAlphaType;
            } else {
                skAlphaType = kOpaque_SkAlphaType;
            }
            skColorType = kN32_SkColorType;
            break;
    }

    {
        // FIXME: Again, this block needs to go into onGetPixels.
        bool convertGrayToRGB = PNG_COLOR_TYPE_GRAY == colorType && skColorType != kAlpha_8_SkColorType;

        // Unless the user is requesting A8, convert a grayscale image into RGB.
        // GRAY_ALPHA will always be converted to RGB
        if (convertGrayToRGB || colorType == PNG_COLOR_TYPE_GRAY_ALPHA) {
            png_set_gray_to_rgb(png_ptr);
        }

        // Add filler (or alpha) byte (after each RGB triplet) if necessary.
        // FIXME: It seems like we could just use RGB as the SrcConfig here.
        if (colorType == PNG_COLOR_TYPE_RGB || convertGrayToRGB) {
            png_set_filler(png_ptr, 0xff, PNG_FILLER_AFTER);
        }
    }

    // FIXME: Also need to check for sRGB (skbug.com/3471).

    SkImageInfo info = SkImageInfo::Make(origWidth, origHeight, skColorType,
                                         skAlphaType);
    SkCodec* codec = SkNEW_ARGS(SkPngCodec, (info, stream, png_ptr, info_ptr));
    autoClean.detach();
    return codec;
}

#define INVALID_NUMBER_PASSES -1
SkPngCodec::SkPngCodec(const SkImageInfo& info, SkStream* stream,
                       png_structp png_ptr, png_infop info_ptr)
    : INHERITED(info, stream)
    , fPng_ptr(png_ptr)
    , fInfo_ptr(info_ptr)
    , fSrcConfig(SkSwizzler::kUnknown)
    , fNumberPasses(INVALID_NUMBER_PASSES)
    , fReallyHasAlpha(false)
{}

SkPngCodec::~SkPngCodec() {
    png_destroy_read_struct(&fPng_ptr, &fInfo_ptr, png_infopp_NULL);
}

///////////////////////////////////////////////////////////////////////////////
// Getting the pixels
///////////////////////////////////////////////////////////////////////////////

static bool conversion_possible(const SkImageInfo& dst, const SkImageInfo& src) {
    // TODO: Support other conversions
    if (dst.colorType() != src.colorType()) {
        return false;
    }
    if (dst.profileType() != src.profileType()) {
        return false;
    }
    if (dst.alphaType() == src.alphaType()) {
        return true;
    }
    return kPremul_SkAlphaType == dst.alphaType() &&
            kUnpremul_SkAlphaType == src.alphaType();
}

SkCodec::Result SkPngCodec::initializeSwizzler(const SkImageInfo& requestedInfo,
                                               void* dst, size_t rowBytes,
                                               const Options& options) {
    // FIXME: Could we use the return value of setjmp to specify the type of
    // error?
    if (setjmp(png_jmpbuf(fPng_ptr))) {
        SkDebugf("setjmp long jump!\n");
        return kInvalidInput;
    }

    // FIXME: We already retrieved this information. Store it in SkPngCodec?
    png_uint_32 origWidth, origHeight;
    int bitDepth, pngColorType, interlaceType;
    png_get_IHDR(fPng_ptr, fInfo_ptr, &origWidth, &origHeight, &bitDepth,
                 &pngColorType, &interlaceType, int_p_NULL, int_p_NULL);

    fNumberPasses = (interlaceType != PNG_INTERLACE_NONE) ?
            png_set_interlace_handling(fPng_ptr) : 1;

    // Set to the default before calling decodePalette, which may change it.
    fReallyHasAlpha = false;
    if (PNG_COLOR_TYPE_PALETTE == pngColorType) {
        fSrcConfig = SkSwizzler::kIndex;
        if (!this->decodePalette(kPremul_SkAlphaType == requestedInfo.alphaType())) {
            return kInvalidInput;
        }
    } else if (kAlpha_8_SkColorType == requestedInfo.colorType()) {
        // Note: we check the destination, since otherwise we would have
        // told png to upscale.
        SkASSERT(PNG_COLOR_TYPE_GRAY == pngColorType);
        fSrcConfig = SkSwizzler::kGray;
    } else if (this->getInfo().alphaType() == kOpaque_SkAlphaType) {
        fSrcConfig = SkSwizzler::kRGBX;
    } else {
        fSrcConfig = SkSwizzler::kRGBA;
    }
    const SkPMColor* colors = fColorTable ? fColorTable->readColors() : NULL;
    fSwizzler.reset(SkSwizzler::CreateSwizzler(fSrcConfig, colors, requestedInfo,
            dst, rowBytes, options.fZeroInitialized));
    if (!fSwizzler) {
        // FIXME: CreateSwizzler could fail for another reason.
        return kUnimplemented;
    }

    // FIXME: Here is where we should likely insert some of the modifications
    // made in the factory.
    png_read_update_info(fPng_ptr, fInfo_ptr);

    return kSuccess;
}

SkCodec::Result SkPngCodec::onGetPixels(const SkImageInfo& requestedInfo, void* dst,
                                        size_t rowBytes, const Options& options,
                                        SkPMColor ctable[], int* ctableCount) {
    if (!this->rewindIfNeeded()) {
        return kCouldNotRewind;
    }
    if (requestedInfo.dimensions() != this->getInfo().dimensions()) {
        return kInvalidScale;
    }
    if (!conversion_possible(requestedInfo, this->getInfo())) {
        return kInvalidConversion;
    }

    const Result result = this->initializeSwizzler(requestedInfo, dst, rowBytes,
                                                   options);
    if (result != kSuccess) {
        return result;
    }

    // FIXME: Could we use the return value of setjmp to specify the type of
    // error?
    if (setjmp(png_jmpbuf(fPng_ptr))) {
        SkDebugf("setjmp long jump!\n");
        return kInvalidInput;
    }

    SkASSERT(fNumberPasses != INVALID_NUMBER_PASSES);
    SkAutoMalloc storage;
    if (fNumberPasses > 1) {
        const int width = requestedInfo.width();
        const int height = requestedInfo.height();
        const int bpp = SkSwizzler::BytesPerPixel(fSrcConfig);
        const size_t rowBytes = width * bpp;

        storage.reset(width * height * bpp);
        uint8_t* const base = static_cast<uint8_t*>(storage.get());

        for (int i = 0; i < fNumberPasses; i++) {
            uint8_t* row = base;
            for (int y = 0; y < height; y++) {
                uint8_t* bmRow = row;
                png_read_rows(fPng_ptr, &bmRow, png_bytepp_NULL, 1);
                row += rowBytes;
            }
        }

        // Now swizzle it.
        uint8_t* row = base;
        for (int y = 0; y < height; y++) {
            fReallyHasAlpha |= !SkSwizzler::IsOpaque(fSwizzler->next(row));
            row += rowBytes;
        }
    } else {
        storage.reset(requestedInfo.width() * SkSwizzler::BytesPerPixel(fSrcConfig));
        uint8_t* srcRow = static_cast<uint8_t*>(storage.get());
        for (int y = 0; y < requestedInfo.height(); y++) {
            png_read_rows(fPng_ptr, &srcRow, png_bytepp_NULL, 1);
            fReallyHasAlpha |= !SkSwizzler::IsOpaque(fSwizzler->next(srcRow));
        }
    }

    // FIXME: do we need substituteTranspColor? Note that we cannot do it for
    // scanline decoding, but we could do it here. Alternatively, we could do
    // it as we go, instead of in post-processing like SkPNGImageDecoder.

    this->finish();
    return kSuccess;
}

void SkPngCodec::finish() {
    if (setjmp(png_jmpbuf(fPng_ptr))) {
        // We've already read all the scanlines. This is a success.
        return;
    }
    /* read rest of file, and get additional chunks in info_ptr - REQUIRED */
    png_read_end(fPng_ptr, fInfo_ptr);
}

class SkPngScanlineDecoder : public SkScanlineDecoder {
public:
    SkPngScanlineDecoder(const SkImageInfo& dstInfo, SkPngCodec* codec)
        : INHERITED(dstInfo)
        , fCodec(codec)
        , fHasAlpha(false)
    {
        fStorage.reset(dstInfo.width() * SkSwizzler::BytesPerPixel(fCodec->fSrcConfig));
        fSrcRow = static_cast<uint8_t*>(fStorage.get());
    }

    SkImageGenerator::Result onGetScanlines(void* dst, int count, size_t rowBytes) override {
        if (setjmp(png_jmpbuf(fCodec->fPng_ptr))) {
            SkDebugf("setjmp long jump!\n");
            return SkImageGenerator::kInvalidInput;
        }

        for (int i = 0; i < count; i++) {
            png_read_rows(fCodec->fPng_ptr, &fSrcRow, png_bytepp_NULL, 1);
            fCodec->fSwizzler->setDstRow(dst);
            fHasAlpha |= !SkSwizzler::IsOpaque(fCodec->fSwizzler->next(fSrcRow));
            dst = SkTAddOffset<void>(dst, rowBytes);
        }
        return SkImageGenerator::kSuccess;
    }

    SkImageGenerator::Result onSkipScanlines(int count) override {
        // FIXME: Could we use the return value of setjmp to specify the type of
        // error?
        if (setjmp(png_jmpbuf(fCodec->fPng_ptr))) {
            SkDebugf("setjmp long jump!\n");
            return SkImageGenerator::kInvalidInput;
        }

        png_read_rows(fCodec->fPng_ptr, png_bytepp_NULL, png_bytepp_NULL, count);
        return SkImageGenerator::kSuccess;
    }

    void onFinish() override {
        fCodec->finish();
    }

    bool onReallyHasAlpha() const override { return fHasAlpha; }

private:
    SkPngCodec*         fCodec;     // Unowned.
    bool                fHasAlpha;
    SkAutoMalloc        fStorage;
    uint8_t*            fSrcRow;

    typedef SkScanlineDecoder INHERITED;
};

SkScanlineDecoder* SkPngCodec::onGetScanlineDecoder(const SkImageInfo& dstInfo) {
    // Check to see if scaling was requested.
    if (dstInfo.dimensions() != this->getInfo().dimensions()) {
        return NULL;
    }

    if (!conversion_possible(dstInfo, this->getInfo())) {
        SkDebugf("no conversion possible\n");
        return NULL;
    }

    // Note: We set dst to NULL since we do not know it yet. rowBytes is not needed,
    // since we'll be manually updating the dstRow, but the SkSwizzler requires it to
    // be at least dstInfo.minRowBytes.
    Options opts;
    // FIXME: Pass this in to getScanlineDecoder?
    opts.fZeroInitialized = kNo_ZeroInitialized;
    if (this->initializeSwizzler(dstInfo, NULL, dstInfo.minRowBytes(), opts) != kSuccess) {
        SkDebugf("failed to initialize the swizzler.\n");
        return NULL;
    }

    SkASSERT(fNumberPasses != INVALID_NUMBER_PASSES);
    if (fNumberPasses > 1) {
        // We cannot efficiently do scanline decoding.
        return NULL;
    }

    return SkNEW_ARGS(SkPngScanlineDecoder, (dstInfo, this));
}


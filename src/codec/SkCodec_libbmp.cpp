/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkCodec_libbmp.h"
#include "SkCodecPriv.h"
#include "SkColorPriv.h"
#include "SkStream.h"

/*
 *
 * Checks if the conversion between the input image and the requested output
 * image has been implemented
 *
 */
static bool conversion_possible(const SkImageInfo& dst,
                                const SkImageInfo& src) {
    // Ensure that the profile type is unchanged
    if (dst.profileType() != src.profileType()) {
        return false;
    }

    // Check for supported color and alpha types
    switch (dst.colorType()) {
        case kN32_SkColorType:
            return src.alphaType() == dst.alphaType() ||
                    (kPremul_SkAlphaType == dst.alphaType() &&
                    kUnpremul_SkAlphaType == src.alphaType());
        default:
            return false;
    }
}

/*
 *
 * Defines the version and type of the second bitmap header
 *
 */
enum BitmapHeaderType {
    kInfoV1_BitmapHeaderType,
    kInfoV2_BitmapHeaderType,
    kInfoV3_BitmapHeaderType,
    kInfoV4_BitmapHeaderType,
    kInfoV5_BitmapHeaderType,
    kOS2V1_BitmapHeaderType,
    kOS2VX_BitmapHeaderType,
    kUnknown_BitmapHeaderType
};

/*
 *
 * Possible bitmap compression types
 *
 */
enum BitmapCompressionMethod {
    kNone_BitmapCompressionMethod =          0,
    k8BitRLE_BitmapCompressionMethod =       1,
    k4BitRLE_BitmapCompressionMethod =       2,
    kBitMasks_BitmapCompressionMethod =      3,
    kJpeg_BitmapCompressionMethod =          4,
    kPng_BitmapCompressionMethod =           5,
    kAlphaBitMasks_BitmapCompressionMethod = 6,
    kCMYK_BitmapCompressionMethod =          11,
    kCMYK8BitRLE_BitmapCompressionMethod =   12,
    kCMYK4BitRLE_BitmapCompressionMethod =   13
};

/*
 *
 * Checks the start of the stream to see if the image is a bitmap
 *
 */
bool SkBmpCodec::IsBmp(SkStream* stream) {
    // TODO: Support "IC", "PT", "CI", "CP", "BA"
    // TODO: ICO files may contain a BMP and need to use this decoder
    const char bmpSig[] = { 'B', 'M' };
    char buffer[sizeof(bmpSig)];
    return stream->read(buffer, sizeof(bmpSig)) == sizeof(bmpSig) &&
            !memcmp(buffer, bmpSig, sizeof(bmpSig));
}

/*
 *
 * Assumes IsBmp was called and returned true
 * Creates a bmp decoder
 * Reads enough of the stream to determine the image format
 *
 */
SkCodec* SkBmpCodec::NewFromStream(SkStream* stream) {
    return SkBmpCodec::NewFromStream(stream, false);
}

/*
 *
 * Creates a bmp decoder for a bmp embedded in ico
 * Reads enough of the stream to determine the image format
 *
 */
SkCodec* SkBmpCodec::NewFromIco(SkStream* stream) {
    return SkBmpCodec::NewFromStream(stream, true);
}

/*
 *
 * Creates a bmp decoder
 * Reads enough of the stream to determine the image format
 *
 */
SkCodec* SkBmpCodec::NewFromStream(SkStream* stream, bool isIco) {
    // Header size constants
    static const uint32_t kBmpHeaderBytes = 14;
    static const uint32_t kBmpHeaderBytesPlusFour = kBmpHeaderBytes + 4;
    static const uint32_t kBmpOS2V1Bytes = 12;
    static const uint32_t kBmpOS2V2Bytes = 64;
    static const uint32_t kBmpInfoBaseBytes = 16;
    static const uint32_t kBmpInfoV1Bytes = 40;
    static const uint32_t kBmpInfoV2Bytes = 52;
    static const uint32_t kBmpInfoV3Bytes = 56;
    static const uint32_t kBmpInfoV4Bytes = 108;
    static const uint32_t kBmpInfoV5Bytes = 124;
    static const uint32_t kBmpMaskBytes = 12;

    // The total bytes in the bmp file
    // We only need to use this value for RLE decoding, so we will only
    // check that it is valid in the RLE case.
    uint32_t totalBytes;
    // The offset from the start of the file where the pixel data begins
    uint32_t offset;
    // The size of the second (info) header in bytes
    uint32_t infoBytes;

    // Bmps embedded in Icos skip the first Bmp header
    if (!isIco) {
        // Read the first header and the size of the second header
        SkAutoTDeleteArray<uint8_t> hBuffer(
                SkNEW_ARRAY(uint8_t, kBmpHeaderBytesPlusFour));
        if (stream->read(hBuffer.get(), kBmpHeaderBytesPlusFour) !=
                kBmpHeaderBytesPlusFour) {
            SkDebugf("Error: unable to read first bitmap header.\n");
            return NULL;
        }

        totalBytes = get_int(hBuffer.get(), 2);
        offset = get_int(hBuffer.get(), 10);
        if (offset < kBmpHeaderBytes + kBmpOS2V1Bytes) {
            SkDebugf("Error: invalid starting location for pixel data\n");
            return NULL;
        }

        // The size of the second (info) header in bytes
        // The size is the first field of the second header, so we have already
        // read the first four infoBytes.
        infoBytes = get_int(hBuffer.get(), 14);
        if (infoBytes < kBmpOS2V1Bytes) {
            SkDebugf("Error: invalid second header size.\n");
            return NULL;
        }
    } else {
        // This value is only used by RLE compression.  Bmp in Ico files do not
        // use RLE.  If the compression field is incorrectly signaled as RLE,
        // we will catch this and signal an error below.
        totalBytes = 0;

        // Bmps in Ico cannot specify an offset.  We will always assume that
        // pixel data begins immediately after the color table.  This value
        // will be corrected below.
        offset = 0;

        // Read the size of the second header
        SkAutoTDeleteArray<uint8_t> hBuffer(
                SkNEW_ARRAY(uint8_t, 4));
        if (stream->read(hBuffer.get(), 4) != 4) {
            SkDebugf("Error: unable to read size of second bitmap header.\n");
            return NULL;
        }
        infoBytes = get_int(hBuffer.get(), 0);
        if (infoBytes < kBmpOS2V1Bytes) {
            SkDebugf("Error: invalid second header size.\n");
            return NULL;
        }
    }

    // We already read the first four bytes of the info header to get the size
    const uint32_t infoBytesRemaining = infoBytes - 4;

    // Read the second header
    SkAutoTDeleteArray<uint8_t> iBuffer(
            SkNEW_ARRAY(uint8_t, infoBytesRemaining));
    if (stream->read(iBuffer.get(), infoBytesRemaining) != infoBytesRemaining) {
        SkDebugf("Error: unable to read second bitmap header.\n");
        return NULL;
    }

    // The number of bits used per pixel in the pixel data
    uint16_t bitsPerPixel;

    // The compression method for the pixel data
    uint32_t compression = kNone_BitmapCompressionMethod;

    // Number of colors in the color table, defaults to 0 or max (see below)
    uint32_t numColors = 0;

    // Bytes per color in the color table, early versions use 3, most use 4
    uint32_t bytesPerColor;

    // The image width and height
    int width, height;

    // Determine image information depending on second header format
    BitmapHeaderType headerType;
    if (infoBytes >= kBmpInfoBaseBytes) {
        // Check the version of the header
        switch (infoBytes) {
            case kBmpInfoV1Bytes:
                headerType = kInfoV1_BitmapHeaderType;
                break;
            case kBmpInfoV2Bytes:
                headerType = kInfoV2_BitmapHeaderType;
                break;
            case kBmpInfoV3Bytes:
                headerType = kInfoV3_BitmapHeaderType;
                break;
            case kBmpInfoV4Bytes:
                headerType = kInfoV4_BitmapHeaderType;
                break;
            case kBmpInfoV5Bytes:
                headerType = kInfoV5_BitmapHeaderType;
                break;
            case 16:
            case 20:
            case 24:
            case 28:
            case 32:
            case 36:
            case 42:
            case 46:
            case 48:
            case 60:
            case kBmpOS2V2Bytes:
                headerType = kOS2VX_BitmapHeaderType;
                break;
            default:
                // We do not signal an error here because there is the
                // possibility of new or undocumented bmp header types.  Most
                // of the newer versions of bmp headers are similar to and
                // build off of the older versions, so we may still be able to
                // decode the bmp.
                SkDebugf("Warning: unknown bmp header format.\n");
                headerType = kUnknown_BitmapHeaderType;
                break;
        }
        // We check the size of the header before entering the if statement.
        // We should not reach this point unless the size is large enough for
        // these required fields.
        SkASSERT(infoBytesRemaining >= 12);
        width = get_int(iBuffer.get(), 0);
        height = get_int(iBuffer.get(), 4);
        bitsPerPixel = get_short(iBuffer.get(), 10);

        // Some versions do not have these fields, so we check before
        // overwriting the default value.
        if (infoBytesRemaining >= 16) {
            compression = get_int(iBuffer.get(), 12);
            if (infoBytesRemaining >= 32) {
                numColors = get_int(iBuffer.get(), 28);
            }
        }

        // All of the headers that reach this point, store color table entries
        // using 4 bytes per pixel.
        bytesPerColor = 4;
    } else if (infoBytes >= kBmpOS2V1Bytes) {
        // The OS2V1 is treated separately because it has a unique format
        headerType = kOS2V1_BitmapHeaderType;
        width = (int) get_short(iBuffer.get(), 0);
        height = (int) get_short(iBuffer.get(), 2);
        bitsPerPixel = get_short(iBuffer.get(), 6);
        bytesPerColor = 3;
    } else {
        // There are no valid bmp headers
        SkDebugf("Error: second bitmap header size is invalid.\n");
        return NULL;
    }

    // Check for valid dimensions from header
    RowOrder rowOrder = kBottomUp_RowOrder;
    if (height < 0) {
        height = -height;
        rowOrder = kTopDown_RowOrder;
    }
    // The height field for bmp in ico is double the actual height because they
    // contain an XOR mask followed by an AND mask
    if (isIco) {
        height /= 2;
    }
    static const int kBmpMaxDim = 1 << 16;
    if (width < 0 || width >= kBmpMaxDim || height >= kBmpMaxDim) {
        // TODO: Decide if we want to support really large bmps.
        SkDebugf("Error: invalid bitmap dimensions.\n");
        return NULL;
    }

    // Create mask struct
    SkMasks::InputMasks inputMasks;
    memset(&inputMasks, 0, sizeof(SkMasks::InputMasks));

    // Determine the input compression format and set bit masks if necessary
    uint32_t maskBytes = 0;
    BitmapInputFormat inputFormat = kUnknown_BitmapInputFormat;
    switch (compression) {
        case kNone_BitmapCompressionMethod:
            inputFormat = kStandard_BitmapInputFormat;
            break;
        case k8BitRLE_BitmapCompressionMethod:
            if (bitsPerPixel != 8) {
                SkDebugf("Warning: correcting invalid bitmap format.\n");
                bitsPerPixel = 8;
            }
            inputFormat = kRLE_BitmapInputFormat;
            break;
        case k4BitRLE_BitmapCompressionMethod:
            if (bitsPerPixel != 4) {
                SkDebugf("Warning: correcting invalid bitmap format.\n");
                bitsPerPixel = 4;
            }
            inputFormat = kRLE_BitmapInputFormat;
            break;
        case kAlphaBitMasks_BitmapCompressionMethod:
        case kBitMasks_BitmapCompressionMethod:
            // Load the masks
            inputFormat = kBitMask_BitmapInputFormat;
            switch (headerType) {
                case kInfoV1_BitmapHeaderType: {
                    // The V1 header stores the bit masks after the header
                    SkAutoTDeleteArray<uint8_t> mBuffer(
                            SkNEW_ARRAY(uint8_t, kBmpMaskBytes));
                    if (stream->read(mBuffer.get(), kBmpMaskBytes) !=
                            kBmpMaskBytes) {
                        SkDebugf("Error: unable to read bit inputMasks.\n");
                        return NULL;
                    }
                    maskBytes = kBmpMaskBytes;
                    inputMasks.red = get_int(mBuffer.get(), 0);
                    inputMasks.green = get_int(mBuffer.get(), 4);
                    inputMasks.blue = get_int(mBuffer.get(), 8);
                    break;
                }
                case kInfoV2_BitmapHeaderType:
                case kInfoV3_BitmapHeaderType:
                case kInfoV4_BitmapHeaderType:
                case kInfoV5_BitmapHeaderType:
                    // Header types are matched based on size.  If the header
                    // is V2+, we are guaranteed to be able to read at least
                    // this size.
                    SkASSERT(infoBytesRemaining >= 48);
                    inputMasks.red = get_int(iBuffer.get(), 36);
                    inputMasks.green = get_int(iBuffer.get(), 40);
                    inputMasks.blue = get_int(iBuffer.get(), 44);
                    break;
                case kOS2VX_BitmapHeaderType:
                    // TODO: Decide if we intend to support this.
                    //       It is unsupported in the previous version and
                    //       in chromium.  I have not come across a test case
                    //       that uses this format.
                    SkDebugf("Error: huffman format unsupported.\n");
                    return NULL;
                default:
                   SkDebugf("Error: invalid bmp bit masks header.\n");
                   return NULL;
            }
            break;
        case kJpeg_BitmapCompressionMethod:
            if (24 == bitsPerPixel) {
                inputFormat = kRLE_BitmapInputFormat;
                break;
            }
            // Fall through
        case kPng_BitmapCompressionMethod:
            // TODO: Decide if we intend to support this.
            //       It is unsupported in the previous version and
            //       in chromium.  I think it is used mostly for printers.
            SkDebugf("Error: compression format not supported.\n");
            return NULL;
        case kCMYK_BitmapCompressionMethod:
        case kCMYK8BitRLE_BitmapCompressionMethod:
        case kCMYK4BitRLE_BitmapCompressionMethod:
            // TODO: Same as above.
            SkDebugf("Error: CMYK not supported for bitmap decoding.\n");
            return NULL;
        default:
            SkDebugf("Error: invalid format for bitmap decoding.\n");
            return NULL;
    }

    // Most versions of bmps should be rendered as opaque.  Either they do
    // not have an alpha channel, or they expect the alpha channel to be
    // ignored.  V3+ bmp files introduce an alpha mask and allow the creator
    // of the image to use the alpha channels.  However, many of these images
    // leave the alpha channel blank and expect to be rendered as opaque.  This
    // is the case for almost all V3 images, so we render these as opaque.  For
    // V4+, we will use the alpha channel, and fix the image later if it turns
    // out to be fully transparent.
    // As an exception, V3 bmp-in-ico may use an alpha mask.
    SkAlphaType alphaType = kOpaque_SkAlphaType;
    if ((kInfoV3_BitmapHeaderType == headerType && isIco) ||
            kInfoV4_BitmapHeaderType == headerType ||
            kInfoV5_BitmapHeaderType == headerType) {
        // Header types are matched based on size.  If the header is
        // V3+, we are guaranteed to be able to read at least this size.
        SkASSERT(infoBytesRemaining > 52);
        inputMasks.alpha = get_int(iBuffer.get(), 48);
        if (inputMasks.alpha != 0) {
            alphaType = kUnpremul_SkAlphaType;
        }
    }
    iBuffer.free();

    // Additionally, 32 bit bmp-in-icos use the alpha channel
    if (isIco && 32 == bitsPerPixel) {
        alphaType = kUnpremul_SkAlphaType;
    }

    // Check for valid bits per pixel input
    switch (bitsPerPixel) {
        // In addition to more standard pixel compression formats, bmp supports
        // the use of bit masks to determine pixel components.  The standard
        // format for representing 16-bit colors is 555 (XRRRRRGGGGGBBBBB),
        // which does not map well to any Skia color formats.  For this reason,
        // we will always enable mask mode with 16 bits per pixel.
        case 16:
            if (kBitMask_BitmapInputFormat != inputFormat) {
                inputMasks.red = 0x7C00;
                inputMasks.green = 0x03E0;
                inputMasks.blue = 0x001F;
                inputFormat = kBitMask_BitmapInputFormat;
            }
            break;
        case 1:
        case 2:
        case 4:
        case 8:
        case 24:
        case 32:
            break;
        default:
            SkDebugf("Error: invalid input value for bits per pixel.\n");
            return NULL;
    }

    // Check that input bit masks are valid and create the masks object
    SkAutoTDelete<SkMasks>
            masks(SkMasks::CreateMasks(inputMasks, bitsPerPixel));
    if (NULL == masks) {
        SkDebugf("Error: invalid input masks.\n");
        return NULL;
    }

    // Check for a valid number of total bytes when in RLE mode
    if (totalBytes <= offset && kRLE_BitmapInputFormat == inputFormat) {
        SkDebugf("Error: RLE requires valid input size.\n");
        return NULL;
    }
    const size_t RLEBytes = totalBytes - offset;

    // Calculate the number of bytes read so far
    const uint32_t bytesRead = kBmpHeaderBytes + infoBytes + maskBytes;
    if (!isIco && offset < bytesRead) {
        SkDebugf("Error: pixel data offset less than header size.\n");
        return NULL;
    }

    // Return the codec
    // We will use ImageInfo to store width, height, and alpha type.  We will
    // set color type to kN32_SkColorType because that should be the default
    // output.
    const SkImageInfo& imageInfo = SkImageInfo::Make(width, height,
            kN32_SkColorType, alphaType);
    return SkNEW_ARGS(SkBmpCodec, (imageInfo, stream, bitsPerPixel,
                                   inputFormat, masks.detach(), numColors,
                                   bytesPerColor, offset - bytesRead,
                                   rowOrder, RLEBytes, isIco));
}

/*
 *
 * Creates an instance of the decoder
 * Called only by NewFromStream
 *
 */
SkBmpCodec::SkBmpCodec(const SkImageInfo& info, SkStream* stream,
                       uint16_t bitsPerPixel, BitmapInputFormat inputFormat,
                       SkMasks* masks, uint32_t numColors,
                       uint32_t bytesPerColor, uint32_t offset,
                       RowOrder rowOrder, size_t RLEBytes, bool isIco)
    : INHERITED(info, stream)
    , fBitsPerPixel(bitsPerPixel)
    , fInputFormat(inputFormat)
    , fMasks(masks)
    , fColorTable(NULL)
    , fNumColors(numColors)
    , fBytesPerColor(bytesPerColor)
    , fOffset(offset)
    , fRowOrder(rowOrder)
    , fRLEBytes(RLEBytes)
    , fIsIco(isIco)

{}

/*
 *
 * Initiates the bitmap decode
 *
 */
SkCodec::Result SkBmpCodec::onGetPixels(const SkImageInfo& dstInfo,
                                        void* dst, size_t dstRowBytes,
                                        const Options&,
                                        SkPMColor*, int*) {
    // Check for proper input and output formats
    if (!this->rewindIfNeeded()) {
        return kCouldNotRewind;
    }
    if (dstInfo.dimensions() != this->getInfo().dimensions()) {
        SkDebugf("Error: scaling not supported.\n");
        return kInvalidScale;
    }
    if (!conversion_possible(dstInfo, this->getInfo())) {
        SkDebugf("Error: cannot convert input type to output type.\n");
        return kInvalidConversion;
    }

    // Create the color table if necessary and prepare the stream for decode
    if (!createColorTable(dstInfo.alphaType())) {
        SkDebugf("Error: could not create color table.\n");
        return kInvalidInput;
    }

    // Perform the decode
    switch (fInputFormat) {
        case kBitMask_BitmapInputFormat:
            return decodeMask(dstInfo, dst, dstRowBytes);
        case kRLE_BitmapInputFormat:
            return decodeRLE(dstInfo, dst, dstRowBytes);
        case kStandard_BitmapInputFormat:
            return decode(dstInfo, dst, dstRowBytes);
        default:
            SkASSERT(false);
            return kInvalidInput;
    }
}

/*
 *
 * Process the color table for the bmp input
 *
 */
 bool SkBmpCodec::createColorTable(SkAlphaType alphaType) {
    // Allocate memory for color table
    uint32_t colorBytes = 0;
    uint32_t maxColors = 0;
    SkPMColor colorTable[256];
    if (fBitsPerPixel <= 8) {
        // Zero is a default for maxColors
        // Also set fNumColors to maxColors when it is too large
        maxColors = 1 << fBitsPerPixel;
        if (fNumColors == 0 || fNumColors >= maxColors) {
            fNumColors = maxColors;
        }

        // Read the color table from the stream
        colorBytes = fNumColors * fBytesPerColor;
        SkAutoTDeleteArray<uint8_t> cBuffer(SkNEW_ARRAY(uint8_t, colorBytes));
        if (stream()->read(cBuffer.get(), colorBytes) != colorBytes) {
            SkDebugf("Error: unable to read color table.\n");
            return false;
        }

        // Choose the proper packing function
        SkPMColor (*packARGB) (uint32_t, uint32_t, uint32_t, uint32_t);
        switch (alphaType) {
            case kOpaque_SkAlphaType:
            case kUnpremul_SkAlphaType:
                packARGB = &SkPackARGB32NoCheck;
                break;
            case kPremul_SkAlphaType:
                packARGB = &SkPreMultiplyARGB;
                break;
            default:
                // This should not be reached because conversion possible
                // should fail if the alpha type is not one of the above
                // values.
                SkASSERT(false);
                packARGB = NULL;
                break;
        }

        // Fill in the color table
        uint32_t i = 0;
        for (; i < fNumColors; i++) {
            uint8_t blue = get_byte(cBuffer.get(), i*fBytesPerColor);
            uint8_t green = get_byte(cBuffer.get(), i*fBytesPerColor + 1);
            uint8_t red = get_byte(cBuffer.get(), i*fBytesPerColor + 2);
            uint8_t alpha = kOpaque_SkAlphaType == alphaType ? 0xFF :
                    (fMasks->getAlphaMask() >> 24) &
                    get_byte(cBuffer.get(), i*fBytesPerColor + 3);
            colorTable[i] = packARGB(alpha, red, green, blue);
        }

        // To avoid segmentation faults on bad pixel data, fill the end of the
        // color table with black.  This is the same the behavior as the
        // chromium decoder.
        for (; i < maxColors; i++) {
            colorTable[i] = SkPackARGB32NoCheck(0xFF, 0, 0, 0);
        }
    }

    // Bmp-in-Ico files do not use an offset to indicate where the pixel data
    // begins.  Pixel data always begins immediately after the color table.
    if (!fIsIco) {
        // Check that we have not read past the pixel array offset
        if(fOffset < colorBytes) {
            // This may occur on OS 2.1 and other old versions where the color
            // table defaults to max size, and the bmp tries to use a smaller
            // color table.  This is invalid, and our decision is to indicate
            // an error, rather than try to guess the intended size of the
            // color table.
            SkDebugf("Error: pixel data offset less than color table size.\n");
            return false;
        }

        // After reading the color table, skip to the start of the pixel array
        if (stream()->skip(fOffset - colorBytes) != fOffset - colorBytes) {
            SkDebugf("Error: unable to skip to image data.\n");
            return false;
        }
    }

    // Set the color table and return true on success
    fColorTable.reset(SkNEW_ARGS(SkColorTable, (colorTable, maxColors)));
    return true;
}

/*
 *
 * Performs the bitmap decoding for bit masks input format
 *
 */
SkCodec::Result SkBmpCodec::decodeMask(const SkImageInfo& dstInfo,
                                       void* dst, size_t dstRowBytes) {
    // Set constant values
    const int width = dstInfo.width();
    const int height = dstInfo.height();
    const size_t rowBytes = SkAlign4(compute_row_bytes(width, fBitsPerPixel));

    // Allocate a buffer large enough to hold the full image
    SkAutoTDeleteArray<uint8_t>
        srcBuffer(SkNEW_ARRAY(uint8_t, height*rowBytes));
    uint8_t* srcRow = srcBuffer.get();

    // Create the swizzler
    SkAutoTDelete<SkMaskSwizzler> maskSwizzler(
            SkMaskSwizzler::CreateMaskSwizzler(dstInfo, dst, dstRowBytes,
            fMasks, fBitsPerPixel));

    // Iterate over rows of the image
    bool transparent = true;
    for (int y = 0; y < height; y++) {
        // Read a row of the input
        if (stream()->read(srcRow, rowBytes) != rowBytes) {
            SkDebugf("Warning: incomplete input stream.\n");
            return kIncompleteInput;
        }

        // Decode the row in destination format
        int row = kBottomUp_RowOrder == fRowOrder ? height - 1 - y : y;
        SkSwizzler::ResultAlpha r = maskSwizzler->next(srcRow, row);
        transparent &= SkSwizzler::IsTransparent(r);

        // Move to the next row
        srcRow = SkTAddOffset<uint8_t>(srcRow, rowBytes);
    }

    // Some fully transparent bmp images are intended to be opaque.  Here, we
    // correct for this possibility.
    if (transparent) {
        const SkImageInfo& opaqueInfo =
                dstInfo.makeAlphaType(kOpaque_SkAlphaType);
        SkAutoTDelete<SkMaskSwizzler> opaqueSwizzler(
                SkMaskSwizzler::CreateMaskSwizzler(opaqueInfo, dst, dstRowBytes,
                                                   fMasks, fBitsPerPixel));
        srcRow = srcBuffer.get();
        for (int y = 0; y < height; y++) {
            // Decode the row in opaque format
            int row = kBottomUp_RowOrder == fRowOrder ? height - 1 - y : y;
            opaqueSwizzler->next(srcRow, row);

            // Move to the next row
            srcRow = SkTAddOffset<uint8_t>(srcRow, rowBytes);
        }
    }

    // Finished decoding the entire image
    return kSuccess;
}

/*
 *
 * Set an RLE pixel using the color table
 *
 */
void SkBmpCodec::setRLEPixel(SkPMColor* dst, size_t dstRowBytes,
                             const SkImageInfo& dstInfo, uint32_t x, uint32_t y,
                             uint8_t index) {
    // Set the row
    int height = dstInfo.height();
    int row;
    if (kBottomUp_RowOrder == fRowOrder) {
        row = height - y - 1;
    } else {
        row = y;
    }

    // Set the pixel based on destination color type
    switch (dstInfo.colorType()) {
        case kN32_SkColorType: {
            SkPMColor* dstRow = SkTAddOffset<SkPMColor>(dst,
                    row * (int) dstRowBytes);
            dstRow[x] = fColorTable->operator[](index);
            break;
        }
        case kRGB_565_SkColorType: {
            uint16_t* dstRow = SkTAddOffset<uint16_t>(dst,
                    row * (int) dstRowBytes);
            dstRow[x] = SkPixel32ToPixel16(fColorTable->operator[](index));
            break;
        }
        default:
            // This case should not be reached.  We should catch an invalid
            // color type when we check that the conversion is possible.
            SkASSERT(false);
            break;
    }
}

/*
 *
 * Set an RLE pixel from R, G, B values
 *
 */
void SkBmpCodec::setRLE24Pixel(SkPMColor* dst, size_t dstRowBytes,
                               const SkImageInfo& dstInfo, uint32_t x,
                               uint32_t y, uint8_t red, uint8_t green,
                               uint8_t blue) {
    // Set the row
    int height = dstInfo.height();
    int row;
    if (kBottomUp_RowOrder == fRowOrder) {
        row = height - y - 1;
    } else {
        row = y;
    }

    // Set the pixel based on destination color type
    switch (dstInfo.colorType()) {
        case kN32_SkColorType: {
            SkPMColor* dstRow = SkTAddOffset<SkPMColor>(dst,
                    row * (int) dstRowBytes);
            dstRow[x] = SkPackARGB32NoCheck(0xFF, red, green, blue);
            break;
        }
        case kRGB_565_SkColorType: {
            uint16_t* dstRow = SkTAddOffset<uint16_t>(dst,
                    row * (int) dstRowBytes);
            dstRow[x] = SkPack888ToRGB16(red, green, blue);
            break;
        }
        default:
            // This case should not be reached.  We should catch an invalid
            // color type when we check that the conversion is possible.
            SkASSERT(false);
            break;
    }
}

/*
 *
 * Performs the bitmap decoding for RLE input format
 * RLE decoding is performed all at once, rather than a one row at a time
 *
 */
SkCodec::Result SkBmpCodec::decodeRLE(const SkImageInfo& dstInfo,
                                      void* dst, size_t dstRowBytes) {
    // Set RLE flags
    static const uint8_t RLE_ESCAPE = 0;
    static const uint8_t RLE_EOL = 0;
    static const uint8_t RLE_EOF = 1;
    static const uint8_t RLE_DELTA = 2;

    // Set constant values
    const int width = dstInfo.width();
    const int height = dstInfo.height();

    // Input buffer parameters
    uint32_t currByte = 0;
    SkAutoTDeleteArray<uint8_t> buffer(SkNEW_ARRAY(uint8_t, fRLEBytes));
    size_t totalBytes = stream()->read(buffer.get(), fRLEBytes);
    if (totalBytes < fRLEBytes) {
        SkDebugf("Warning: incomplete RLE file.\n");
    } else if (totalBytes <= 0) {
        SkDebugf("Error: could not read RLE image data.\n");
        return kInvalidInput;
    }

    // Destination parameters
    int x = 0;
    int y = 0;
    // If the code skips pixels, remaining pixels are transparent or black
    // TODO: Skip this if memory was already zeroed.
    memset(dst, 0, dstRowBytes * height);
    SkPMColor* dstPtr = (SkPMColor*) dst;

    while (true) {
        // Every entry takes at least two bytes
        if ((int) totalBytes - currByte < 2) {
            SkDebugf("Warning: incomplete RLE input.\n");
            return kIncompleteInput;
        }

        // Read the next two bytes.  These bytes have different meanings
        // depending on their values.  In the first interpretation, the first
        // byte is an escape flag and the second byte indicates what special
        // task to perform.
        const uint8_t flag = buffer.get()[currByte++];
        const uint8_t task = buffer.get()[currByte++];

        // If we have reached a row that is beyond the image size, and the RLE
        // code does not indicate end of file, abort and signal a warning.
        if (y >= height && (flag != RLE_ESCAPE || (task != RLE_EOF))) {
            SkDebugf("Warning: invalid RLE input.\n");
            return kIncompleteInput;
        }

        // Perform decoding
        if (RLE_ESCAPE == flag) {
            switch (task) {
                case RLE_EOL:
                    x = 0;
                    y++;
                    break;
                case RLE_EOF:
                    return kSuccess;
                case RLE_DELTA: {
                    // Two bytes are needed to specify delta
                    if ((int) totalBytes - currByte < 2) {
                        SkDebugf("Warning: incomplete RLE input\n");
                        return kIncompleteInput;
                    }
                    // Modify x and y
                    const uint8_t dx = buffer.get()[currByte++];
                    const uint8_t dy = buffer.get()[currByte++];
                    x += dx;
                    y += dy;
                    if (x > width || y > height) {
                        SkDebugf("Warning: invalid RLE input.\n");
                        return kIncompleteInput;
                    }
                    break;
                }
                default: {
                    // If task does not match any of the above signals, it
                    // indicates that we have a sequence of non-RLE pixels.
                    // Furthermore, the value of task is equal to the number
                    // of pixels to interpret.
                    uint8_t numPixels = task;
                    const size_t rowBytes = compute_row_bytes(numPixels,
                            fBitsPerPixel);
                    // Abort if setting numPixels moves us off the edge of the
                    // image.  Also abort if there are not enough bytes
                    // remaining in the stream to set numPixels.
                    if (x + numPixels > width ||
                            (int) totalBytes - currByte < SkAlign2(rowBytes)) {
                        SkDebugf("Warning: invalid RLE input.\n");
                        return kIncompleteInput;
                    }
                    // Set numPixels number of pixels
                    while (numPixels > 0) {
                        switch(fBitsPerPixel) {
                            case 4: {
                                SkASSERT(currByte < totalBytes);
                                uint8_t val = buffer.get()[currByte++];
                                setRLEPixel(dstPtr, dstRowBytes, dstInfo, x++,
                                        y, val >> 4);
                                numPixels--;
                                if (numPixels != 0) {
                                    setRLEPixel(dstPtr, dstRowBytes, dstInfo,
                                            x++, y, val & 0xF);
                                    numPixels--;
                                }
                                break;
                            }
                            case 8:
                                SkASSERT(currByte < totalBytes);
                                setRLEPixel(dstPtr, dstRowBytes, dstInfo, x++,
                                        y, buffer.get()[currByte++]);
                                numPixels--;
                                break;
                            case 24: {
                                SkASSERT(currByte + 2 < totalBytes);
                                uint8_t blue = buffer.get()[currByte++];
                                uint8_t green = buffer.get()[currByte++];
                                uint8_t red = buffer.get()[currByte++];
                                setRLE24Pixel(dstPtr, dstRowBytes, dstInfo,
                                            x++, y, red, green, blue);
                                numPixels--;
                            }
                            default:
                                SkASSERT(false);
                                return kInvalidInput;
                        }
                    }
                    // Skip a byte if necessary to maintain alignment
                    if (!SkIsAlign2(rowBytes)) {
                        currByte++;
                    }
                    break;
                }
            }
        } else {
            // If the first byte read is not a flag, it indicates the number of
            // pixels to set in RLE mode.
            const uint8_t numPixels = flag;
            const int endX = SkTMin<int>(x + numPixels, width);

            if (24 == fBitsPerPixel) {
                // In RLE24, the second byte read is part of the pixel color.
                // There are two more required bytes to finish encoding the
                // color.
                if ((int) totalBytes - currByte < 2) {
                    SkDebugf("Warning: incomplete RLE input\n");
                    return kIncompleteInput;
                }

                // Fill the pixels up to endX with the specified color
                uint8_t blue = task;
                uint8_t green = buffer.get()[currByte++];
                uint8_t red = buffer.get()[currByte++];
                while (x < endX) {
                    setRLE24Pixel(dstPtr, dstRowBytes, dstInfo, x++, y, red,
                            green, blue);
                }
            } else {
                // In RLE8 or RLE4, the second byte read gives the index in the
                // color table to look up the pixel color.
                // RLE8 has one color index that gets repeated
                // RLE4 has two color indexes in the upper and lower 4 bits of
                // the bytes, which are alternated
                uint8_t indices[2] = { task, task };
                if (4 == fBitsPerPixel) {
                    indices[0] >>= 4;
                    indices[1] &= 0xf;
                }

                // Set the indicated number of pixels
                for (int which = 0; x < endX; x++) {
                    setRLEPixel(dstPtr, dstRowBytes, dstInfo, x, y,
                            indices[which]);
                    which = !which;
                }
            }
        }
    }
}

/*
 *
 * Performs the bitmap decoding for standard input format
 *
 */
SkCodec::Result SkBmpCodec::decode(const SkImageInfo& dstInfo,
                                   void* dst, size_t dstRowBytes) {
    // Set constant values
    const int width = dstInfo.width();
    const int height = dstInfo.height();
    const size_t rowBytes = SkAlign4(compute_row_bytes(width, fBitsPerPixel));

    // Get swizzler configuration
    SkSwizzler::SrcConfig config;
    switch (fBitsPerPixel) {
        case 1:
            config = SkSwizzler::kIndex1;
            break;
        case 2:
            config = SkSwizzler::kIndex2;
            break;
        case 4:
            config = SkSwizzler::kIndex4;
            break;
        case 8:
            config = SkSwizzler::kIndex;
            break;
        case 24:
            config = SkSwizzler::kBGR;
            break;
        case 32:
            if (kOpaque_SkAlphaType == dstInfo.alphaType()) {
                config = SkSwizzler::kBGRX;
            } else {
                config = SkSwizzler::kBGRA;
            }
            break;
        default:
            SkASSERT(false);
            return kInvalidInput;
    }

    // Create swizzler
    SkAutoTDelete<SkSwizzler> swizzler(SkSwizzler::CreateSwizzler(config,
            fColorTable->readColors(), dstInfo, dst, dstRowBytes,
            SkImageGenerator::kNo_ZeroInitialized));

    // Allocate space for a row buffer and a source for the swizzler
    SkAutoTDeleteArray<uint8_t> srcBuffer(SkNEW_ARRAY(uint8_t, rowBytes));

    // Iterate over rows of the image
    // FIXME: bool transparent = true;
    for (int y = 0; y < height; y++) {
        // Read a row of the input
        if (stream()->read(srcBuffer.get(), rowBytes) != rowBytes) {
            SkDebugf("Warning: incomplete input stream.\n");
            return kIncompleteInput;
        }

        // Decode the row in destination format
        uint32_t row;
        if (kTopDown_RowOrder == fRowOrder) {
            row = y;
        } else {
            row = height - 1 - y;
        }

        swizzler->next(srcBuffer.get(), row);
        // FIXME: SkSwizzler::ResultAlpha r =
        //        swizzler->next(srcBuffer.get(), row);
        // FIXME: transparent &= SkSwizzler::IsTransparent(r);
    }

    // FIXME: This code exists to match the behavior in the chromium decoder
    // and to follow the bmp specification as it relates to alpha masks.  It is
    // commented out because we have yet to discover a test image that provides
    // an alpha mask and uses this decode mode.

    // Now we adjust the output image with some additional behavior that
    // SkSwizzler does not support.  Firstly, all bmp images that contain
    // alpha are masked by the alpha mask.  Secondly, many fully transparent
    // bmp images are intended to be opaque.  Here, we make those corrections.
    /*
    SkPMColor* dstRow = (SkPMColor*) dst;
    if (SkSwizzler::kBGRA == config) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                if (transparent) {
                    dstRow[x] |= 0xFF000000;
                } else {
                    dstRow[x] &= alphaMask;
                }
                dstRow = SkTAddOffset<SkPMColor>(dstRow, dstRowBytes);
            }
        }
    }
    */

    // Finally, apply the AND mask for bmp-in-ico images
    if (fIsIco) {
        // The AND mask is always 1 bit per pixel
        const size_t rowBytes = SkAlign4(compute_row_bytes(width, 1));

        SkPMColor* dstPtr = (SkPMColor*) dst;
        for (int y = 0; y < height; y++) {
            // The srcBuffer will at least be large enough
            if (stream()->read(srcBuffer.get(), rowBytes) != rowBytes) {
                SkDebugf("Warning: incomplete AND mask for bmp-in-ico.\n");
                return kIncompleteInput;
            }

            int row;
            if (kBottomUp_RowOrder == fRowOrder) {
                row = height - y - 1;
            } else {
                row = y;
            }

            SkPMColor* dstRow =
                    SkTAddOffset<SkPMColor>(dstPtr, row * dstRowBytes);

            for (int x = 0; x < width; x++) {
                int quotient;
                int modulus;
                SkTDivMod(x, 8, &quotient, &modulus);
                uint32_t shift = 7 - modulus;
                uint32_t alphaBit =
                        (srcBuffer.get()[quotient] >> shift) & 0x1;
                dstRow[x] &= alphaBit - 1;
            }
        }
    }

    // Finished decoding the entire image
    return kSuccess;
}

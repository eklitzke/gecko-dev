
/*
 * Copyright 2012 Mozilla Foundation
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cairo.h"
#include "cairo-ft.h"

#include "SkFontHost_FreeType_common.h"

#include "SkAdvancedTypefaceMetrics.h"
#include "SkFDot6.h"
#include "SkPath.h"
#include "SkScalerContext.h"
#include "SkTypefaceCache.h"

#include <cmath>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

// for FT_GlyphSlot_Embolden
#ifdef FT_SYNTHESIS_H
#include FT_SYNTHESIS_H
#endif

// for FT_Library_SetLcdFilter
#ifdef FT_LCD_FILTER_H
#include FT_LCD_FILTER_H
#else
typedef enum FT_LcdFilter_
{
    FT_LCD_FILTER_NONE    = 0,
    FT_LCD_FILTER_DEFAULT = 1,
    FT_LCD_FILTER_LIGHT   = 2,
    FT_LCD_FILTER_LEGACY  = 16,
} FT_LcdFilter;
#endif

// If compiling with FreeType before 2.5.0
#ifndef FT_LOAD_COLOR
#    define FT_LOAD_COLOR ( 1L << 20 )
#    define FT_PIXEL_MODE_BGRA 7
#endif

#ifndef SK_CAN_USE_DLOPEN
#define SK_CAN_USE_DLOPEN 1
#endif
#if SK_CAN_USE_DLOPEN
#include <dlfcn.h>
#endif

#ifndef SK_FONTHOST_CAIRO_STANDALONE
#define SK_FONTHOST_CAIRO_STANDALONE 1
#endif

static cairo_user_data_key_t kSkTypefaceKey;

static bool gFontHintingEnabled = true;
static FT_Error (*gSetLcdFilter)(FT_Library, FT_LcdFilter) = nullptr;
static void (*gGlyphSlotEmbolden)(FT_GlyphSlot) = nullptr;

void SkInitCairoFT(bool fontHintingEnabled)
{
    gFontHintingEnabled = fontHintingEnabled;
#if SK_CAN_USE_DLOPEN
    gSetLcdFilter = (FT_Error (*)(FT_Library, FT_LcdFilter))dlsym(RTLD_DEFAULT, "FT_Library_SetLcdFilter");
    gGlyphSlotEmbolden = (void (*)(FT_GlyphSlot))dlsym(RTLD_DEFAULT, "FT_GlyphSlot_Embolden");
#else
    gSetLcdFilter = &FT_Library_SetLcdFilter;
    gGlyphSlotEmbolden = &FT_GlyphSlot_Embolden;
#endif
    // FT_Library_SetLcdFilter may be provided but have no effect if FreeType
    // is built without FT_CONFIG_OPTION_SUBPIXEL_RENDERING.
    if (gSetLcdFilter &&
        gSetLcdFilter(nullptr, FT_LCD_FILTER_NONE) == FT_Err_Unimplemented_Feature) {
        gSetLcdFilter = nullptr;
    }
}

#ifndef CAIRO_HAS_FC_FONT
typedef struct _FcPattern FcPattern;
#endif

class SkScalerContext_CairoFT : public SkScalerContext_FreeType_Base {
public:
    SkScalerContext_CairoFT(sk_sp<SkTypeface> typeface, const SkScalerContextEffects& effects, const SkDescriptor* desc,
                            cairo_font_face_t* fontFace, FcPattern* pattern);
    virtual ~SkScalerContext_CairoFT();

    bool isValid() const {
        return fScaledFont != nullptr;
    }

protected:
    virtual unsigned generateGlyphCount() override;
    virtual uint16_t generateCharToGlyph(SkUnichar uniChar) override;
    virtual void generateAdvance(SkGlyph* glyph) override;
    virtual void generateMetrics(SkGlyph* glyph) override;
    virtual void generateImage(const SkGlyph& glyph) override;
    virtual void generatePath(const SkGlyphID glyphID, SkPath* path) override;
    virtual void generateFontMetrics(SkPaint::FontMetrics* metrics) override;
    virtual SkUnichar generateGlyphToChar(uint16_t glyph) override;

private:
    bool computeShapeMatrix(const SkMatrix& m);
    void prepareGlyph(FT_GlyphSlot glyph);
    void fixVerticalLayoutBearing(FT_GlyphSlot glyph);

#ifdef CAIRO_HAS_FC_FONT
    void parsePattern(FcPattern* pattern);
#endif

    cairo_scaled_font_t* fScaledFont;
    FT_Int32 fLoadGlyphFlags;
    FT_LcdFilter fLcdFilter;
    SkScalar fScaleX;
    SkScalar fScaleY;
    SkMatrix fShapeMatrix;
    FT_Matrix fShapeMatrixFT;
    bool fHaveShape;
};

class CairoLockedFTFace {
public:
    CairoLockedFTFace(cairo_scaled_font_t* scaledFont)
        : fScaledFont(scaledFont)
        , fFace(cairo_ft_scaled_font_lock_face(scaledFont))
    {}

    ~CairoLockedFTFace()
    {
        cairo_ft_scaled_font_unlock_face(fScaledFont);
    }

    FT_Face getFace()
    {
        return fFace;
    }

private:
    cairo_scaled_font_t* fScaledFont;
    FT_Face fFace;
};

template<typename T> static bool isLCD(const T& rec) {
    return SkMask::kLCD16_Format == rec.fMaskFormat;
}

static bool bothZero(SkScalar a, SkScalar b) {
    return 0 == a && 0 == b;
}

// returns false if there is any non-90-rotation or skew
static bool isAxisAligned(const SkScalerContextRec& rec) {
    return 0 == rec.fPreSkewX &&
           (bothZero(rec.fPost2x2[0][1], rec.fPost2x2[1][0]) ||
            bothZero(rec.fPost2x2[0][0], rec.fPost2x2[1][1]));
}

class SkCairoFTTypeface : public SkTypeface {
public:
    static SkTypeface* CreateTypeface(cairo_font_face_t* fontFace, FT_Face face,
                                      FcPattern* pattern = nullptr) {
        SkASSERT(fontFace != nullptr);
        SkASSERT(cairo_font_face_get_type(fontFace) == CAIRO_FONT_TYPE_FT);
        SkASSERT(face != nullptr);

        SkFontStyle style(face->style_flags & FT_STYLE_FLAG_BOLD ?
                              SkFontStyle::kBold_Weight : SkFontStyle::kNormal_Weight,
                          SkFontStyle::kNormal_Width,
                          face->style_flags & FT_STYLE_FLAG_ITALIC ?
                              SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);

        bool isFixedWidth = face->face_flags & FT_FACE_FLAG_FIXED_WIDTH;

        return new SkCairoFTTypeface(style, isFixedWidth, fontFace, pattern);
    }

    virtual SkStreamAsset* onOpenStream(int*) const override { return nullptr; }

    virtual std::unique_ptr<SkAdvancedTypefaceMetrics> onGetAdvancedMetrics() const override
    {
        SkDEBUGCODE(SkDebugf("SkCairoFTTypeface::onGetAdvancedMetrics unimplemented\n"));
        return nullptr;
    }

    virtual SkScalerContext* onCreateScalerContext(const SkScalerContextEffects& effects, const SkDescriptor* desc) const override
    {
        SkScalerContext_CairoFT* ctx =
            new SkScalerContext_CairoFT(sk_ref_sp(const_cast<SkCairoFTTypeface*>(this)),
                                        effects, desc, fFontFace, fPattern);
        if (!ctx->isValid()) {
            delete ctx;
            return nullptr;
        }
        return ctx;
    }

    virtual void onFilterRec(SkScalerContextRec* rec) const override
    {
        // No subpixel AA unless enabled in Fontconfig.
        if (!fPattern && isLCD(*rec)) {
            rec->fMaskFormat = SkMask::kA8_Format;
        }

        // rotated text looks bad with hinting, so we disable it as needed
        if (!gFontHintingEnabled || !isAxisAligned(*rec)) {
            rec->setHinting(SkPaint::kNo_Hinting);
        }

        // Don't apply any gamma so that we match cairo-ft's results.
        rec->ignorePreBlend();
    }

    virtual int onGetVariationDesignPosition(
        SkFontArguments::VariationPosition::Coordinate coordinates[],
        int coordinateCount) const override
    {
        return -1;
    }

    virtual void onGetFontDescriptor(SkFontDescriptor*, bool*) const override
    {
        SkDEBUGCODE(SkDebugf("SkCairoFTTypeface::onGetFontDescriptor unimplemented\n"));
    }

    virtual int onCharsToGlyphs(void const*, SkTypeface::Encoding, uint16_t*, int) const override
    {
        return 0;
    }

    virtual int onCountGlyphs() const override
    {
        return 0;
    }

    virtual int onGetUPEM() const override
    {
        return 0;
    }

    virtual SkTypeface::LocalizedStrings* onCreateFamilyNameIterator() const override
    {
        return nullptr;
    }

    virtual void onGetFamilyName(SkString* familyName) const override
    {
        familyName->reset();
    }

    virtual int onGetTableTags(SkFontTableTag*) const override
    {
        return 0;
    }

    virtual size_t onGetTableData(SkFontTableTag, size_t, size_t, void*) const override
    {
        return 0;
    }

private:

    SkCairoFTTypeface(const SkFontStyle& style, bool isFixedWidth,
                      cairo_font_face_t* fontFace, FcPattern* pattern)
        : SkTypeface(style, isFixedWidth)
        , fFontFace(fontFace)
        , fPattern(pattern)
    {
        cairo_font_face_set_user_data(fFontFace, &kSkTypefaceKey, this, nullptr);
        cairo_font_face_reference(fFontFace);
#ifdef CAIRO_HAS_FC_FONT
        if (fPattern) {
            FcPatternReference(fPattern);
        }
#endif
    }

    ~SkCairoFTTypeface()
    {
        cairo_font_face_set_user_data(fFontFace, &kSkTypefaceKey, nullptr, nullptr);
        cairo_font_face_destroy(fFontFace);
#ifdef CAIRO_HAS_FC_FONT
        if (fPattern) {
            FcPatternDestroy(fPattern);
        }
#endif
    }

    cairo_font_face_t* fFontFace;
    FcPattern* fPattern;
};

SkTypeface* SkCreateTypefaceFromCairoFTFontWithFontconfig(cairo_scaled_font_t* scaledFont, FcPattern* pattern)
{
    cairo_font_face_t* fontFace = cairo_scaled_font_get_font_face(scaledFont);
    SkASSERT(cairo_font_face_status(fontFace) == CAIRO_STATUS_SUCCESS);

    SkTypeface* typeface = reinterpret_cast<SkTypeface*>(cairo_font_face_get_user_data(fontFace, &kSkTypefaceKey));
    if (typeface) {
        typeface->ref();
    } else {
        CairoLockedFTFace faceLock(scaledFont);
        if (FT_Face face = faceLock.getFace()) {
            typeface = SkCairoFTTypeface::CreateTypeface(fontFace, face, pattern);
            SkTypefaceCache::Add(typeface);
        }
    }

    return typeface;
}

SkTypeface* SkCreateTypefaceFromCairoFTFont(cairo_scaled_font_t* scaledFont)
{
    return SkCreateTypefaceFromCairoFTFontWithFontconfig(scaledFont, nullptr);
}

SkScalerContext_CairoFT::SkScalerContext_CairoFT(sk_sp<SkTypeface> typeface, const SkScalerContextEffects& effects, const SkDescriptor* desc,
                                                 cairo_font_face_t* fontFace, FcPattern* pattern)
    : SkScalerContext_FreeType_Base(std::move(typeface), effects, desc)
    , fLcdFilter(FT_LCD_FILTER_NONE)
{
    SkMatrix matrix;
    fRec.getSingleMatrix(&matrix);

    cairo_matrix_t fontMatrix, ctMatrix;
    cairo_matrix_init(&fontMatrix, matrix.getScaleX(), matrix.getSkewY(), matrix.getSkewX(), matrix.getScaleY(), 0.0, 0.0);
    cairo_matrix_init_identity(&ctMatrix);

    cairo_font_options_t *fontOptions = cairo_font_options_create();
    fScaledFont = cairo_scaled_font_create(fontFace, &fontMatrix, &ctMatrix, fontOptions);
    cairo_font_options_destroy(fontOptions);

    computeShapeMatrix(matrix);

    fRec.fFlags |= SkScalerContext::kEmbeddedBitmapText_Flag;

#ifdef CAIRO_HAS_FC_FONT
    if (pattern) {
        parsePattern(pattern);
    }
#endif

    FT_Int32 loadFlags = FT_LOAD_DEFAULT;

    if (SkMask::kBW_Format == fRec.fMaskFormat) {
        if (fRec.getHinting() == SkPaint::kNo_Hinting) {
            loadFlags |= FT_LOAD_NO_HINTING;
        } else {
            loadFlags = FT_LOAD_TARGET_MONO;
        }
        loadFlags |= FT_LOAD_MONOCHROME;
    } else {
        switch (fRec.getHinting()) {
        case SkPaint::kNo_Hinting:
            loadFlags |= FT_LOAD_NO_HINTING;
            break;
        case SkPaint::kSlight_Hinting:
            loadFlags = FT_LOAD_TARGET_LIGHT;  // This implies FORCE_AUTOHINT
            break;
        case SkPaint::kNormal_Hinting:
            if (fRec.fFlags & SkScalerContext::kForceAutohinting_Flag) {
                loadFlags |= FT_LOAD_FORCE_AUTOHINT;
            }
            break;
        case SkPaint::kFull_Hinting:
            if (isLCD(fRec)) {
                if (fRec.fFlags & SkScalerContext::kLCD_Vertical_Flag) {
                    loadFlags = FT_LOAD_TARGET_LCD_V;
                } else {
                    loadFlags = FT_LOAD_TARGET_LCD;
                }
            }
            if (fRec.fFlags & SkScalerContext::kForceAutohinting_Flag) {
                loadFlags |= FT_LOAD_FORCE_AUTOHINT;
            }
            break;
        default:
            SkDebugf("---------- UNKNOWN hinting %d\n", fRec.getHinting());
            break;
        }
    }

    // Disable autohinting to disable hinting even for "tricky" fonts.
    if (!gFontHintingEnabled) {
        loadFlags |= FT_LOAD_NO_AUTOHINT;
    }

    if ((fRec.fFlags & SkScalerContext::kEmbeddedBitmapText_Flag) == 0) {
        loadFlags |= FT_LOAD_NO_BITMAP;
    }

    // Always using FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH to get correct
    // advances, as fontconfig and cairo do.
    // See http://code.google.com/p/skia/issues/detail?id=222.
    loadFlags |= FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH;

    if (fRec.fFlags & SkScalerContext::kVertical_Flag) {
        loadFlags |= FT_LOAD_VERTICAL_LAYOUT;
    }

    loadFlags |= FT_LOAD_COLOR;

    fLoadGlyphFlags = loadFlags;
}

SkScalerContext_CairoFT::~SkScalerContext_CairoFT()
{
    cairo_scaled_font_destroy(fScaledFont);
}

#ifdef CAIRO_HAS_FC_FONT
void SkScalerContext_CairoFT::parsePattern(FcPattern* pattern)
{
    FcBool antialias, autohint, bitmap, embolden, hinting, vertical;

    if (FcPatternGetBool(pattern, FC_AUTOHINT, 0, &autohint) == FcResultMatch && autohint) {
        fRec.fFlags |= SkScalerContext::kForceAutohinting_Flag;
    }
    if (FcPatternGetBool(pattern, FC_EMBOLDEN, 0, &embolden) == FcResultMatch && embolden) {
        fRec.fFlags |= SkScalerContext::kEmbolden_Flag;
    }
    if (FcPatternGetBool(pattern, FC_VERTICAL_LAYOUT, 0, &vertical) == FcResultMatch && vertical) {
        fRec.fFlags |= SkScalerContext::kVertical_Flag;
    }

    // Match cairo-ft's handling of embeddedbitmap:
    // If AA is explicitly disabled, leave bitmaps enabled.
    // Otherwise, disable embedded bitmaps unless explicitly enabled.
    if (FcPatternGetBool(pattern, FC_ANTIALIAS, 0, &antialias) == FcResultMatch && !antialias) {
        fRec.fMaskFormat = SkMask::kBW_Format;
    } else if (FcPatternGetBool(pattern, FC_EMBEDDED_BITMAP, 0, &bitmap) != FcResultMatch || !bitmap) {
        fRec.fFlags &= ~SkScalerContext::kEmbeddedBitmapText_Flag;
    }

    if (fRec.fMaskFormat != SkMask::kBW_Format) {
        int rgba;
        if (!isLCD(fRec) ||
            FcPatternGetInteger(pattern, FC_RGBA, 0, &rgba) != FcResultMatch) {
            rgba = FC_RGBA_UNKNOWN;
        }
        switch (rgba) {
        case FC_RGBA_RGB:
            break;
        case FC_RGBA_BGR:
            fRec.fFlags |= SkScalerContext::kLCD_BGROrder_Flag;
            break;
        case FC_RGBA_VRGB:
            fRec.fFlags |= SkScalerContext::kLCD_Vertical_Flag;
            break;
        case FC_RGBA_VBGR:
            fRec.fFlags |= SkScalerContext::kLCD_Vertical_Flag |
                           SkScalerContext::kLCD_BGROrder_Flag;
            break;
        default:
            fRec.fMaskFormat = SkMask::kA8_Format;
            break;
        }

        int filter;
        if (isLCD(fRec)) {
            if (FcPatternGetInteger(pattern, FC_LCD_FILTER, 0, &filter) != FcResultMatch) {
                filter = FC_LCD_LEGACY;
            }
            switch (filter) {
            case FC_LCD_NONE:
                fLcdFilter = FT_LCD_FILTER_NONE;
                break;
            case FC_LCD_DEFAULT:
                fLcdFilter = FT_LCD_FILTER_DEFAULT;
                break;
            case FC_LCD_LIGHT:
                fLcdFilter = FT_LCD_FILTER_LIGHT;
                break;
            case FC_LCD_LEGACY:
            default:
                fLcdFilter = FT_LCD_FILTER_LEGACY;
                break;
            }
        }
    }

    if (fRec.getHinting() != SkPaint::kNo_Hinting) {
        // Hinting was requested, so check if the fontconfig pattern needs to override it.
        // If hinting is either explicitly enabled by fontconfig or not configured, try to
        // parse the hint style. Otherwise, ensure hinting is disabled.
        int hintstyle;
        if (FcPatternGetBool(pattern, FC_HINTING, 0, &hinting) != FcResultMatch || hinting) {
            if (FcPatternGetInteger(pattern, FC_HINT_STYLE, 0, &hintstyle) != FcResultMatch) {
                hintstyle = FC_HINT_FULL;
            }
        } else {
            hintstyle = FC_HINT_NONE;
        }
        switch (hintstyle) {
        case FC_HINT_NONE:
            fRec.setHinting(SkPaint::kNo_Hinting);
            break;
        case FC_HINT_SLIGHT:
            fRec.setHinting(SkPaint::kSlight_Hinting);
            break;
        case FC_HINT_MEDIUM:
        default:
            fRec.setHinting(SkPaint::kNormal_Hinting);
            break;
        case FC_HINT_FULL:
            fRec.setHinting(SkPaint::kFull_Hinting);
            break;
        }
    }
}
#endif

bool SkScalerContext_CairoFT::computeShapeMatrix(const SkMatrix& m)
{
    // Compute a shape matrix compatible with Cairo's _compute_transform.
    // Finds major/minor scales and uses them to normalize the transform.
    double scaleX = m.getScaleX();
    double skewX = m.getSkewX();
    double skewY = m.getSkewY();
    double scaleY = m.getScaleY();
    double det = scaleX * scaleY - skewY * skewX;
    if (!std::isfinite(det)) {
        fScaleX = fRec.fTextSize * fRec.fPreScaleX;
        fScaleY = fRec.fTextSize;
        fHaveShape = false;
        return false;
    }
    double major = det != 0.0 ? hypot(scaleX, skewY) : 0.0;
    double minor = major != 0.0 ? fabs(det) / major : 0.0;
    // Limit scales to be above 1pt.
    major = SkTMax(major, 1.0);
    minor = SkTMax(minor, 1.0);

    // If the font is not scalable, then choose the best available size.
    CairoLockedFTFace faceLock(fScaledFont);
    FT_Face face = faceLock.getFace();
    if (face && !FT_IS_SCALABLE(face)) {
        double bestDist = DBL_MAX;
        FT_Int bestSize = -1;
        for (FT_Int i = 0; i < face->num_fixed_sizes; i++) {
            // Distance is positive if strike is larger than desired size,
            // or negative if smaller. If previously a found smaller strike,
            // then prefer a larger strike. Otherwise, minimize distance.
            double dist = face->available_sizes[i].y_ppem / 64.0 - minor;
            if (bestDist < 0 ? dist >= bestDist : fabs(dist) <= bestDist) {
                bestDist = dist;
                bestSize = i;
            }
        }
        if (bestSize < 0) {
            fScaleX = fRec.fTextSize * fRec.fPreScaleX;
            fScaleY = fRec.fTextSize;
            fHaveShape = false;
            return false;
        }
        major = face->available_sizes[bestSize].x_ppem / 64.0;
        minor = face->available_sizes[bestSize].y_ppem / 64.0;
        fHaveShape = true;
    } else {
        fHaveShape = !m.isScaleTranslate();
    }

    fScaleX = SkDoubleToScalar(major);
    fScaleY = SkDoubleToScalar(minor);

    if (fHaveShape) {
        // Normalize the transform and convert to fixed-point.
        fShapeMatrix = m;
        fShapeMatrix.preScale(SkDoubleToScalar(1.0 / major), SkDoubleToScalar(1.0 / minor));

        fShapeMatrixFT.xx = SkScalarToFixed(fShapeMatrix.getScaleX());
        fShapeMatrixFT.yx = SkScalarToFixed(-fShapeMatrix.getSkewY());
        fShapeMatrixFT.xy = SkScalarToFixed(-fShapeMatrix.getSkewX());
        fShapeMatrixFT.yy = SkScalarToFixed(fShapeMatrix.getScaleY());
    }
    return true;
}

unsigned SkScalerContext_CairoFT::generateGlyphCount()
{
    CairoLockedFTFace faceLock(fScaledFont);
    return faceLock.getFace()->num_glyphs;
}

uint16_t SkScalerContext_CairoFT::generateCharToGlyph(SkUnichar uniChar)
{
    CairoLockedFTFace faceLock(fScaledFont);
    return SkToU16(FT_Get_Char_Index(faceLock.getFace(), uniChar));
}

void SkScalerContext_CairoFT::generateAdvance(SkGlyph* glyph)
{
    generateMetrics(glyph);
}

void SkScalerContext_CairoFT::prepareGlyph(FT_GlyphSlot glyph)
{
    if (fRec.fFlags & SkScalerContext::kEmbolden_Flag &&
        gGlyphSlotEmbolden) {
        gGlyphSlotEmbolden(glyph);
    }
    if (fRec.fFlags & SkScalerContext::kVertical_Flag) {
        fixVerticalLayoutBearing(glyph);
    }
}

void SkScalerContext_CairoFT::fixVerticalLayoutBearing(FT_GlyphSlot glyph)
{
    FT_Vector vector;
    vector.x = glyph->metrics.vertBearingX - glyph->metrics.horiBearingX;
    vector.y = -glyph->metrics.vertBearingY - glyph->metrics.horiBearingY;
    if (glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        if (fHaveShape) {
            FT_Vector_Transform(&vector, &fShapeMatrixFT);
        }
        FT_Outline_Translate(&glyph->outline, vector.x, vector.y);
    } else if (glyph->format == FT_GLYPH_FORMAT_BITMAP) {
        glyph->bitmap_left += SkFDot6Floor(vector.x);
        glyph->bitmap_top  += SkFDot6Floor(vector.y);
    }
}

void SkScalerContext_CairoFT::generateMetrics(SkGlyph* glyph)
{
    SkASSERT(fScaledFont != nullptr);

    glyph->zeroMetrics();

    CairoLockedFTFace faceLock(fScaledFont);
    FT_Face face = faceLock.getFace();

    FT_Error err = FT_Load_Glyph( face, glyph->getGlyphID(), fLoadGlyphFlags );
    if (err != 0) {
        return;
    }

    prepareGlyph(face->glyph);

    if (fRec.fFlags & SkScalerContext::kVertical_Flag) {
        glyph->fAdvanceX = -SkFDot6ToFloat(face->glyph->advance.x);
        glyph->fAdvanceY = SkFDot6ToFloat(face->glyph->advance.y);
    } else {
        glyph->fAdvanceX = SkFDot6ToFloat(face->glyph->advance.x);
        glyph->fAdvanceY = -SkFDot6ToFloat(face->glyph->advance.y);
    }

    SkIRect bounds;
    switch (face->glyph->format) {
    case FT_GLYPH_FORMAT_OUTLINE:
        if (!face->glyph->outline.n_contours) {
            return;
        }

        FT_BBox bbox;
        FT_Outline_Get_CBox(&face->glyph->outline, &bbox);
        bbox.xMin &= ~63;
        bbox.yMin &= ~63;
        bbox.xMax = (bbox.xMax + 63) & ~63;
        bbox.yMax = (bbox.yMax + 63) & ~63;
        bounds = SkIRect::MakeLTRB(SkFDot6Floor(bbox.xMin),
                                   -SkFDot6Floor(bbox.yMax),
                                   SkFDot6Floor(bbox.xMax),
                                   -SkFDot6Floor(bbox.yMin));

        if (isLCD(fRec)) {
            // In FreeType < 2.8.1, LCD filtering, if explicitly used, may
            // add padding to the glyph. When not used, there is no padding.
            // As of 2.8.1, LCD filtering is now always supported and may
            // add padding even if an LCD filter is not explicitly set.
            // Regardless, if no LCD filtering is used, or if LCD filtering
            // doesn't add padding, it is safe to modify the glyph's bounds
            // here. generateGlyphImage will detect if the mask is smaller
            // than the bounds and clip things appropriately.
            if (fRec.fFlags & kLCD_Vertical_Flag) {
                bounds.outset(0, 1);
            } else {
                bounds.outset(1, 0);
            }
        }
        break;
    case FT_GLYPH_FORMAT_BITMAP:
        if (face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
            glyph->fMaskFormat = SkMask::kARGB32_Format;
        }

        if (isLCD(fRec)) {
            fRec.fMaskFormat = SkMask::kA8_Format;
        }

        if (fHaveShape) {
            // Ensure filtering is preserved when the bitmap is transformed.
            // Otherwise, the result will look horrifically aliased.
            if (fRec.fMaskFormat == SkMask::kBW_Format) {
                fRec.fMaskFormat = SkMask::kA8_Format;
            }

            // Apply the shape matrix to the glyph's bounding box.
            SkRect srcRect = SkRect::MakeXYWH(
                SkIntToScalar(face->glyph->bitmap_left),
                -SkIntToScalar(face->glyph->bitmap_top),
                SkIntToScalar(face->glyph->bitmap.width),
                SkIntToScalar(face->glyph->bitmap.rows));
            SkRect destRect;
            fShapeMatrix.mapRect(&destRect, srcRect);
            SkIRect glyphRect = destRect.roundOut();
            bounds = SkIRect::MakeXYWH(SkScalarRoundToInt(destRect.fLeft),
                                       SkScalarRoundToInt(destRect.fTop),
                                       glyphRect.width(),
                                       glyphRect.height());
        } else {
            bounds = SkIRect::MakeXYWH(face->glyph->bitmap_left,
                                       -face->glyph->bitmap_top,
                                       face->glyph->bitmap.width,
                                       face->glyph->bitmap.rows);
        }
        break;
    default:
        SkDEBUGFAIL("unknown glyph format");
        return;
    }

    if (SkIRect::MakeXYWH(SHRT_MIN, SHRT_MIN, USHRT_MAX, USHRT_MAX).contains(bounds)) {
        glyph->fWidth  = SkToU16(bounds.width());
        glyph->fHeight = SkToU16(bounds.height());
        glyph->fLeft   = SkToS16(bounds.left());
        glyph->fTop    = SkToS16(bounds.top());
    }
}

void SkScalerContext_CairoFT::generateImage(const SkGlyph& glyph)
{
    SkASSERT(fScaledFont != nullptr);
    CairoLockedFTFace faceLock(fScaledFont);
    FT_Face face = faceLock.getFace();

    FT_Error err = FT_Load_Glyph(face, glyph.getGlyphID(), fLoadGlyphFlags);

    if (err != 0) {
        memset(glyph.fImage, 0, glyph.rowBytes() * glyph.fHeight);
        return;
    }

    prepareGlyph(face->glyph);

    bool useLcdFilter =
        face->glyph->format == FT_GLYPH_FORMAT_OUTLINE &&
        isLCD(glyph) &&
        gSetLcdFilter;
    if (useLcdFilter) {
        gSetLcdFilter(face->glyph->library, fLcdFilter);
    }

    SkMatrix matrix;
    if (face->glyph->format == FT_GLYPH_FORMAT_BITMAP &&
        fHaveShape) {
        matrix = fShapeMatrix;
    } else {
        matrix.setIdentity();
    }
    generateGlyphImage(face, glyph, matrix);

    if (useLcdFilter) {
        gSetLcdFilter(face->glyph->library, FT_LCD_FILTER_NONE);
    }
}

void SkScalerContext_CairoFT::generatePath(const SkGlyphID glyphID, SkPath* path)
{
    SkASSERT(fScaledFont != nullptr);
    CairoLockedFTFace faceLock(fScaledFont);
    FT_Face face = faceLock.getFace();

    SkASSERT(path);

    uint32_t flags = fLoadGlyphFlags;
    flags |= FT_LOAD_NO_BITMAP; // ignore embedded bitmaps so we're sure to get the outline
    flags &= ~FT_LOAD_RENDER;   // don't scan convert (we just want the outline)

    FT_Error err = FT_Load_Glyph(face, glyphID, flags);

    if (err != 0) {
        path->reset();
        return;
    }

    prepareGlyph(face->glyph);

    generateGlyphPath(face, path);
}

void SkScalerContext_CairoFT::generateFontMetrics(SkPaint::FontMetrics* metrics)
{
    if (metrics) {
        memset(metrics, 0, sizeof(SkPaint::FontMetrics));
    }
}

SkUnichar SkScalerContext_CairoFT::generateGlyphToChar(uint16_t glyph)
{
    SkASSERT(fScaledFont != nullptr);
    CairoLockedFTFace faceLock(fScaledFont);
    FT_Face face = faceLock.getFace();

    FT_UInt glyphIndex;
    SkUnichar charCode = FT_Get_First_Char(face, &glyphIndex);
    while (glyphIndex != 0) {
        if (glyphIndex == glyph) {
            return charCode;
        }
        charCode = FT_Get_Next_Char(face, charCode, &glyphIndex);
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////

#include "SkFontMgr.h"

sk_sp<SkFontMgr> SkFontMgr::Factory() {
    // todo
    return nullptr;
}


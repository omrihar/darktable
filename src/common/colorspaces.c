/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "common/colormatrices.c"
#include "common/colorspaces.h"
#include "common/debug.h"
#include "common/srgb_tone_curve_values.h"
#include "develop/imageop.h"

#ifdef USE_COLORDGTK
#include "colord-gtk.h"
#endif

#ifdef GDK_WINDOWING_QUARTZ
#include <Carbon/Carbon.h>
#include <ApplicationServices/ApplicationServices.h>
#include <CoreServices/CoreServices.h>
#endif

#define generate_mat3inv_body(c_type, A, B)                                                                  \
  int mat3inv_##c_type(c_type *const dst, const c_type *const src)                                           \
  {                                                                                                          \
                                                                                                             \
    const c_type det = A(1, 1) * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3))                                     \
                       - A(2, 1) * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3))                                   \
                       + A(3, 1) * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));                                  \
                                                                                                             \
    const c_type epsilon = 1e-7f;                                                                            \
    if(fabs(det) < epsilon) return 1;                                                                        \
                                                                                                             \
    const c_type invDet = 1.0 / det;                                                                         \
                                                                                                             \
    B(1, 1) = invDet * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3));                                              \
    B(1, 2) = -invDet * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3));                                             \
    B(1, 3) = invDet * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));                                              \
                                                                                                             \
    B(2, 1) = -invDet * (A(3, 3) * A(2, 1) - A(3, 1) * A(2, 3));                                             \
    B(2, 2) = invDet * (A(3, 3) * A(1, 1) - A(3, 1) * A(1, 3));                                              \
    B(2, 3) = -invDet * (A(2, 3) * A(1, 1) - A(2, 1) * A(1, 3));                                             \
                                                                                                             \
    B(3, 1) = invDet * (A(3, 2) * A(2, 1) - A(3, 1) * A(2, 2));                                              \
    B(3, 2) = -invDet * (A(3, 2) * A(1, 1) - A(3, 1) * A(1, 2));                                             \
    B(3, 3) = invDet * (A(2, 2) * A(1, 1) - A(2, 1) * A(1, 2));                                              \
    return 0;                                                                                                \
  }

#define A(y, x) src[(y - 1) * 3 + (x - 1)]
#define B(y, x) dst[(y - 1) * 3 + (x - 1)]
/** inverts the given 3x3 matrix */
generate_mat3inv_body(float, A, B)

    int mat3inv(float *const dst, const float *const src)
{
  return mat3inv_float(dst, src);
}

generate_mat3inv_body(double, A, B)
#undef B
#undef A
#undef generate_mat3inv_body


static void mat3mulv(float *dst, const float *const mat, const float *const v)
{
  for(int k = 0; k < 3; k++)
  {
    float x = 0.0f;
    for(int i = 0; i < 3; i++) x += mat[3 * k + i] * v[i];
    dst[k] = x;
  }
}

static void mat3mul(float *dst, const float *const m1, const float *const m2)
{
  for(int k = 0; k < 3; k++)
  {
    for(int i = 0; i < 3; i++)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++) x += m1[3 * k + j] * m2[3 * j + i];
      dst[3 * k + i] = x;
    }
  }
}

static int dt_colorspaces_get_matrix_from_profile(cmsHPROFILE prof, float *matrix, float *lutr, float *lutg,
                                                  float *lutb, const int lutsize, const int input,
                                                  const int intent)
{
  // create an OpenCL processable matrix + tone curves from an cmsHPROFILE:

  // check this first:
  if(!cmsIsMatrixShaper(prof)) return 1;

  // if this profile contains LUT, it might also contain swapped matrix,
  // so the only right way to handle it is to let LCMS apply it.
  const int UsedDirection = input ? LCMS_USED_AS_INPUT : LCMS_USED_AS_OUTPUT;

  if(cmsIsCLUT(prof, intent, UsedDirection)) return 1;

  cmsToneCurve *red_curve = cmsReadTag(prof, cmsSigRedTRCTag);
  cmsToneCurve *green_curve = cmsReadTag(prof, cmsSigGreenTRCTag);
  cmsToneCurve *blue_curve = cmsReadTag(prof, cmsSigBlueTRCTag);

  cmsCIEXYZ *red_color = cmsReadTag(prof, cmsSigRedColorantTag);
  cmsCIEXYZ *green_color = cmsReadTag(prof, cmsSigGreenColorantTag);
  cmsCIEXYZ *blue_color = cmsReadTag(prof, cmsSigBlueColorantTag);

  if(!red_curve || !green_curve || !blue_curve || !red_color || !green_color || !blue_color) return 2;

  matrix[0] = red_color->X;
  matrix[1] = green_color->X;
  matrix[2] = blue_color->X;
  matrix[3] = red_color->Y;
  matrix[4] = green_color->Y;
  matrix[5] = blue_color->Y;
  matrix[6] = red_color->Z;
  matrix[7] = green_color->Z;
  matrix[8] = blue_color->Z;

  // some camera ICC profiles claim to have color locations for red, green and blue base colors defined,
  // but in fact these are all set to zero. we catch this case here.
  float sum = 0.0f;
  for(int k = 0; k < 9; k++) sum += matrix[k];
  if(sum == 0.0f) return 3;

  if(input)
  {
    // mark as linear, if they are:
    if(cmsIsToneCurveLinear(red_curve))
      lutr[0] = -1.0f;
    else
      for(int k = 0; k < lutsize; k++) lutr[k] = cmsEvalToneCurveFloat(red_curve, k / (lutsize - 1.0f));
    if(cmsIsToneCurveLinear(green_curve))
      lutg[0] = -1.0f;
    else
      for(int k = 0; k < lutsize; k++) lutg[k] = cmsEvalToneCurveFloat(green_curve, k / (lutsize - 1.0f));
    if(cmsIsToneCurveLinear(blue_curve))
      lutb[0] = -1.0f;
    else
      for(int k = 0; k < lutsize; k++) lutb[k] = cmsEvalToneCurveFloat(blue_curve, k / (lutsize - 1.0f));
  }
  else
  {
    // invert profile->XYZ matrix for output profiles
    float tmp[9];
    memcpy(tmp, matrix, sizeof(float) * 9);
    if(mat3inv(matrix, tmp)) return 3;
    // also need to reverse gamma, to apply reverse before matrix multiplication:
    cmsToneCurve *rev_red = cmsReverseToneCurveEx(0x8000, red_curve);
    cmsToneCurve *rev_green = cmsReverseToneCurveEx(0x8000, green_curve);
    cmsToneCurve *rev_blue = cmsReverseToneCurveEx(0x8000, blue_curve);
    if(!rev_red || !rev_green || !rev_blue)
    {
      cmsFreeToneCurve(rev_red);
      cmsFreeToneCurve(rev_green);
      cmsFreeToneCurve(rev_blue);
      return 4;
    }
    // pass on tonecurves, in case lutsize > 0:
    if(cmsIsToneCurveLinear(red_curve))
      lutr[0] = -1.0f;
    else
      for(int k = 0; k < lutsize; k++) lutr[k] = cmsEvalToneCurveFloat(rev_red, k / (lutsize - 1.0f));
    if(cmsIsToneCurveLinear(green_curve))
      lutg[0] = -1.0f;
    else
      for(int k = 0; k < lutsize; k++) lutg[k] = cmsEvalToneCurveFloat(rev_green, k / (lutsize - 1.0f));
    if(cmsIsToneCurveLinear(blue_curve))
      lutb[0] = -1.0f;
    else
      for(int k = 0; k < lutsize; k++) lutb[k] = cmsEvalToneCurveFloat(rev_blue, k / (lutsize - 1.0f));
    cmsFreeToneCurve(rev_red);
    cmsFreeToneCurve(rev_green);
    cmsFreeToneCurve(rev_blue);
  }
  return 0;
}

int dt_colorspaces_get_matrix_from_input_profile(cmsHPROFILE prof, float *matrix, float *lutr, float *lutg,
                                                 float *lutb, const int lutsize, const int intent)
{
  return dt_colorspaces_get_matrix_from_profile(prof, matrix, lutr, lutg, lutb, lutsize, 1, intent);
}

int dt_colorspaces_get_matrix_from_output_profile(cmsHPROFILE prof, float *matrix, float *lutr, float *lutg,
                                                  float *lutb, const int lutsize, const int intent)
{
  return dt_colorspaces_get_matrix_from_profile(prof, matrix, lutr, lutg, lutb, lutsize, 0, intent);
}

cmsHPROFILE dt_colorspaces_create_lab_profile()
{
  return cmsCreateLab4Profile(cmsD50_xyY());
}

#if 0
static cmsHPROFILE _colorspaces_create_srgb_profile(int v4)
{
  cmsHPROFILE hsRGB;

  cmsCIEXYZTRIPLE Colorants = { { 0.436066, 0.222488, 0.013916 },
                                { 0.385147, 0.716873, 0.097076 },
                                { 0.143066, 0.060608, 0.714096 } };

  cmsCIEXYZ black = { 0, 0, 0 };
  cmsCIEXYZ D65 = { 0.95045, 1, 1.08905 };
  cmsToneCurve *transferFunction;

  transferFunction
      = cmsBuildTabulatedToneCurve16(NULL, dt_srgb_tone_curve_values_n, dt_srgb_tone_curve_values);

  hsRGB = cmsCreateProfilePlaceholder(0);

  cmsSetProfileVersion(hsRGB, 2.1);

  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "Public Domain");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "sRGB");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "Darktable");
  cmsMLU *mlu3 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu3, "en", "US", "sRGB");
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hsRGB, cmsSigCopyrightTag, mlu0);
  cmsWriteTag(hsRGB, cmsSigProfileDescriptionTag, mlu1);
  cmsWriteTag(hsRGB, cmsSigDeviceMfgDescTag, mlu2);
  cmsWriteTag(hsRGB, cmsSigDeviceModelDescTag, mlu3);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);
  cmsMLUfree(mlu3);

  cmsSetDeviceClass(hsRGB, cmsSigDisplayClass);
  cmsSetColorSpace(hsRGB, cmsSigRgbData);
  cmsSetPCS(hsRGB, cmsSigXYZData);

  cmsWriteTag(hsRGB, cmsSigMediaWhitePointTag, &D65);
  cmsWriteTag(hsRGB, cmsSigMediaBlackPointTag, &black);

  cmsWriteTag(hsRGB, cmsSigRedColorantTag, (void *)&Colorants.Red);
  cmsWriteTag(hsRGB, cmsSigGreenColorantTag, (void *)&Colorants.Green);
  cmsWriteTag(hsRGB, cmsSigBlueColorantTag, (void *)&Colorants.Blue);

  cmsWriteTag(hsRGB, cmsSigRedTRCTag, (void *)transferFunction);
  cmsLinkTag(hsRGB, cmsSigGreenTRCTag, cmsSigRedTRCTag);
  cmsLinkTag(hsRGB, cmsSigBlueTRCTag, cmsSigRedTRCTag);

  cmsFreeToneCurve(transferFunction);

  return hsRGB;
}
#else
// code partly from elle, see http://ninedegreesbelow.com/photography/lcms-make-icc-profiles.html
static cmsHPROFILE _colorspaces_create_srgb_profile(int v4)
{
  cmsHPROFILE hsRGB;

  cmsCIExyYTRIPLE srgb_primaries_pre_quantized = {
    {0.639998686, 0.330010138, 1.0},
    {0.300003784, 0.600003357, 1.0},
    {0.150002046, 0.059997204, 1.0}
  };
  cmsCIEXYZ black = { 0, 0, 0 };
  cmsCIExyY d65_srgb_adobe_specs = {0.3127, 0.3290, 1.0};

  /* sRGB TRC */
  cmsFloat64Number srgb_parameters[5] = { 2.4, 1.0 / 1.055,  0.055 / 1.055, 1.0 / 12.92, 0.04045 };
  cmsToneCurve *transferFunction = cmsBuildParametricToneCurve(NULL, 4, srgb_parameters);
  cmsToneCurve *srgb_parametric[3] = {transferFunction, transferFunction, transferFunction};

  hsRGB = cmsCreateRGBProfile(&d65_srgb_adobe_specs, &srgb_primaries_pre_quantized, srgb_parametric);

  if(!v4) cmsSetProfileVersion(hsRGB, 2.1);

  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "Public Domain");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "sRGB");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "Darktable");
  cmsMLU *mlu3 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu3, "en", "US", "sRGB");
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hsRGB, cmsSigCopyrightTag, mlu0);
  cmsWriteTag(hsRGB, cmsSigProfileDescriptionTag, mlu1);
  cmsWriteTag(hsRGB, cmsSigDeviceMfgDescTag, mlu2);
  cmsWriteTag(hsRGB, cmsSigDeviceModelDescTag, mlu3);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);
  cmsMLUfree(mlu3);

  cmsSetDeviceClass(hsRGB, cmsSigDisplayClass);
  cmsSetColorSpace(hsRGB, cmsSigRgbData);
  cmsSetPCS(hsRGB, cmsSigXYZData);

  cmsWriteTag(hsRGB, cmsSigMediaBlackPointTag, &black);

  cmsFreeToneCurve(transferFunction);

  return hsRGB;
}

cmsHPROFILE dt_colorspaces_create_srgb_profile()
{
  return _colorspaces_create_srgb_profile(0);
}

cmsHPROFILE dt_colorspaces_create_srgb_profile_v4()
{
  return _colorspaces_create_srgb_profile(1);
}
#endif

// Create the ICC virtual profile for adobe rgb space
cmsHPROFILE dt_colorspaces_create_adobergb_profile(void)
{
  cmsHPROFILE hAdobeRGB;

  cmsCIEXYZTRIPLE Colorants = { { 0.609741, 0.311111, 0.019470 },
                                { 0.205276, 0.625671, 0.060867 },
                                { 0.149185, 0.063217, 0.744568 } };

  cmsCIEXYZ black = { 0, 0, 0 };
  cmsCIEXYZ D65 = { 0.95045, 1, 1.08905 };
  cmsToneCurve *transferFunction;

  // AdobeRGB's "2.2" gamma is technically defined as 2 + 51/256
  transferFunction = cmsBuildGamma(NULL, 2.19921875);

  hAdobeRGB = cmsCreateProfilePlaceholder(0);

  cmsSetProfileVersion(hAdobeRGB, 2.1);

  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "Public Domain");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "Adobe RGB (compatible)");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "Darktable");
  cmsMLU *mlu3 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu3, "en", "US", "Adobe RGB");
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hAdobeRGB, cmsSigCopyrightTag, mlu0);
  cmsWriteTag(hAdobeRGB, cmsSigProfileDescriptionTag, mlu1);
  cmsWriteTag(hAdobeRGB, cmsSigDeviceMfgDescTag, mlu2);
  cmsWriteTag(hAdobeRGB, cmsSigDeviceModelDescTag, mlu3);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);
  cmsMLUfree(mlu3);

  cmsSetDeviceClass(hAdobeRGB, cmsSigDisplayClass);
  cmsSetColorSpace(hAdobeRGB, cmsSigRgbData);
  cmsSetPCS(hAdobeRGB, cmsSigXYZData);

  cmsWriteTag(hAdobeRGB, cmsSigMediaWhitePointTag, &D65);
  cmsWriteTag(hAdobeRGB, cmsSigMediaBlackPointTag, &black);

  cmsWriteTag(hAdobeRGB, cmsSigRedColorantTag, (void *)&Colorants.Red);
  cmsWriteTag(hAdobeRGB, cmsSigGreenColorantTag, (void *)&Colorants.Green);
  cmsWriteTag(hAdobeRGB, cmsSigBlueColorantTag, (void *)&Colorants.Blue);

  cmsWriteTag(hAdobeRGB, cmsSigRedTRCTag, (void *)transferFunction);
  cmsLinkTag(hAdobeRGB, cmsSigGreenTRCTag, cmsSigRedTRCTag);
  cmsLinkTag(hAdobeRGB, cmsSigBlueTRCTag, cmsSigRedTRCTag);

  cmsFreeToneCurve(transferFunction);

  return hAdobeRGB;
}

static cmsToneCurve *build_linear_gamma(void)
{
  double Parameters[2];

  Parameters[0] = 1.0;
  Parameters[1] = 0;

  return cmsBuildParametricToneCurve(0, 1, Parameters);
}

static float cbrt_5f(float f)
{
  uint32_t *p = (uint32_t *)&f;
  *p = *p / 3 + 709921077;
  return f;
}

static float cbrta_halleyf(const float a, const float R)
{
  const float a3 = a * a * a;
  const float b = a * (a3 + R + R) / (a3 + a3 + R);
  return b;
}

static float lab_f(const float x)
{
  const float epsilon = 216.0f / 24389.0f;
  const float kappa = 24389.0f / 27.0f;
  if(x > epsilon)
  {
    // approximate cbrtf(x):
    const float a = cbrt_5f(x);
    return cbrta_halleyf(a, x);
  }
  else
    return (kappa * x + 16.0f) / 116.0f;
}

void dt_XYZ_to_Lab(const float *XYZ, float *Lab)
{
  const float d50[3] = { 0.9642, 1.0, 0.8249 };
  const float f[3] = { lab_f(XYZ[0] / d50[0]), lab_f(XYZ[1] / d50[1]), lab_f(XYZ[2] / d50[2]) };
  Lab[0] = 116.0f * f[1] - 16.0f;
  Lab[1] = 500.0f * (f[0] - f[1]);
  Lab[2] = 200.0f * (f[1] - f[2]);
}

static float lab_f_inv(const float x)
{
  const float epsilon = 0.20689655172413796; // cbrtf(216.0f/24389.0f);
  const float kappa = 24389.0f / 27.0f;
  if(x > epsilon)
    return x * x * x;
  else
    return (116.0f * x - 16.0f) / kappa;
}

void dt_Lab_to_XYZ(const float *Lab, float *XYZ)
{
  const float d50[3] = { 0.9642, 1.0, 0.8249 };
  const float fy = (Lab[0] + 16.0f) / 116.0f;
  const float fx = Lab[1] / 500.0f + fy;
  const float fz = fy - Lab[2] / 200.0f;
  XYZ[0] = d50[0] * lab_f_inv(fx);
  XYZ[1] = d50[1] * lab_f_inv(fy);
  XYZ[2] = d50[2] * lab_f_inv(fz);
}


int dt_colorspaces_get_darktable_matrix(const char *makermodel, float *matrix)
{
  dt_profiled_colormatrix_t *preset = NULL;
  for(int k = 0; k < dt_profiled_colormatrix_cnt; k++)
  {
    if(!strcasecmp(makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      preset = dt_profiled_colormatrices + k;
      break;
    }
  }
  if(!preset) return -1;

  const float wxyz = preset->white[0] + preset->white[1] + preset->white[2];
  const float rxyz = preset->rXYZ[0] + preset->rXYZ[1] + preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] + preset->gXYZ[1] + preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] + preset->bXYZ[1] + preset->bXYZ[2];

  const float xn = preset->white[0] / wxyz;
  const float yn = preset->white[1] / wxyz;
  const float xr = preset->rXYZ[0] / rxyz;
  const float yr = preset->rXYZ[1] / rxyz;
  const float xg = preset->gXYZ[0] / gxyz;
  const float yg = preset->gXYZ[1] / gxyz;
  const float xb = preset->bXYZ[0] / bxyz;
  const float yb = preset->bXYZ[1] / bxyz;

  const float primaries[9] = { xr, xg, xb, yr, yg, yb, 1.0f - xr - yr, 1.0f - xg - yg, 1.0f - xb - yb };

  float result[9];
  if(mat3inv(result, primaries)) return -1;

  const float whitepoint[3] = { xn / yn, 1.0f, (1.0f - xn - yn) / yn };
  float coeff[3];

  // get inverse primary whitepoint
  mat3mulv(coeff, result, whitepoint);


  float tmp[9] = { coeff[0] * xr, coeff[1] * xg, coeff[2] * xb, coeff[0] * yr, coeff[1] * yg, coeff[2] * yb,
                   coeff[0] * (1.0f - xr - yr), coeff[1] * (1.0f - xg - yg), coeff[2] * (1.0f - xb - yb) };

  // input whitepoint[] in XYZ with Y normalized to 1.0f
  const float dn[3]
      = { preset->white[0] / (float)preset->white[1], 1.0f, preset->white[2] / (float)preset->white[1] };
  const float lam_rigg[9] = { 0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296 };
  const float d50[3] = { 0.9642, 1.0, 0.8249 };


  // adapt to d50
  float chad_inv[9];
  if(mat3inv(chad_inv, lam_rigg)) return -1;

  float cone_src_rgb[3], cone_dst_rgb[3];
  mat3mulv(cone_src_rgb, lam_rigg, dn);
  mat3mulv(cone_dst_rgb, lam_rigg, d50);

  const float cone[9]
      = { cone_dst_rgb[0] / cone_src_rgb[0], 0.0f, 0.0f, 0.0f, cone_dst_rgb[1] / cone_src_rgb[1], 0.0f, 0.0f,
          0.0f, cone_dst_rgb[2] / cone_src_rgb[2] };

  float tmp2[9];
  float bradford[9];
  mat3mul(tmp2, cone, lam_rigg);
  mat3mul(bradford, chad_inv, tmp2);

  mat3mul(matrix, bradford, tmp);
  return 0;
}

cmsHPROFILE dt_colorspaces_create_alternate_profile(const char *makermodel)
{
  dt_profiled_colormatrix_t *preset = NULL;
  for(int k = 0; k < dt_alternate_colormatrix_cnt; k++)
  {
    if(!strcmp(makermodel, dt_alternate_colormatrices[k].makermodel))
    {
      preset = dt_alternate_colormatrices + k;
      break;
    }
  }
  if(!preset) return NULL;

  const float wxyz = preset->white[0] + preset->white[1] + preset->white[2];
  const float rxyz = preset->rXYZ[0] + preset->rXYZ[1] + preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] + preset->gXYZ[1] + preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] + preset->bXYZ[1] + preset->bXYZ[2];
  cmsCIExyY WP = { preset->white[0] / wxyz, preset->white[1] / wxyz, 1.0 };
  cmsCIExyYTRIPLE XYZPrimaries = { { preset->rXYZ[0] / rxyz, preset->rXYZ[1] / rxyz, 1.0 },
                                   { preset->gXYZ[0] / gxyz, preset->gXYZ[1] / gxyz, 1.0 },
                                   { preset->bXYZ[0] / bxyz, preset->bXYZ[1] / bxyz, 1.0 } };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE hp;

  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();

  hp = cmsCreateRGBProfile(&WP, &XYZPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(hp == NULL) return NULL;

  char name[512];
  snprintf(name, sizeof(name), "darktable alternate %s", makermodel);
  cmsSetProfileVersion(hp, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", name);
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", name);
  cmsWriteTag(hp, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hp, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hp, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hp;
}

cmsHPROFILE dt_colorspaces_create_vendor_profile(const char *makermodel)
{
  dt_profiled_colormatrix_t *preset = NULL;
  for(int k = 0; k < dt_vendor_colormatrix_cnt; k++)
  {
    if(!strcmp(makermodel, dt_vendor_colormatrices[k].makermodel))
    {
      preset = dt_vendor_colormatrices + k;
      break;
    }
  }
  if(!preset) return NULL;

  const float wxyz = preset->white[0] + preset->white[1] + preset->white[2];
  const float rxyz = preset->rXYZ[0] + preset->rXYZ[1] + preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] + preset->gXYZ[1] + preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] + preset->bXYZ[1] + preset->bXYZ[2];
  cmsCIExyY WP = { preset->white[0] / wxyz, preset->white[1] / wxyz, 1.0 };
  cmsCIExyYTRIPLE XYZPrimaries = { { preset->rXYZ[0] / rxyz, preset->rXYZ[1] / rxyz, 1.0 },
                                   { preset->gXYZ[0] / gxyz, preset->gXYZ[1] / gxyz, 1.0 },
                                   { preset->bXYZ[0] / bxyz, preset->bXYZ[1] / bxyz, 1.0 } };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE hp;

  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();

  hp = cmsCreateRGBProfile(&WP, &XYZPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(hp == NULL) return NULL;

  char name[512];
  snprintf(name, sizeof(name), "darktable vendor %s", makermodel);
  cmsSetProfileVersion(hp, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", name);
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", name);
  cmsWriteTag(hp, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hp, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hp, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hp;
}

cmsHPROFILE dt_colorspaces_create_darktable_profile(const char *makermodel)
{
  dt_profiled_colormatrix_t *preset = NULL;
  for(int k = 0; k < dt_profiled_colormatrix_cnt; k++)
  {
    if(!strcasecmp(makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      preset = dt_profiled_colormatrices + k;
      break;
    }
  }
  if(!preset) return NULL;

  const float wxyz = preset->white[0] + preset->white[1] + preset->white[2];
  const float rxyz = preset->rXYZ[0] + preset->rXYZ[1] + preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] + preset->gXYZ[1] + preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] + preset->bXYZ[1] + preset->bXYZ[2];
  cmsCIExyY WP = { preset->white[0] / wxyz, preset->white[1] / wxyz, 1.0 };
  cmsCIExyYTRIPLE XYZPrimaries = { { preset->rXYZ[0] / rxyz, preset->rXYZ[1] / rxyz, 1.0 },
                                   { preset->gXYZ[0] / gxyz, preset->gXYZ[1] / gxyz, 1.0 },
                                   { preset->bXYZ[0] / bxyz, preset->bXYZ[1] / bxyz, 1.0 } };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE hp;

  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();

  hp = cmsCreateRGBProfile(&WP, &XYZPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(hp == NULL) return NULL;

  char name[512];
  snprintf(name, sizeof(name), "Darktable profiled %s", makermodel);
  cmsSetProfileVersion(hp, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", name);
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", name);
  cmsWriteTag(hp, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hp, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hp, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hp;
}

cmsHPROFILE dt_colorspaces_create_xyz_profile(void)
{
  cmsHPROFILE hXYZ = cmsCreateXYZProfile();
  // revert some settings which prevent us from using XYZ as output profile:
  cmsSetDeviceClass(hXYZ, cmsSigDisplayClass);
  cmsSetColorSpace(hXYZ, cmsSigRgbData);
  cmsSetPCS(hXYZ, cmsSigXYZData);
  cmsSetHeaderRenderingIntent(hXYZ, INTENT_PERCEPTUAL);

  if(hXYZ == NULL) return NULL;

  cmsSetProfileVersion(hXYZ, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "linear XYZ");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "Darktable linear XYZ");
  cmsWriteTag(hXYZ, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hXYZ, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hXYZ, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hXYZ;
}

cmsHPROFILE dt_colorspaces_create_linear_rec709_rgb_profile(void)
{
  cmsHPROFILE hRec709RGB;

  cmsCIEXYZTRIPLE Colorants = { { 0.436066, 0.222488, 0.013916 },
                                { 0.385147, 0.716873, 0.097076 },
                                { 0.143066, 0.060608, 0.714096 } };

  cmsCIEXYZ black = { 0, 0, 0 };
  cmsCIEXYZ D65 = { 0.95045, 1, 1.08905 };
  cmsToneCurve *transferFunction;

  transferFunction = build_linear_gamma();

  hRec709RGB = cmsCreateProfilePlaceholder(0);

  cmsSetProfileVersion(hRec709RGB, 2.1);

  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "Public Domain");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "Linear Rec709 RGB");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "Darktable");
  cmsMLU *mlu3 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu3, "en", "US", "Linear Rec709 RGB");
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hRec709RGB, cmsSigCopyrightTag, mlu0);
  cmsWriteTag(hRec709RGB, cmsSigProfileDescriptionTag, mlu1);
  cmsWriteTag(hRec709RGB, cmsSigDeviceMfgDescTag, mlu2);
  cmsWriteTag(hRec709RGB, cmsSigDeviceModelDescTag, mlu3);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);
  cmsMLUfree(mlu3);

  cmsSetDeviceClass(hRec709RGB, cmsSigDisplayClass);
  cmsSetColorSpace(hRec709RGB, cmsSigRgbData);
  cmsSetPCS(hRec709RGB, cmsSigXYZData);

  cmsWriteTag(hRec709RGB, cmsSigMediaWhitePointTag, &D65);
  cmsWriteTag(hRec709RGB, cmsSigMediaBlackPointTag, &black);

  cmsWriteTag(hRec709RGB, cmsSigRedColorantTag, (void *)&Colorants.Red);
  cmsWriteTag(hRec709RGB, cmsSigGreenColorantTag, (void *)&Colorants.Green);
  cmsWriteTag(hRec709RGB, cmsSigBlueColorantTag, (void *)&Colorants.Blue);

  cmsWriteTag(hRec709RGB, cmsSigRedTRCTag, (void *)transferFunction);
  cmsLinkTag(hRec709RGB, cmsSigGreenTRCTag, cmsSigRedTRCTag);
  cmsLinkTag(hRec709RGB, cmsSigBlueTRCTag, cmsSigRedTRCTag);

  cmsFreeToneCurve(transferFunction);

  return hRec709RGB;
}

cmsHPROFILE dt_colorspaces_create_linear_rec2020_rgb_profile(void)
{
  cmsHPROFILE hRec2020RGB;

  cmsCIEXYZTRIPLE Colorants = { { 0.673492, 0.279037, -0.001938 },
                                { 0.165665, 0.675354, 0.029984 },
                                { 0.125046, 0.045609, 0.796860 } };

  cmsCIEXYZ black = { 0, 0, 0 };
  cmsCIEXYZ D65 = { 0.95045, 1, 1.08905 };
  cmsToneCurve *transferFunction;

  transferFunction = build_linear_gamma();

  hRec2020RGB = cmsCreateProfilePlaceholder(0);

  cmsSetProfileVersion(hRec2020RGB, 2.1);

  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "Public Domain");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "Linear Rec2020 RGB");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "Darktable");
  cmsMLU *mlu3 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu3, "en", "US", "Linear Rec2020 RGB");
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hRec2020RGB, cmsSigCopyrightTag, mlu0);
  cmsWriteTag(hRec2020RGB, cmsSigProfileDescriptionTag, mlu1);
  cmsWriteTag(hRec2020RGB, cmsSigDeviceMfgDescTag, mlu2);
  cmsWriteTag(hRec2020RGB, cmsSigDeviceModelDescTag, mlu3);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);
  cmsMLUfree(mlu3);

  cmsSetDeviceClass(hRec2020RGB, cmsSigDisplayClass);
  cmsSetColorSpace(hRec2020RGB, cmsSigRgbData);
  cmsSetPCS(hRec2020RGB, cmsSigXYZData);

  cmsWriteTag(hRec2020RGB, cmsSigMediaWhitePointTag, &D65);
  cmsWriteTag(hRec2020RGB, cmsSigMediaBlackPointTag, &black);

  cmsWriteTag(hRec2020RGB, cmsSigRedColorantTag, (void *)&Colorants.Red);
  cmsWriteTag(hRec2020RGB, cmsSigGreenColorantTag, (void *)&Colorants.Green);
  cmsWriteTag(hRec2020RGB, cmsSigBlueColorantTag, (void *)&Colorants.Blue);

  cmsWriteTag(hRec2020RGB, cmsSigRedTRCTag, (void *)transferFunction);
  cmsLinkTag(hRec2020RGB, cmsSigGreenTRCTag, cmsSigRedTRCTag);
  cmsLinkTag(hRec2020RGB, cmsSigBlueTRCTag, cmsSigRedTRCTag);

  cmsFreeToneCurve(transferFunction);

  return hRec2020RGB;
}

cmsHPROFILE dt_colorspaces_create_linear_infrared_profile(void)
{
  // linear rgb with r and b swapped:
  cmsCIExyY D65;
  cmsCIExyYTRIPLE Rec709Primaries
      = { { 0.1500, 0.0600, 1.0 }, { 0.3000, 0.6000, 1.0 }, { 0.6400, 0.3300, 1.0 } };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE hsRGB;

  cmsWhitePointFromTemp(&D65, 6504.0);
  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();

  hsRGB = cmsCreateRGBProfile(&D65, &Rec709Primaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(hsRGB == NULL) return NULL;

  cmsSetProfileVersion(hsRGB, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "linear infrared bgr");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "Darktable Linear Infrared BGR");
  cmsWriteTag(hsRGB, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hsRGB, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hsRGB, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hsRGB;
}

int dt_colorspaces_find_profile(char *filename, size_t filename_len, const char *profile, const char *inout)
{
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(datadir, sizeof(datadir));
  snprintf(filename, filename_len, "%s/color/%s/%s", datadir, inout, profile);
  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
  {
    dt_loc_get_datadir(datadir, sizeof(datadir));
    snprintf(filename, filename_len, "%s/color/%s/%s", datadir, inout, profile);
    if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR)) return 1;
  }
  return 0;
}

const dt_colorspaces_color_profile_t *dt_colorspaces_get_output_profile(const int imgid)
{
  // find the colorout module -- the pointer stays valid until darktable shuts down
  static dt_iop_module_so_t *colorout = NULL;
  if(colorout == NULL)
  {
    GList *modules = g_list_first(darktable.iop);
    while(modules)
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules->data);
      if(!strcmp(module->op, "colorout"))
      {
        colorout = module;
        break;
      }
      modules = g_list_next(modules);
    }
  }

  const dt_colorspaces_color_profile_t *p = NULL;

  int over_type = dt_conf_get_int("plugins/lighttable/export/icctype");
  gchar *over_filename = dt_conf_get_string("plugins/lighttable/export/iccprofile");

  if(over_type != DT_COLORSPACE_NONE)
  {
    // return the profile specified in export
    p = dt_colorspaces_get_profile(over_type, over_filename, DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY);

    // the profile asked for doesn't exist, fall back to sRGB
    if(!p) p = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_OUT);
  }
  else
  {
    // get the profile assigned from colorout
    // FIXME: does this work when using JPEG thumbs and the image was never opened?
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT op_params FROM history WHERE imgid=?1 AND operation='colorout' ORDER BY num DESC LIMIT 1", -1,
      &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      // use introspection to get the profile name from the binary params blob
      const void *params = sqlite3_column_blob(stmt, 0);
      dt_colorspaces_color_profile_type_t *type = colorout->get_p(params, "type");
      char *filename = colorout->get_p(params, "filename");

      if(type && filename) p = dt_colorspaces_get_profile(*type, filename,
                                                          DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY);
    }

    // couldn't get it from colorout -> fall back to sRGB
    if(!p) p = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_OUT);
  }

  return p;
}

void dt_colorspaces_create_cmatrix(float cmatrix[4][3], float mat[3][3])
{
  // sRGB D65, the linear part:
  const float rgb_to_xyz[3][3] = { { 0.4124564, 0.3575761, 0.1804375 },
                                   { 0.2126729, 0.7151522, 0.0721750 },
                                   { 0.0193339, 0.1191920, 0.9503041 } };

  for(int c = 0; c < 3; c++)
  {
    for(int j = 0; j < 3; j++)
    {
      mat[c][j] = 0.0f;
      for(int k = 0; k < 3; k++)
      {
        mat[c][j] += rgb_to_xyz[k][j] * cmatrix[c][k];
      }
    }
  }
}

cmsHPROFILE dt_colorspaces_create_xyzimatrix_profile(float mat[3][3])
{
  // mat: xyz -> cam
  float imat[3][3];
  mat3inv((float *)imat, (float *)mat);
  return dt_colorspaces_create_xyzmatrix_profile(imat);
}

cmsHPROFILE dt_colorspaces_create_xyzmatrix_profile(float mat[3][3])
{
  // mat: cam -> xyz
  cmsCIExyY D65;
  float x[3], y[3];
  for(int k = 0; k < 3; k++)
  {
    const float norm = mat[0][k] + mat[1][k] + mat[2][k];
    x[k] = mat[0][k] / norm;
    y[k] = mat[1][k] / norm;
  }
  cmsCIExyYTRIPLE CameraPrimaries = { { x[0], y[0], 1.0 }, { x[1], y[1], 1.0 }, { x[2], y[2], 1.0 } };
  cmsHPROFILE cmat;

  cmsWhitePointFromTemp(&D65, 6504.0);

  cmsToneCurve *Gamma[3];
  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();
  cmat = cmsCreateRGBProfile(&D65, &CameraPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(cmat == NULL) return NULL;

  cmsSetProfileVersion(cmat, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "color matrix built-in");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "color matrix built-in");
  cmsWriteTag(cmat, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(cmat, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(cmat, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return cmat;
}

void dt_colorspaces_cleanup_profile(cmsHPROFILE p)
{
  if(!p) return;
  cmsCloseProfile(p);
}

void dt_colorspaces_get_profile_name(cmsHPROFILE p, const char *language, const char *country, char *name,
                                     size_t len)
{
  cmsUInt32Number size;
  gchar *buf = NULL;
  wchar_t *wbuf = NULL;
  gchar *utf8 = NULL;

  size = cmsGetProfileInfoASCII(p, cmsInfoDescription, language, country, NULL, 0);
  if(size == 0) goto error;

  buf = (char *)calloc(size + 1, sizeof(char));
  size = cmsGetProfileInfoASCII(p, cmsInfoDescription, language, country, buf, size);
  if(size == 0) goto error;

  // most unix like systems should work with this, but at least Windows doesn't
  if(sizeof(wchar_t) != 4 || g_utf8_validate(buf, -1, NULL))
    g_strlcpy(name, buf, len); // better a little weird than totally borked
  else
  {
    wbuf = (wchar_t *)calloc(size + 1, sizeof(wchar_t));
    size = cmsGetProfileInfo(p, cmsInfoDescription, language, country, wbuf, sizeof(wchar_t) * size);
    if(size == 0) goto error;
    utf8 = g_ucs4_to_utf8((gunichar *)wbuf, -1, NULL, NULL, NULL);
    if(!utf8) goto error;
    g_strlcpy(name, utf8, len);
  }

  free(buf);
  free(wbuf);
  g_free(utf8);
  return;

error:
  if(buf)
    g_strlcpy(name, buf, len); // better a little weird than totally borked
  else
    *name = '\0'; // nothing to do here
  free(buf);
  free(wbuf);
  g_free(utf8);
}

void rgb2hsl(const float rgb[3], float *h, float *s, float *l)
{
  const float r = rgb[0], g = rgb[1], b = rgb[2];
  float pmax = fmax(r, fmax(g, b));
  float pmin = fmin(r, fmin(g, b));
  float delta = (pmax - pmin);

  float hv = 0, sv = 0, lv = (pmin + pmax) / 2.0;

  if(pmax != pmin)
  {
    sv = lv < 0.5 ? delta / (pmax + pmin) : delta / (2.0 - pmax - pmin);

    if(pmax == r)
      hv = (g - b) / delta;
    else if(pmax == g)
      hv = 2.0 + (b - r) / delta;
    else if(pmax == b)
      hv = 4.0 + (r - g) / delta;
    hv /= 6.0;
    if(hv < 0.0)
      hv += 1.0;
    else if(hv > 1.0)
      hv -= 1.0;
  }
  *h = hv;
  *s = sv;
  *l = lv;
}

static inline float hue2rgb(float m1, float m2, float hue)
{
  if(hue < 0.0)
    hue += 1.0;
  else if(hue > 1.0)
    hue -= 1.0;

  if(hue < 1.0 / 6.0)
    return (m1 + (m2 - m1) * hue * 6.0);
  else if(hue < 1.0 / 2.0)
    return m2;
  else if(hue < 2.0 / 3.0)
    return (m1 + (m2 - m1) * ((2.0 / 3.0) - hue) * 6.0);
  else
    return m1;
}

void hsl2rgb(float rgb[3], float h, float s, float l)
{
  float m1, m2;
  if(s == 0)
  {
    rgb[0] = rgb[1] = rgb[2] = l;
    return;
  }
  m2 = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
  m1 = (2.0 * l - m2);
  rgb[0] = hue2rgb(m1, m2, h + (1.0 / 3.0));
  rgb[1] = hue2rgb(m1, m2, h);
  rgb[2] = hue2rgb(m1, m2, h - (1.0 / 3.0));
}

static const char *_profile_names[] =
{
  "", // 0th entry is a dummy for DT_COLORSPACE_FILE and not used
  N_("sRGB"), // this is only used in error messages, no need for the (...) description
  N_("Adobe RGB (compatible)"),
  N_("linear Rec709 RGB"),
  N_("linear Rec2020 RGB"),
  N_("linear XYZ"),
  N_("Lab"),
  N_("linear infrared BGR"),
  N_("system display profile"),
  N_("embedded ICC profile"),
  N_("embedded matrix"),
  N_("standard color matrix"),
  N_("enhanced color matrix"),
  N_("vendor color matrix"),
  N_("alternate color matrix")
};

static dt_colorspaces_color_profile_t *_create_profile(dt_colorspaces_color_profile_type_t type,
                                                       cmsHPROFILE profile, const char *name,
                                                       int in_pos, int out_pos, int display_pos)
{
  dt_colorspaces_color_profile_t *prof;
  prof = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
  prof->type = type;
  g_strlcpy(prof->name, name, sizeof(prof->name));
  prof->profile = profile;
  prof->in_pos = in_pos;
  prof->out_pos = out_pos;
  prof->display_pos = display_pos;
  return prof;
}

dt_colorspaces_t *dt_colorspaces_init()
{
  dt_colorspaces_t *res = (dt_colorspaces_t *)calloc(1, sizeof(dt_colorspaces_t));

  pthread_rwlock_init(&res->xprofile_lock, NULL);

  int in_pos = -1,
      out_pos = -1,
      display_pos = -1;
  cmsHPROFILE tmpprof;
  const gchar *d_name;
  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  char dirname[PATH_MAX] = { 0 };
  char filename[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));
  GDir *dir;
  char *lang = getenv("LANG");
  if(!lang) lang = "en_US";

  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_DISPLAY, NULL,
                                                               _("system display profile"), -1, -1, ++display_pos));
  // we want a v4 with parametric curve for input and a v2 with point trc for output
  // see http://ninedegreesbelow.com/photography/lcms-make-icc-profiles.html#profile-variants-and-versions
  // TODO: what about display?
  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_SRGB,
                                                               dt_colorspaces_create_srgb_profile_v4(),
                                                               _("sRGB (e.g. JPG)"),
                                                               ++in_pos, -1, -1));

  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_SRGB,
                                                               dt_colorspaces_create_srgb_profile(),
                                                               _("sRGB (web-safe)"),
                                                               -1, ++out_pos, ++display_pos));

  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_ADOBERGB,
                                                               dt_colorspaces_create_adobergb_profile(),
                                                               _("Adobe RGB (compatible)"),
                                                               ++in_pos, ++out_pos, ++display_pos));

  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_LIN_REC709,
                                                               dt_colorspaces_create_linear_rec709_rgb_profile(),
                                                               _("linear Rec709 RGB"),
                                                               ++in_pos, ++out_pos, ++display_pos));

  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_LIN_REC2020,
                                                               dt_colorspaces_create_linear_rec2020_rgb_profile(),
                                                               _("linear Rec2020 RGB"),
                                                               ++in_pos, ++out_pos, ++display_pos));

  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_XYZ,
                                                               dt_colorspaces_create_xyz_profile(),
                                                               _("linear XYZ"),
                                                               ++in_pos, -1, -1));

  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_LAB,
                                                               dt_colorspaces_create_lab_profile(),
                                                               _("Lab"),
                                                               ++in_pos, -1, -1));

  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_INFRARED,
                                                               dt_colorspaces_create_linear_infrared_profile(),
                                                               _("linear infrared BGR"),
                                                               ++in_pos, -1, -1));

  // read {userconfig,datadir}/color/in/*.icc, in this order.
  snprintf(dirname, sizeof(dirname), "%s/color/in", confdir);
  if(!g_file_test(dirname, G_FILE_TEST_IS_DIR)) snprintf(dirname, sizeof(dirname), "%s/color/in", datadir);
  dir = g_dir_open(dirname, 0, NULL);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      snprintf(filename, sizeof(filename), "%s/%s", dirname, d_name);
      tmpprof = cmsOpenProfileFromFile(filename, "r");
      if(tmpprof)
      {
        dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
        dt_colorspaces_get_profile_name(tmpprof, lang, lang + 3, prof->name, sizeof(prof->name));

        g_strlcpy(prof->filename, d_name, sizeof(prof->filename));
        prof->type = DT_COLORSPACE_FILE;
        prof->profile = tmpprof;
        prof->in_pos = ++in_pos;
        prof->out_pos = -1;
        prof->display_pos = -1;
        res->profiles = g_list_append(res->profiles, prof);
      }
    }
    g_dir_close(dir);
  }

  // read {conf,data}dir/color/out/*.icc
  snprintf(dirname, sizeof(dirname), "%s/color/out", confdir);
  if(!g_file_test(dirname, G_FILE_TEST_IS_DIR)) snprintf(dirname, sizeof(dirname), "%s/color/out", datadir);
  dir = g_dir_open(dirname, 0, NULL);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      snprintf(filename, sizeof(filename), "%s/%s", dirname, d_name);
      tmpprof = cmsOpenProfileFromFile(filename, "r");
      if(tmpprof)
      {
        dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
        dt_colorspaces_get_profile_name(tmpprof, lang, lang + 3, prof->name, sizeof(prof->name));

        g_strlcpy(prof->filename, d_name, sizeof(prof->filename));
        prof->type = DT_COLORSPACE_FILE;
        prof->profile = tmpprof;
        prof->in_pos = -1;
        prof->out_pos = ++out_pos;
        prof->display_pos = ++display_pos;
        res->profiles = g_list_append(res->profiles, prof);
      }
    }
    g_dir_close(dir);
  }

  res->profiles = g_list_first(res->profiles);

  // init display profile and softproof/gama checking from conf
  res->display_type = dt_conf_get_int("ui_last/color/display_type");
  res->softproof_type = dt_conf_get_int("ui_last/color/softproof_type");
  char *tmp = dt_conf_get_string("ui_last/color/display_filename");
  g_strlcpy(res->display_filename, tmp, sizeof(res->display_filename));
  g_free(tmp);
  tmp = dt_conf_get_string("ui_last/color/softproof_filename");
  g_strlcpy(res->softproof_filename, tmp, sizeof(res->softproof_filename));
  g_free(tmp);
  res->display_intent = dt_conf_get_int("ui_last/color/display_intent");
  res->softproof_intent = dt_conf_get_int("ui_last/color/softproof_intent");
  res->mode = dt_conf_get_int("ui_last/color/mode");
  if((unsigned int)res->display_type >= DT_COLORSPACE_LAST
    || (res->display_type == DT_COLORSPACE_FILE
        && !res->display_filename[0]))
    res->display_type = DT_COLORSPACE_DISPLAY;
  if((unsigned int)res->softproof_type >= DT_COLORSPACE_LAST
    || (res->softproof_type == DT_COLORSPACE_FILE
        && !res->softproof_filename[0]))
    res->softproof_type = DT_COLORSPACE_SRGB;
  if((unsigned int)res->mode > DT_PROFILE_GAMUTCHECK) res->mode = DT_PROFILE_NORMAL;

  return res;
}

void dt_colorspaces_cleanup(dt_colorspaces_t *self)
{
  // remember display profile and softproof/gama checking from conf
  dt_conf_set_int("ui_last/color/display_type", self->display_type);
  dt_conf_set_int("ui_last/color/softproof_type", self->softproof_type);
  dt_conf_set_string("ui_last/color/display_filename", self->display_filename);
  dt_conf_set_string("ui_last/color/softproof_filename", self->softproof_filename);
  dt_conf_set_int("ui_last/color/display_intent", self->display_intent);
  dt_conf_set_int("ui_last/color/softproof_intent", self->softproof_intent);
  dt_conf_set_int("ui_last/color/mode", self->mode);

  for(GList *iter = self->profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *p = (dt_colorspaces_color_profile_t *)iter->data;
    dt_colorspaces_cleanup_profile(p->profile);
  }
  g_list_free_full(self->profiles, free);

  pthread_rwlock_destroy(&self->xprofile_lock);
  g_free(self->colord_profile_file);
  g_free(self->xprofile_data);
  free(self);
}

const char *dt_colorspaces_get_name(dt_colorspaces_color_profile_type_t type, const char *filename)
{
  if(type == DT_COLORSPACE_NONE)
    return NULL;
  else if(type != DT_COLORSPACE_FILE)
    return _(_profile_names[type]);
  else
    return filename;
}

#ifdef USE_COLORDGTK
static void dt_colorspaces_get_display_profile_colord_callback(GObject *source, GAsyncResult *res, gpointer user_data)
{
  pthread_rwlock_wrlock(&darktable.color_profiles->xprofile_lock);

  int profile_changed = 0;
  CdWindow *window = CD_WINDOW(source);
  GError *error = NULL;
  CdProfile *profile = cd_window_get_profile_finish(window, res, &error);
  if(error == NULL && profile != NULL)
  {
    const gchar *filename = cd_profile_get_filename(profile);
    if(filename)
    {
      if(g_strcmp0(filename, darktable.color_profiles->colord_profile_file))
      {
        /* the profile has changed (either because the user changed the colord settings or because we are on a
         * different screen now) */
        // update darktable.color_profiles->colord_profile_file
        g_free(darktable.color_profiles->colord_profile_file);
        darktable.color_profiles->colord_profile_file = g_strdup(filename);
        // read the file
        guchar *tmp_data = NULL;
        gsize size;
        g_file_get_contents(filename, (gchar **)&tmp_data, &size, NULL);
        profile_changed = size > 0 && (darktable.color_profiles->xprofile_size != size
                          || memcmp(darktable.color_profiles->xprofile_data, tmp_data, size) != 0);
        if(profile_changed)
        {
          g_free(darktable.color_profiles->xprofile_data);
          darktable.color_profiles->xprofile_data = tmp_data;
          darktable.color_profiles->xprofile_size = size;
          for(GList *iter = darktable.color_profiles->profiles; iter; iter = g_list_next(iter))
          {
            dt_colorspaces_color_profile_t *p = (dt_colorspaces_color_profile_t *)iter->data;
            if(p->type == DT_COLORSPACE_DISPLAY)
            {
              if(p->profile) dt_colorspaces_cleanup_profile(p->profile);
              p->profile = cmsOpenProfileFromMem(tmp_data, size);
              break;
            }
          }
          dt_print(DT_DEBUG_CONTROL,
                   "[color profile] colord gave us a new screen profile: '%s' (size: %ld)\n", filename, size);
        }
        else
        {
          g_free(tmp_data);
        }
      }
    }
  }
  if(profile) g_object_unref(profile);
  g_object_unref(window);

  pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
  if(profile_changed) dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_CHANGED);
}
#endif

// Get the display ICC profile of the monitor associated with the widget.
// For X display, uses the ICC profile specifications version 0.2 from
// http://burtonini.com/blog/computers/xicc
// Based on code from Gimp's modules/cdisplay_lcms.c
void dt_colorspaces_set_display_profile()
{
  if(!dt_control_running()) return;
  // make sure that no one gets a broken profile
  // FIXME: benchmark if the try is really needed when moving/resizing the window. Maybe we can just lock it
  // and block
  if(pthread_rwlock_trywrlock(&darktable.color_profiles->xprofile_lock))
    return; // we are already updating the profile. Or someone is reading right now. Too bad we can't
            // distinguish that. Whatever ...

  GtkWidget *widget = dt_ui_center(darktable.gui->ui);
  guint8 *buffer = NULL;
  gint buffer_size = 0;
  gchar *profile_source = NULL;

#if defined GDK_WINDOWING_X11

  // we will use the xatom no matter what configured when compiled without colord
  gboolean use_xatom = TRUE;
#if defined USE_COLORDGTK
  gboolean use_colord = TRUE;
  gchar *display_profile_source = dt_conf_get_string("ui_last/display_profile_source");
  if(display_profile_source)
  {
    if(!strcmp(display_profile_source, "xatom"))
      use_colord = FALSE;
    else if(!strcmp(display_profile_source, "colord"))
      use_xatom = FALSE;
    g_free(display_profile_source);
  }
#endif

  /* let's have a look at the xatom, just in case ... */
  if(use_xatom)
  {
    GdkScreen *screen = gtk_widget_get_screen(widget);
    if(screen == NULL) screen = gdk_screen_get_default();
    int monitor = gdk_screen_get_monitor_at_window(screen, gtk_widget_get_window(widget));
    char *atom_name;
    if(monitor > 0)
      atom_name = g_strdup_printf("_ICC_PROFILE_%d", monitor);
    else
      atom_name = g_strdup("_ICC_PROFILE");

    profile_source = g_strdup_printf("xatom %s", atom_name);

    GdkAtom type = GDK_NONE;
    gint format = 0;
    gdk_property_get(gdk_screen_get_root_window(screen), gdk_atom_intern(atom_name, FALSE), GDK_NONE, 0,
                     64 * 1024 * 1024, FALSE, &type, &format, &buffer_size, &buffer);
    g_free(atom_name);
  }

#ifdef USE_COLORDGTK
  /* also try to get the profile from colord. this will set the value asynchronously! */
  if(use_colord)
  {
    CdWindow *window = cd_window_new();
    GtkWidget *center_widget = dt_ui_center(darktable.gui->ui);
    cd_window_get_profile(window, center_widget, NULL, dt_colorspaces_get_display_profile_colord_callback, NULL);
  }
#endif

#elif defined GDK_WINDOWING_QUARTZ
  // disable automatic color management on OS X 10.10 since we end up applying a profile twice
  // TODO: check if this issue applies to OS X 10.9, also re-check 10.8 and earlier versions
#ifndef kCFCoreFoundationVersionNumber10_10
#define kCFCoreFoundationVersionNumber10_10 1151.16
#endif
  if(kCFCoreFoundationVersionNumber < kCFCoreFoundationVersionNumber10_10)
  {
    GdkScreen *screen = gtk_widget_get_screen(widget);
    if(screen == NULL) screen = gdk_screen_get_default();
    int monitor = gdk_screen_get_monitor_at_window(screen, gtk_widget_get_window(widget));

    CGDirectDisplayID ids[monitor + 1];
    uint32_t total_ids;
    CMProfileRef prof = NULL;
    if(CGGetOnlineDisplayList(monitor + 1, &ids[0], &total_ids) == kCGErrorSuccess && total_ids == monitor + 1)
      CMGetProfileByAVID(ids[monitor], &prof);
    if(prof != NULL)
    {
      CFDataRef data;
      data = CMProfileCopyICCData(NULL, prof);
      CMCloseProfile(prof);

      UInt8 *tmp_buffer = (UInt8 *)g_malloc(CFDataGetLength(data));
      CFDataGetBytes(data, CFRangeMake(0, CFDataGetLength(data)), tmp_buffer);

      buffer = (guint8 *)tmp_buffer;
      buffer_size = CFDataGetLength(data);

      CFRelease(data);
    }
    profile_source = g_strdup("osx color profile api");
  }
#elif defined G_OS_WIN32
  (void)widget;
  HDC hdc = GetDC(NULL);
  if(hdc != NULL)
  {
    DWORD len = 0;
    GetICMProfile(hdc, &len, NULL);
    gchar *path = g_new(gchar, len);

    if(GetICMProfile(hdc, &len, path))
    {
      gsize size;
      g_file_get_contents(path, (gchar **)&buffer, &size, NULL);
      buffer_size = size;
    }
    g_free(path);
    ReleaseDC(NULL, hdc);
  }
  profile_source = g_strdup("windows color profile api");
#endif

  int profile_changed = buffer_size > 0
                        && (darktable.color_profiles->xprofile_size != buffer_size
                            || memcmp(darktable.color_profiles->xprofile_data, buffer, buffer_size) != 0);
  if(profile_changed)
  {
    cmsHPROFILE profile = NULL;
    char name[512] = { 0 };
    // thanks to ufraw for this!
    g_free(darktable.color_profiles->xprofile_data);
    darktable.color_profiles->xprofile_data = buffer;
    darktable.color_profiles->xprofile_size = buffer_size;
    profile = cmsOpenProfileFromMem(buffer, buffer_size);
    if(profile)
    {
      dt_colorspaces_get_profile_name(profile, "en", "US", name, sizeof(name));
      for(GList *iter = darktable.color_profiles->profiles; iter; iter = g_list_next(iter))
      {
        dt_colorspaces_color_profile_t *p = (dt_colorspaces_color_profile_t *)iter->data;
        if(p->type == DT_COLORSPACE_DISPLAY)
        {
          if(p->profile) dt_colorspaces_cleanup_profile(p->profile);
          p->profile = profile;
          break;
        }
      }
    }
    dt_print(DT_DEBUG_CONTROL, "[color profile] we got a new screen profile `%s' from the %s (size: %d)\n",
             *name ? name : "(unknown)", profile_source, buffer_size);
  }
  else
  {
    g_free(buffer);
  }
  pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
  if(profile_changed) dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_CHANGED);
  g_free(profile_source);
}

const dt_colorspaces_color_profile_t *dt_colorspaces_get_profile(dt_colorspaces_color_profile_type_t type, const char *filename, dt_colorspaces_profile_direction_t direction)
{
  for(GList *iter = darktable.color_profiles->profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *p = (dt_colorspaces_color_profile_t *)iter->data;
    if(((direction & DT_PROFILE_DIRECTION_IN && p->in_pos > -1) ||
        (direction & DT_PROFILE_DIRECTION_OUT && p->out_pos > -1) ||
        (direction & DT_PROFILE_DIRECTION_DISPLAY && p->display_pos > -1)) &&
       (p->type == type && (type != DT_COLORSPACE_FILE || !strcmp(p->filename, filename))))
    {
      return p;
    }
  }

  return NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

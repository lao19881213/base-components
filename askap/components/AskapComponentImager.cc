/// @file AskapComponentImager.cc
///
/// @copyright (c) 2016 CSIRO
/// Australia Telescope National Facility (ATNF)
/// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
/// PO Box 76, Epping NSW 1710, Australia
/// atnf-enquiries@csiro.au
///
/// This file is part of the ASKAP software distribution.
///
/// The ASKAP software distribution is free software: you can redistribute it
/// and/or modify it under the terms of the GNU General Public License as
/// published by the Free Software Foundation; either version 2 of the License,
/// or (at your option) any later version.
///
/// This program is distributed in the hope that it will be useful,
/// but WITHOUT ANY WARRANTY; without even the implied warranty of
/// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
/// GNU General Public License for more details.
///
/// You should have received a copy of the GNU General Public License
/// along with this program; if not, write to the Free Software
/// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
///
/// @author Ben Humphreys <ben.humphreys@csiro.au>
/// @author Matthew Whiting <Matthew.Whiting@csiro.au>
/// This implementation is based on the Casacore Component Imager,
/// although the evaluateGaussian functions are original.

// Include own header file first
#include "AskapComponentImager.h"

// Include package level header file
#include "askap_components.h"

// System includes
#include <cmath>
#include <limits>
#include <algorithm>
#include <typeinfo>

// ASKAPsoft includes
#include "askap/askap/AskapLogging.h"
#include "askap/askap/AskapError.h"
#include "casacore/casa/aipstype.h"
#include "casacore/casa/Arrays/IPosition.h"
#include "casacore/casa/Arrays/Vector.h"
#include "casacore/casa/Quanta/MVAngle.h"
#include "casacore/casa/Quanta/MVDirection.h"
#include "casacore/casa/Quanta/MVFrequency.h"
#include "casacore/scimath/Functionals/Gaussian2D.h"
#include "casacore/scimath/Functionals/Gaussian1D.h"
#include "casacore/images/Images/ImageInterface.h"
#include "casacore/measures/Measures/Stokes.h"
#include "casacore/measures/Measures/MDirection.h"
#include "casacore/measures/Measures/MFrequency.h"
#include "casacore/measures/Measures/MeasRef.h"
#include "components/ComponentModels/SkyComponent.h"
#include "components/ComponentModels/ComponentList.h"
#include "components/ComponentModels/Flux.h"
#include "components/ComponentModels/ConstantSpectrum.h"
#include "components/ComponentModels/SpectralIndex.h"
#include "components/ComponentModels/SpectralModel.h"
#include "components/ComponentModels/ComponentType.h"
#include "components/ComponentModels/ComponentShape.h"
#include "components/ComponentModels/PointShape.h"
#include "components/ComponentModels/GaussianShape.h"
#include "components/ComponentModels/DiskShape.h"
#include "casacore/coordinates/Coordinates/CoordinateUtil.h"
#include "casacore/coordinates/Coordinates/CoordinateSystem.h"
#include "casacore/coordinates/Coordinates/DirectionCoordinate.h"
#include "casacore/coordinates/Coordinates/SpectralCoordinate.h"

ASKAP_LOGGER(logger, ".AskapComponentImager");

using namespace askap;
using namespace askap::components;
using namespace casacore;

template <class T>
void AskapComponentImager::project(casacore::ImageInterface<T>& image,
                                   const casacore::ComponentList& list, const unsigned int term)
{
    if (list.nelements() == 0) {
        return;
    }

    const CoordinateSystem& coords = image.coordinates();
    const IPosition imageShape = image.shape();

    // Find which pixel axes correspond to the DirectionCoordinate in the
    // supplied coordinate system
    const Vector<Int> dirAxes = CoordinateUtil::findDirectionAxes(coords);
    ASKAPCHECK(dirAxes.nelements() == 2,
               "Coordinate system has unsupported number of direction axes");
    const uInt latAxis = dirAxes(0);
    const uInt longAxis = dirAxes(1);

    // Find the Direction coordinate and check the right number of axes exists
    DirectionCoordinate dirCoord = coords.directionCoordinate(
                                       coords.findCoordinate(Coordinate::DIRECTION));
    ASKAPCHECK(dirCoord.nPixelAxes() == 2,
               "DirectionCoordinate has unsupported number of pixel axes");
    ASKAPCHECK(dirCoord.nWorldAxes() == 2,
               "DirectionCoordinate has unsupported number of world axes");
    dirCoord.setWorldAxisUnits(Vector<String>(2, "rad"));

    // Check if there is a Stokes Axes and if so which polarizations.
    // Otherwise only image the I polarisation.
    Vector<Stokes::StokesTypes> stokes;
    // Vector stating which polarisations are on each plane
    // Find which axis is the stokes pixel axis
    const Int polAxis = CoordinateUtil::findStokesAxis(stokes, coords);
    const uInt nStokes = stokes.nelements();

    if (polAxis >= 0) {
        ASKAPASSERT(static_cast<uInt>(imageShape(polAxis)) == nStokes);
        // If there is a Stokes axis it can only contain Stokes::I,Q,U,V pols.
        for (uInt p = 0; p < nStokes; ++p) {
            ASKAPCHECK(stokes(p) == Stokes::I || stokes(p) == Stokes::Q ||
                       stokes(p) == Stokes::U || stokes(p) == Stokes::V,
                       "Stokes axis can only contain I, Q, U or V pols");
        }
    } else {
        ASKAPLOG_DEBUG_STR(logger, "No polarisation axis, assuming Stokes I");
    }

    // Get the frequency axis and get the all the frequencies
    // as a Vector<MVFrequency>.
    const Int freqAxis = CoordinateUtil::findSpectralAxis(coords);
    ASKAPCHECK(freqAxis >= 0, "Image must have a frequency axis");
    const uInt nFreqs = static_cast<uInt>(imageShape(freqAxis));
    Vector<MVFrequency> freqValues(nFreqs);
    {
        SpectralCoordinate specCoord =
            coords.spectralCoordinate(coords.findCoordinate(Coordinate::SPECTRAL));
        specCoord.setWorldAxisUnits(Vector<String>(1, "Hz"));

        // Create Frequency MeasFrame; this will enable conversions between
        // spectral frames (e.g. the CS frame might be TOPO and the CL
        // frame LSRK)
        MFrequency::Types specConv;
        MEpoch epochConv;
        MPosition posConv;
        MDirection dirConv;
        specCoord.getReferenceConversion(specConv, epochConv, posConv, dirConv);
        for (uInt f = 0; f < nFreqs; f++) {
            Double thisFreq;
            if (!specCoord.toWorld(thisFreq, static_cast<Double>(f))) {
                ASKAPTHROW(AskapError, "Cannot convert a frequency value");
            }
            freqValues(f) = MVFrequency(thisFreq);
        }
    }

    // Process each SkyComponent individually
    for (uInt i = 0; i < list.nelements(); ++i) {
        const SkyComponent& c = list.component(i);

        for (uInt freqIdx = 0; freqIdx < nFreqs; ++freqIdx) {

            // Scale flux based on spectral model and taylor term
            const MFrequency chanFrequency(freqValues(freqIdx).get());
            Flux<Double> flux = makeFlux(c, chanFrequency, term);

            for (uInt polIdx = 0; polIdx < stokes.size(); ++polIdx) {

                switch (c.shape().type()) {
                    case ComponentType::POINT:
                        projectPointShape(image, c, latAxis, longAxis, dirCoord,
                                          freqAxis, freqIdx, flux,
                                          polAxis, polIdx, stokes(polIdx));
                        break;

                    case ComponentType::GAUSSIAN:
                        projectGaussianShape(image, c, latAxis, longAxis, dirCoord,
                                             freqAxis, freqIdx, flux,
                                             polAxis, polIdx, stokes(polIdx));
                        break;

                    default:
                        ASKAPTHROW(AskapError, "Unsupported shape type");
                        break;
                }

            } // end polIdx loop
        } // End freqIdx loop

    } // End component list loop
}

template <class T>
void AskapComponentImager::projectPointShape(casacore::ImageInterface<T>& image,
        const casacore::SkyComponent& c,
        const casacore::Int latAxis, const casacore::Int longAxis,
        const casacore::DirectionCoordinate& dirCoord,
        const casacore::Int freqAxis, const casacore::uInt freqIdx,
        const casacore::Flux<casacore::Double>& flux,
        const casacore::Int polAxis, const casacore::uInt polIdx,
        const casacore::Stokes::StokesTypes& stokes)
{
    // Convert world position to pixel position
    const MDirection& dir = c.shape().refDirection();
    Vector<Double> pixelPosition(2);
    const bool toPixelOk = dirCoord.toPixel(pixelPosition, dir);
    ASKAPCHECK(toPixelOk, "toPixel failed");

    // Don't image this component if it falls outside the image
    const IPosition imageShape = image.shape();
    const double latPosition = round(pixelPosition(0));
    const double lonPosition = round(pixelPosition(1));
    if (latPosition < 0 || latPosition > (imageShape(latAxis) - 1)
            || lonPosition < 0 || lonPosition > (imageShape(longAxis) - 1)) {
        return;
    }

    // Add to image
    const IPosition pos = makePosition(latAxis, longAxis, freqAxis, polAxis,
                                       static_cast<size_t>(latPosition),
                                       static_cast<size_t>(lonPosition),
                                       freqIdx, polIdx);
    image.putAt(image(pos) + (flux.copy().value(stokes, true).getValue("Jy")), pos);
}

template <class T>
void AskapComponentImager::projectGaussianShape(casacore::ImageInterface<T>& image,
        const casacore::SkyComponent& c,
        const casacore::Int latAxis, const casacore::Int longAxis,
        const casacore::DirectionCoordinate& dirCoord,
        const casacore::Int freqAxis, const casacore::uInt freqIdx,
        const casacore::Flux<casacore::Double>& flux,
        const casacore::Int polAxis, const casacore::uInt polIdx,
        const casacore::Stokes::StokesTypes& stokes)
{
    // Convert world position to pixel position
    const MDirection& dir = c.shape().refDirection();
    Vector<Double> pixelPosition(2);
    const bool toPixelOk = dirCoord.toPixel(pixelPosition, dir);
    ASKAPCHECK(toPixelOk, "toPixel failed");

    // Don't image this component if it falls outside the image
    // Note: This code will cull those components which may (due to rounding)
    // have been positioned in the edge pixels.
    const IPosition imageShape = image.shape();
    if (pixelPosition(0) < 0 || pixelPosition(0) > (imageShape(latAxis) - 1)
            || pixelPosition(1) < 0 || pixelPosition(1) > (imageShape(longAxis) - 1)) {
        return;
    }

    // Get the pixel sizes then convert the axis sizes to pixels
    const GaussianShape& cShape = dynamic_cast<const GaussianShape&>(c.shape());
    const MVAngle pixelLatSize = MVAngle(abs(dirCoord.increment()(0)));
    const MVAngle pixelLongSize = MVAngle(abs(dirCoord.increment()(1)));
    ASKAPCHECK(pixelLatSize == pixelLongSize, "Non-equal pixel sizes not supported");
    const double majorAxisPixels = cShape.majorAxisInRad() / pixelLongSize.radian();
    const double minorAxisPixels = cShape.minorAxisInRad() / pixelLongSize.radian();

    // Create the guassian function
    Gaussian2D<T> gauss;
    gauss.setXcenter(pixelPosition(0));
    gauss.setYcenter(pixelPosition(1));
    gauss.setMinorAxis(std::numeric_limits<T>::min());
    gauss.setMajorAxis(std::max(majorAxisPixels, minorAxisPixels));
    gauss.setMinorAxis(std::min(majorAxisPixels, minorAxisPixels));
    gauss.setPA(cShape.positionAngleInRad());
    gauss.setFlux(flux.copy().value(stokes, true).getValue("Jy"));

    // Determine how far to sample before the flux gets too low to be meaningful
    // We do this by going out from the centre position along both the x and y
    // axis then choose the maximum of the two
    const T epsilon = std::numeric_limits<T>::epsilon();
    const int cutoff = findCutoff(gauss, std::max(imageShape(latAxis), imageShape(longAxis)), epsilon);

    // Determine the starting and end pixels which need processing on both axes. Note
    // that these are "inclusive" ranges.
    const int startLat = std::max(0, static_cast<int>(pixelPosition(0)) - cutoff);
    const int endLat = std::min(static_cast<int>(imageShape(latAxis) - 1),
                                static_cast<int>(pixelPosition(0)) + cutoff);
    const int startLon = std::max(0, static_cast<int>(pixelPosition(1)) - cutoff);
    const int endLon = std::min(static_cast<int>(imageShape(longAxis) - 1),
                                static_cast<int>(pixelPosition(1)) + cutoff);

    IPosition pos = makePosition(latAxis, longAxis, freqAxis, polAxis,
                                 static_cast<size_t>(nearbyint(pixelPosition[0])),
                                 static_cast<size_t>(nearbyint(pixelPosition[1])),
                                 freqIdx, polIdx);

    // For each pixel in the region bounded by the source centre + cutoff
    for (int lat = startLat; lat <= endLat; ++lat) {
        for (int lon = startLon; lon <= endLon; ++lon) {
            pos(latAxis) = lat;
            pos(longAxis) = lon;
            image.putAt(image(pos) + evaluateGaussian(gauss, lat, lon), pos);
        }
    }
}

IPosition AskapComponentImager::makePosition(const casacore::Int latAxis, const casacore::Int longAxis,
        const casacore::Int spectralAxis, const casacore::Int polAxis,
        const casacore::uInt latIdx, const casacore::uInt longIdx,
        const casacore::uInt spectralIdx, const casacore::uInt polIdx)
{
    // Count the number of valid axes
    uInt naxis = 0;

    if (latAxis >= 0) ++naxis;
    if (longAxis >= 0) ++naxis;
    if (spectralAxis >= 0) ++naxis;
    if (polAxis >= 0) ++naxis;

    // Create the IPosition
    IPosition pos(naxis);
    if (latAxis >= 0) pos(latAxis) = latIdx;
    if (longAxis >= 0) pos(longAxis) = longIdx;
    if (spectralAxis >= 0) pos(spectralAxis) = spectralIdx;
    if (polAxis >= 0) pos(polAxis) = polIdx;

    return pos;
}

casacore::Flux<casacore::Double> AskapComponentImager::makeFlux(const casacore::SkyComponent& c,
        const casacore::MFrequency& chanFrequency,
        const unsigned int term)
{
    // Transform flux for the given spectral model
    Flux<Double> flux;
    if (c.spectrum().type() == ComponentType::CONSTANT_SPECTRUM) {
        flux = c.flux().copy();

    } else if (c.spectrum().type() == ComponentType::SPECTRAL_INDEX) {
        // Scale flux based on spectral index
        flux = c.flux().copy();
        const Double scale = c.spectrum().sample(chanFrequency);
        flux.scaleValue(scale, scale, scale, scale);

    } else {
        ASKAPTHROW(AskapError, "Unsupported spectral model");
    }

    // Now transform flux for the given taylor term
    if (term == 0) {
        // Taylor Term 0
        // I0 = I(v0)
    } else if (term == 1) {
        // Taylor Term 1
        // I1 = I(v0) * alpha
        Double alpha = 0.0;
        if (c.spectrum().type() == ComponentType::SPECTRAL_INDEX) {
            const casacore::SpectralIndex& spectralModel =
                dynamic_cast<const casacore::SpectralIndex&>(c.spectrum());
            alpha = spectralModel.index();
        }
        flux.scaleValue(alpha, alpha, alpha, alpha);
    } else if (term == 2) {
        // Taylor Term 2
        // I2 = I(v0) * (0.5 * alpha * (alpha - 1) + beta)
        Double alpha = 0.0;
        if (c.spectrum().type() == ComponentType::SPECTRAL_INDEX) {
            const casacore::SpectralIndex& spectralModel =
                dynamic_cast<const casacore::SpectralIndex&>(c.spectrum());
            alpha = spectralModel.index();
        }
        const Double beta = 0.0;
        const Double factor = (0.5 * alpha * (alpha - 1.0) + beta);
        flux.scaleValue(factor, factor, factor, factor);
    } else {
        ASKAPTHROW(AskapError, "Only support taylor terms 0, 1 & 2");
    }

    return flux;
}

template <class T>
int AskapComponentImager::findCutoff(const Gaussian2D<T>& gauss, const int spatialLimit,
                                     const double fluxLimit)
{
    // Make a copy of the gaussian and set the PA to zero so this function can
    // walk the major axis easily, to determine the cutoff. The major axis is
    // parallel with the y axis when the position angle is zero.
    Gaussian2D<T> g = gauss;
    g.setPA(0.0);

    int cutoff = 0;
    while ((cutoff <= spatialLimit) &&
            abs(g(g.xCenter(), g.yCenter() + cutoff) >= fluxLimit)) {
        ++cutoff;
    }
    return cutoff;
}

template <class T>
double AskapComponentImager::evaluateGaussian(const Gaussian2D<T> &gauss,
        const int xpix, const int ypix)
{
    // If we have a very narrow Gaussian, calculate the pixel flux
    // using the 1D approach. Otherwise, we need to do a 2D integral.
    if (gauss.minorAxis() < 1.e-3) {
        return evaluateGaussian1D<T>(gauss, xpix, ypix);
    } else {
        return evaluateGaussian2D<T>(gauss, xpix, ypix);
    }

}

template <class T>
double AskapComponentImager::evaluateGaussian2D(const Gaussian2D<T> &gauss,
        const int xpix, const int ypix)
{
    // Performs a spatial integration over the pixel extent to
    // evaluate the contained flux.

    double pixelVal = 0.;

    double minSigma = std::min(gauss.majorAxis(), gauss.minorAxis()) /
                      (2. * M_SQRT2 * sqrt(M_LN2));
    /// @todo We could do a lot better here in getting the right
    /// sampling in an adaptive manner. This approach is a little bit
    /// of a kludge, but works reasonably well.
    double delta = std::min(1. / 32.,
                            pow(10., floor(log10(minSigma / 5.) / log10(2.)) * log10(2.)));
    int nstep = int(1. / delta);

    double xpos, ypos;
    xpos = xpix - 0.5 - delta;
    double xScaleFactor, yScaleFactor;

    for (int dx = 0; dx <= nstep; dx++) {
        xpos += delta;
        ypos = ypix - 0.5 - delta;

        for (int dy = 0; dy <= nstep; dy++) {
            ypos += delta;

            // This is integration using Simpson's rule. In each direction, the
            // end points get a factor of 1, then odd steps get a factor of 4, and
            // even steps 2. The whole sum then gets scaled by delta/3. for each
            // dimension.

            if (dx == 0 || dx == nstep) xScaleFactor = 1;
            else xScaleFactor = (dx % 2 == 1) ? 4. : 2.;

            if (dy == 0 || dy == nstep) yScaleFactor = 1;
            else yScaleFactor = (dy % 2 == 1) ? 4. : 2.;

            pixelVal += gauss(xpos, ypos) * (xScaleFactor * yScaleFactor);

        }
    }

    pixelVal *= (delta * delta / 9.);

    return pixelVal;
}

template <class T>
double AskapComponentImager::evaluateGaussian1D(const Gaussian2D<T> &gauss,
        const int xpix, const int ypix)
{
    // This approach represents the Gaussian as a one-dimensional
    // line, and finds the points where that line intercepts the
    // borders of the given pixel. Note that the provided (integral)
    // position is assumed to be at the centre of the pixel. If the
    // line does not intercept the pixel, the flux for the pixel is
    // zero.

    // ASKAPLOG_DEBUG_STR(logger, "1d Gaussian evalution, at " << xpix << "," << ypix);

    // boundaries of the pixel
    double ypixmax = ypix + 0.5;
    double ypixmin = ypix - 0.5;
    double xpixmin = xpix - 0.5;
    double xpixmax = xpix + 0.5;
    // ASKAPLOG_DEBUG_STR(logger, "Pixel ranges: xpixmin="<<xpixmin<<", xpixmax="<<xpixmax<<
    //                    ",  ypixmin="<<ypixmin<<", ypixmax="<<ypixmax);

    // properties of the (2D) gaussian
    double x0gauss = gauss.xCenter();
    double y0gauss = gauss.yCenter();
    double sigma = gauss.majorAxis() / (2. * M_SQRT2 * sqrt(M_LN2));
    // ASKAPLOG_DEBUG_STR(logger, "Centre of Gaussian = ("<<x0gauss << "," << y0gauss << ", pa="<<gauss.PA()*180./M_PI);

    // Find where the line intersectes the pixel boundaries
    std::vector<std::pair<double, double> > interceptList;

    if (fabs(gauss.PA()) < 1.e-6) {
        // vertical line - simplifies things
        if ((x0gauss >= xpixmin) && (x0gauss < xpixmax)) {
            // if we are in the pixel
            interceptList.push_back(std::pair<double, double>(x0gauss, ypixmin));
            interceptList.push_back(std::pair<double, double>(x0gauss, ypixmax));
        }
    } else if (fabs(gauss.PA() - M_PI / 2.) < 1.e-6) {
        // horizontal line
        if ((y0gauss >= ypixmin) && (y0gauss < ypixmax)) {
            // if we are in the pixel
            interceptList.push_back(std::pair<double, double>(xpixmin, y0gauss));
            interceptList.push_back(std::pair<double, double>(xpixmax, y0gauss));
        }

    } else {
        // general case of angled line. Need to find (up to) two points where
        // the line intersects the pixel boundaries
        // xminInt = x-value where line intersects bottom pixel boundary
        // xmaxInt = x-value where line intersects top pixel boundary
        // yminInt = y-value where line intersects left pixel boundary
        // ymaxInt = y-value where line intersects right pixel boundary

        double gaussianSlope = tan(gauss.PA() - M_PI / 2.);
        double xminInt = x0gauss + (ypixmin - y0gauss) / gaussianSlope;
        double xmaxInt = x0gauss + (ypixmax - y0gauss) / gaussianSlope;
        double yminInt = y0gauss + (xpixmin - x0gauss) * gaussianSlope;
        double ymaxInt = y0gauss + (xpixmax - x0gauss) * gaussianSlope;

        // ASKAPLOG_DEBUG_STR(logger, "intercepts: " << xminInt << " " << yminInt << " " << xmaxInt << " " << ymaxInt);

        if ((xminInt >= xpixmin) && (xminInt < xpixmax)) {
            interceptList.push_back(std::pair<double, double>(xminInt, ypixmin));
        }
        if ((xmaxInt >= xpixmin) && (xmaxInt < xpixmax)) {
            interceptList.push_back(std::pair<double, double>(xmaxInt, ypixmax));
        }
        if ((yminInt >= ypixmin) && (yminInt < ypixmax)) {
            interceptList.push_back(std::pair<double, double>(xpixmin, yminInt));
        }
        if ((ymaxInt >= ypixmin) && (ymaxInt < ypixmax)) {
            interceptList.push_back(std::pair<double, double>(xpixmax, ymaxInt));
        }

    }

    double pixelVal = 0.;
    if (interceptList.size() == 2) {

        // Find the locations of the two intercept points in the
        // coordinates *along* the line, in units of the sigma value
        // (ie. standard 'z-score' values)
        std::vector<double> z(2);
        for (int i = 0; i < 2; i++) {
            z[i] = hypot(x0gauss - interceptList[i].first,
                         y0gauss - interceptList[i].second) / sigma;
            if (y0gauss > interceptList[i].second) {
                // Make displacement negative for points below the centre
                z[i] *= -1.;
            }
        }

        // Make a 1D gaussian to get the height correct, since if the
        // 2D gaussian's minor axis is really small, then the height
        // will be massive for a reasonable (integrated) flux. We
        // define with the height, but set the flux directly (which
        // will implicitly reset the height value).

        casacore::Gaussian1D<T> gauss1d(gauss.height(), 0., gauss.majorAxis());
        gauss1d.setFlux(gauss.flux());
        // ASKAPLOG_DEBUG_STR(logger, "Defined a 1D Gaussian with height=" << gauss1d.height() << ", flux="<<
        //                    gauss1d.flux() << " and FWHM="<<gauss1d.width());

        // Find the flux via a different in error-function values for the two intercept points
        pixelVal = gauss1d.flux() * fabs(0.5 * (erf(z[0] / (M_SQRT2 * sigma)) - erf(z[1] / (M_SQRT2 * sigma))));

        // ASKAPLOG_DEBUG_STR(logger, "Flux of " << pixelVal << " between z=["<<z[0]<<","<<z[1] <<
        //                    "] or pixel locations ("<<interceptList[0].first<<","<<interceptList[0].second <<
        //                    ") & ("<<interceptList[1].first<<","<<interceptList[1].second <<")");

    } else {
        // Line does not intersect this pixel. Flux = 0.
        pixelVal = 0.;
    }

    return pixelVal;
}


// Explicit instantiation
template void AskapComponentImager::project(casacore::ImageInterface<float>&,
        const casacore::ComponentList&, const unsigned int);
template void AskapComponentImager::project(casacore::ImageInterface<double>&,
        const casacore::ComponentList&, const unsigned int);
template double AskapComponentImager::evaluateGaussian(const casacore::Gaussian2D<float> &gauss,
        const int xpix, const int ypix);
template double AskapComponentImager::evaluateGaussian(const casacore::Gaussian2D<double> &gauss,
        const int xpix, const int ypix);

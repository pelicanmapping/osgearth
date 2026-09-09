#include "BrunetonImpl"
#include "eb_shaders.h"
#include <osgEarth/Notify>
#include <osgEarth/VirtualProgram>
#include <osg/Texture2D>
#include <osg/Texture3D>

using namespace osgEarth;
using namespace Bruneton;

ComputeDrawable::ComputeDrawable(
    float bottom_radius,
    float top_radius,
    bool best_quality) :

    osg::Drawable(),

    // atmosphere upper and lower bounds
    _bottom_radius(bottom_radius),
    _top_radius(top_radius),

    _sun_angular_radius(0.004675f), // Mean solar half-angle, about 0.268 degrees.

    // higher values make no visible difference
    _length_unit_in_meters(1.0f),

    // half_prec=true does not work; it results in rendering artifacts.
    _use_half_precision(false),

    // combined=true is 50% faster and uses one fewer texture.
    // But... it had rendering artifacts. Something about the
    // GetExtrapolatedSingleMieScattering shader method is not unpacking
    // the mie scattering values properly
    _use_combined_textures(false),

    // false = use the real solar spectrum
    _use_constant_solar_spectrum(false),

    _use_ozone(true),
    _use_luminance(true),
    _do_white_balance(false),

    // false => per-vertex radiance; true => per-fragment radiance (slower)
    _best_quality(best_quality)
{
    setCullingActive(false);
}

void
ComputeDrawable::drawImplementation(osg::RenderInfo& ri) const
{
    // Values from "Reference Solar Spectral Irradiance: ASTM G-173", ETR column
    // (see http://rredc.nrel.gov/solar/spectra/am1.5/ASTMG173/ASTMG173.html),
    // summed and averaged in each bin (e.g. the value for 360nm is the average
    // of the ASTM G-173 values for all wavelengths between 360 and 370nm).
    // Values in W.m^-2.
    int lambda_min = 360;
    int lambda_max = 830;

    double kSolarIrradiance[] =
    {
        1.11776, 1.14259, 1.01249, 1.14716, 1.72765, 1.73054, 1.6887, 1.61253,
        1.91198, 2.03474, 2.02042, 2.02212, 1.93377, 1.95809, 1.91686, 1.8298,
        1.8685, 1.8931, 1.85149, 1.8504, 1.8341, 1.8345, 1.8147, 1.78158, 1.7533,
        1.6965, 1.68194, 1.64654, 1.6048, 1.52143, 1.55622, 1.5113, 1.474, 1.4482,
        1.41018, 1.36775, 1.34188, 1.31429, 1.28303, 1.26758, 1.2367, 1.2082,
        1.18737, 1.14683, 1.12362, 1.1058, 1.07124, 1.04992
    };

    // Values from http://www.iup.uni-bremen.de/gruppen/molspec/databases/
    // referencespectra/o3spectra2011/index.html for 233K, summed and averaged in
    // each bin (e.g. the value for 360nm is the average of the original values
    // for all wavelengths between 360 and 370nm). Values in m^2.
    double kOzoneCrossSection[] =
    {
        1.18e-27, 2.182e-28, 2.818e-28, 6.636e-28, 1.527e-27, 2.763e-27, 5.52e-27,
        8.451e-27, 1.582e-26, 2.316e-26, 3.669e-26, 4.924e-26, 7.752e-26, 9.016e-26,
        1.48e-25, 1.602e-25, 2.139e-25, 2.755e-25, 3.091e-25, 3.5e-25, 4.266e-25,
        4.672e-25, 4.398e-25, 4.701e-25, 5.019e-25, 4.305e-25, 3.74e-25, 3.215e-25,
        2.662e-25, 2.238e-25, 1.852e-25, 1.473e-25, 1.209e-25, 9.423e-26, 7.455e-26,
        6.566e-26, 5.105e-26, 4.15e-26, 4.228e-26, 3.237e-26, 2.451e-26, 2.801e-26,
        2.534e-26, 1.624e-26, 1.465e-26, 2.078e-26, 1.383e-26, 7.105e-27
    };

    // From https://en.wikipedia.org/wiki/Dobson_unit, in molecules.m^-2.
    double kDobsonUnit = 2.687e20;
    // Maximum number density of ozone molecules, in m^-3 (computed so at to get
    // 300 Dobson units of ozone - for this we divide 300 DU by the integral of
    // the ozone density profile defined below, which is equal to 15km).
    double kMaxOzoneNumberDensity = 300.0 * kDobsonUnit / 15000.0;
    // Wavelength independent solar irradiance "spectrum" (not physically
    // realistic, but was used in the original implementation).
    double kConstantSolarIrradiance = 1.5;
    double kTopRadius = _top_radius; // 6420000.0;
    double kRayleigh = 1.24062e-6;
    double kRayleighScaleHeight = 8000.0;
    double kMieScaleHeight = 1200.0;
    double kMieAngstromAlpha = 0.0;
    double kMieAngstromBeta = 5.328e-3;
    double kMieSingleScatteringAlbedo = 0.9;
    double kMiePhaseFunctionG = 0.8;
    double kGroundAlbedo = 0.1;
    double max_sun_zenith_angle = (_use_half_precision ? 102.0 : 120.0) / 180.0 * osg::PI;

    dw::DensityProfileLayer* rayleigh_layer = new dw::DensityProfileLayer("rayleigh", 0.0, 1.0, -1.0 / kRayleighScaleHeight, 0.0, 0.0);
    dw::DensityProfileLayer* mie_layer = new dw::DensityProfileLayer("mie", 0.0, 1.0, -1.0 / kMieScaleHeight, 0.0, 0.0);

    // Density profile increasing linearly from 0 to 1 between 10 and 25km, and
    // decreasing linearly from 1 to 0 between 25 and 40km. This is an approximate
    // profile from http://www.kln.ac.lk/science/Chemistry/Teaching_Resources/
    // Documents/Introduction%20to%20atmospheric%20chemistry.pdf (page 10).
    std::vector<dw::DensityProfileLayer*> ozone_density;
    ozone_density.push_back(new dw::DensityProfileLayer("absorption0", 25000.0, 0.0, 0.0, 1.0 / 15000.0, -2.0 / 3.0));
    ozone_density.push_back(new dw::DensityProfileLayer("absorption1", 0.0, 0.0, 0.0, -1.0 / 15000.0, 8.0 / 3.0));

    std::vector<double> wavelengths;
    std::vector<double> solar_irradiance;
    std::vector<double> rayleigh_scattering;
    std::vector<double> mie_scattering;
    std::vector<double> mie_extinction;
    std::vector<double> absorption_extinction;
    std::vector<double> ground_albedo;

    for (int l = lambda_min; l <= lambda_max; l += 10)
    {
        double lambda = l * 1e-3;  // micro-meters
        double mie = kMieAngstromBeta / kMieScaleHeight * pow(lambda, -kMieAngstromAlpha);

        wavelengths.push_back(l);

        if (_use_constant_solar_spectrum)
            solar_irradiance.push_back(kConstantSolarIrradiance);
        else
            solar_irradiance.push_back(kSolarIrradiance[(l - lambda_min) / 10]);

        rayleigh_scattering.push_back(kRayleigh * pow(lambda, -4));
        mie_scattering.push_back(mie * kMieSingleScatteringAlbedo);
        mie_extinction.push_back(mie);
        absorption_extinction.push_back(_use_ozone ? kMaxOzoneNumberDensity * kOzoneCrossSection[(l - lambda_min) / 10] : 0.0);
        ground_albedo.push_back(kGroundAlbedo);
    }

    _model = std::unique_ptr<dw::AtmosphereModel>(new dw::AtmosphereModel);

    _model->m_half_precision = _use_half_precision;
    _model->m_combine_scattering_textures = _use_combined_textures;
    _model->m_use_luminance = _use_luminance ? dw::PRECOMPUTED : dw::NONE;
    _model->m_wave_lengths = wavelengths;
    _model->m_solar_irradiance = solar_irradiance;
    _model->m_sun_angular_radius = _sun_angular_radius;
    _model->m_bottom_radius = _bottom_radius;
    _model->m_top_radius = _top_radius;
    _model->m_rayleigh_density = rayleigh_layer;
    _model->m_rayleigh_scattering = rayleigh_scattering;
    _model->m_mie_density = mie_layer;
    _model->m_mie_scattering = mie_scattering;
    _model->m_mie_extinction = mie_extinction;
    _model->m_mie_phase_function_g = kMiePhaseFunctionG;
    _model->m_absorption_density = ozone_density;
    _model->m_absorption_extinction = absorption_extinction;
    _model->m_ground_albedo = ground_albedo;
    _model->m_max_sun_zenith_angle = max_sun_zenith_angle;
    _model->m_length_unit_in_meters = 1.0f; // _length_unit_in_meters;

    int num_scattering_orders = 4;

    _model->initialize(num_scattering_orders);

    double white_point_r = 1.0;
    double white_point_g = 1.0;
    double white_point_b = 1.0;

    if (_do_white_balance)
    {
        _model->convert_spectrum_to_linear_srgb(white_point_r, white_point_g, white_point_b);

        double white_point = (white_point_r + white_point_g + white_point_b) / 3.0;
        white_point_r /= white_point;
        white_point_g /= white_point;
        white_point_b /= white_point;
    }

    _white_point = glm::vec3(float(white_point_r), float(white_point_g), float(white_point_b));

    ri.getState()->setLastAppliedProgramObject(nullptr);
    ri.getState()->dirtyAllAttributes();
    ri.getState()->apply();
}

bool
ComputeDrawable::populateRenderingStateSets(
    osg::StateSet* groundStateSet,
    osg::StateSet* skyStateSet,
    osgEarth::TerrainResources* resources) const
{
    OE_SOFT_ASSERT_AND_RETURN(groundStateSet != nullptr, false);
    OE_SOFT_ASSERT_AND_RETURN(skyStateSet != nullptr, false);
    OE_SOFT_ASSERT_AND_RETURN(resources != nullptr, false);

    if (!resources->reserveTextureImageUnit(_reservation[0], "Sky transmittance"))
        return false;

    if (!resources->reserveTextureImageUnit(_reservation[1], "Sky scattering"))
        return false;

    if (!resources->reserveTextureImageUnit(_reservation[2], "Sky irradiance"))
        return false;

    if (!_model->m_combine_scattering_textures)
    {
        if (!resources->reserveTextureImageUnit(_reservation[3], "Sky single mie scattering"))
            return false;
    }

    if (!_transmittance_tex.valid())
    {
        _transmittance_tex = makeOSGTexture(_model->m_transmittance_texture);
        _scattering_tex = makeOSGTexture(_model->m_scattering_texture);
        _irradiance_tex = makeOSGTexture(_model->m_irradiance_texture);
        if (!_model->m_combine_scattering_textures)
        {
            _single_mie_scattering_tex = makeOSGTexture(_model->m_optional_single_mie_scattering_texture);
        }
    }

    for (int i = 0; i < 2; ++i)
    {
        osg::StateSet* ss = (i == 0 ? groundStateSet : skyStateSet);

        if (ss == nullptr)
            continue;
        
        ss->addUniform(new osg::Uniform("transmittance_texture", _reservation[0].unit()));
        ss->setTextureAttributeAndModes(_reservation[0].unit(), _transmittance_tex, 1);

        ss->addUniform(new osg::Uniform("scattering_texture", _reservation[1].unit()));
        ss->setTextureAttributeAndModes(_reservation[1].unit(), _scattering_tex, 1);

        ss->addUniform(new osg::Uniform("irradiance_texture", _reservation[2].unit()));
        ss->setTextureAttributeAndModes(_reservation[2].unit(), _irradiance_tex, 1);

        if (!_model->m_combine_scattering_textures)
        {
            ss->addUniform(new osg::Uniform("single_mie_scattering_texture", _reservation[3].unit()));
            ss->setTextureAttributeAndModes(_reservation[3].unit(), _single_mie_scattering_tex, 1);
        }

        ss->addUniform(new osg::Uniform("TRANSMITTANCE_TEXTURE_WIDTH", dw::CONSTANTS::TRANSMITTANCE_WIDTH));
        ss->addUniform(new osg::Uniform("TRANSMITTANCE_TEXTURE_HEIGHT", dw::CONSTANTS::TRANSMITTANCE_HEIGHT));
        ss->addUniform(new osg::Uniform("SCATTERING_TEXTURE_R_SIZE", dw::CONSTANTS::SCATTERING_R));
        ss->addUniform(new osg::Uniform("SCATTERING_TEXTURE_MU_SIZE", dw::CONSTANTS::SCATTERING_MU));
        ss->addUniform(new osg::Uniform("SCATTERING_TEXTURE_MU_S_SIZE", dw::CONSTANTS::SCATTERING_MU_S));
        ss->addUniform(new osg::Uniform("SCATTERING_TEXTURE_NU_SIZE", dw::CONSTANTS::SCATTERING_NU));
        ss->addUniform(new osg::Uniform("SCATTERING_TEXTURE_WIDTH", dw::CONSTANTS::SCATTERING_WIDTH));
        ss->addUniform(new osg::Uniform("SCATTERING_TEXTURE_HEIGHT", dw::CONSTANTS::SCATTERING_HEIGHT));
        ss->addUniform(new osg::Uniform("SCATTERING_TEXTURE_DEPTH", dw::CONSTANTS::SCATTERING_DEPTH));
        ss->addUniform(new osg::Uniform("IRRADIANCE_TEXTURE_WIDTH", dw::CONSTANTS::IRRADIANCE_WIDTH));
        ss->addUniform(new osg::Uniform("IRRADIANCE_TEXTURE_HEIGHT", dw::CONSTANTS::IRRADIANCE_HEIGHT));

        ss->addUniform(new osg::Uniform("sun_angular_radius", (float)_model->m_sun_angular_radius));
        ss->addUniform(new osg::Uniform("bottom_radius", (float)(_model->m_bottom_radius / _model->m_length_unit_in_meters)));
        ss->addUniform(new osg::Uniform("top_radius", (float)(_model->m_top_radius / _model->m_length_unit_in_meters)));
        ss->addUniform(new osg::Uniform("mie_phase_function_g", (float)_model->m_mie_phase_function_g));
        ss->addUniform(new osg::Uniform("mu_s_min", (float)cos(_model->m_max_sun_zenith_angle)));

        glm::vec3 sky_spectral_radiance_to_luminance, sun_spectral_radiance_to_luminance;
        _model->sky_sun_radiance_to_luminance(sky_spectral_radiance_to_luminance, sun_spectral_radiance_to_luminance);

        ss->addUniform(new osg::Uniform("SKY_SPECTRAL_RADIANCE_TO_LUMINANCE", sky_spectral_radiance_to_luminance));
        ss->addUniform(new osg::Uniform("SUN_SPECTRAL_RADIANCE_TO_LUMINANCE", sun_spectral_radiance_to_luminance));

        double lambdas[] = { kLambdaR, kLambdaG, kLambdaB };

        glm::vec3 solar_irradiance = _model->to_vector(_model->m_wave_lengths, _model->m_solar_irradiance, lambdas, 1.0);
        ss->addUniform(new osg::Uniform("solar_irradiance", solar_irradiance));

        glm::vec3 rayleigh_scattering = _model->to_vector(_model->m_wave_lengths, _model->m_rayleigh_scattering, lambdas, _model->m_length_unit_in_meters);
        ss->addUniform(new osg::Uniform("rayleigh_scattering", rayleigh_scattering));

        glm::vec3 mie_scattering = _model->to_vector(_model->m_wave_lengths, _model->m_mie_scattering, lambdas, _model->m_length_unit_in_meters);
        ss->addUniform(new osg::Uniform("mie_scattering", mie_scattering));

        ss->addUniform(new osg::Uniform("sun_size", osg::Vec2f(tan(_model->m_sun_angular_radius), cos(_model->m_sun_angular_radius))));

        Bruneton::Shaders shaders;
        VirtualProgram* vp = VirtualProgram::getOrCreate(ss);

        std::string common =
            shaders.header +
            shaders.constants +
            shaders.uniforms +
            shaders.utility +
            shaders.transmittance_functions +
            shaders.scattering_functions +
            shaders.irradiance_functions +
            shaders.rendering_functions +
            shaders.radiance_api;

        //vp->setFunction(
        //    "atmos_eb_ground_init_frag",
        //    shaders.ground_init_frag,
        //    VirtualProgram::LOCATION_FRAGMENT_COLORING,
        //    -FLT_MAX);

        if (i == 0) // ground
        {
            ss->setDefine("OE_SKY_REFLECTION_SAMPLES", _best_quality ? "16" : "8");

            std::string vert =
                common +
                (_best_quality ? shaders.ground_best_vert : shaders.ground_fast_vert);

            std::string frag =
                common +
                shaders.pbr +
                (_best_quality ? shaders.ground_best_frag : shaders.ground_fast_frag);

            vp->setFunction(
                "atmos_eb_ground_render_vert",
                vert,
                VirtualProgram::LOCATION_VERTEX_VIEW,
                1.1f);

            vp->setFunction(
                "atmos_eb_ground_render_frag",
                frag,
                VirtualProgram::LOCATION_FRAGMENT_LIGHTING,
                0.8f);
        }
        else // sky
        {
            std::string vert =
                common + shaders.sky_vert;
            //(_high_quality ? shaders.sky_vert : shaders.sky_vert_fast);

            std::string frag =
                common + shaders.sky_frag;
            //(_high_quality ? shaders.sky_frag : shaders.sky_frag_fast);

            vp->setFunction(
                "atmos_eb_sky_render_vert",
                vert,
                VirtualProgram::LOCATION_VERTEX_VIEW,
                1.1f);

            vp->setFunction(
                "atmos_eb_sky_render_frag",
                frag,
                VirtualProgram::LOCATION_FRAGMENT_LIGHTING,
                0.8f);

            vp->setInheritShaders(false);
        }

        if (_model->m_combine_scattering_textures)
            ss->setDefine("COMBINED_SCATTERING_TEXTURES");

        if (_model->m_use_luminance == dw::NONE)
            ss->setDefine("RADIANCE_API_ENABLED");

        ss->setDefine(
            "UNIT_LENGTH_METERS_INVERSE",
            std::to_string(1.0 / _model->m_length_unit_in_meters));
    }

    return true;
}

osg::Texture*
ComputeDrawable::makeOSGTexture(dw::Texture* dwt) const
{
    if (dwt->target() == GL_TEXTURE_2D)
        return new WrapTexture2D(static_cast<dw::Texture2D*>(dwt));
    else if (dwt->target() == GL_TEXTURE_3D)
        return new WrapTexture3D(static_cast<dw::Texture3D*>(dwt));
    else
        return nullptr;
}

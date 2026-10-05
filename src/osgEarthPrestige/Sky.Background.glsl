in vec2 oe_psky_uv;
out vec4 oe_psky_result;
uniform sampler2D oe_prestige_sky_stars;
uniform mat3 oe_prestige_sky_earthToECI;
uniform vec4 oe_prestige_sky_moon; // direction and angular radius

// Composites catalog stars, the solar disk and a phase-lit lunar sphere behind the atmosphere.
void main()
{
    vec3 direction = normalize(oe_prestige_sky_viewToEarth*oe_psky_viewRay(oe_psky_uv));
    vec3 radiance = vec3(0.0), transmission = vec3(1.0);
    vec2 ground = oe_psky_sphere(oe_prestige_sky_eye,direction,oe_psky_radius);
    bool blocked = ground.x > 0.0;
    if (!blocked)
    {
        vec3 star = oe_prestige_sky_earthToECI*direction;
        vec2 uv = vec2(atan(star.y,star.x)/(2.0*oe_psky_pi),acos(clamp(star.z,-1.0,1.0))/oe_psky_pi);
        radiance = texture(oe_prestige_sky_stars,uv).rgb*oe_prestige_sky_flags.w*0.08;
        float sunAngle = acos(clamp(dot(direction,oe_prestige_sky_sun),-1.0,1.0));
        float aa = max(fwidth(sunAngle),1e-5);
        float sunDisk = 1.0-smoothstep(0.00465-aa,0.00465+aa,sunAngle);
        radiance += oe_prestige_sky_solarIrradiance*2.0*oe_prestige_sky_flags.y*sunDisk;
        float moonAngle = acos(clamp(dot(direction,oe_prestige_sky_moon.xyz),-1.0,1.0));
        float moonRadius = oe_prestige_sky_moon.w;
        float moonAA = max(fwidth(moonAngle),1e-5);
        float moonDisk = 1.0-smoothstep(moonRadius-moonAA,moonRadius+moonAA,moonAngle);
        if (moonDisk > 0.0 && oe_prestige_sky_flags.z > 0.5)
        {
            vec3 offset = (direction-oe_prestige_sky_moon.xyz*dot(direction,oe_prestige_sky_moon.xyz))/moonRadius;
            vec3 normal = offset-oe_prestige_sky_moon.xyz*sqrt(max(0.0,1.0-dot(offset,offset)));
            float phase = max(0.0,dot(normal,oe_prestige_sky_sun));
            // Low-frequency lunar albedo; no external asset or failed texture load at startup.
            float maria = 0.65+0.15*sin(offset.x*19.0+sin(offset.z*13.0))*sin(offset.y*17.0);
            radiance = mix(radiance,oe_prestige_sky_solarIrradiance*(maria*(phase*0.08+0.0008)),moonDisk);
        }
    }
#ifdef OE_PRESTIGE_SKY_ATMOSPHERE
    if (oe_prestige_sky_flags.x > 0.5)
    {
        // Extinction along the entire celestial ray; outside the shell it starts at the entry point.
        vec2 shell = oe_psky_sphere(oe_prestige_sky_eye,direction,oe_psky_top);
        if (shell.y > max(shell.x,0.0))
        {
            vec3 origin = oe_prestige_sky_eye+direction*max(0.0,shell.x+0.001);
            transmission = oe_psky_transmittance(origin,direction);
        }
        radiance = radiance*transmission+oe_psky_sky(direction);
    }
#endif
#ifdef OE_CLOUD_LAYER
    radiance = oe_cloud_apply(radiance,direction,1e8);
#endif
    oe_psky_result = vec4(oe_psky_output(radiance),1.0);
}

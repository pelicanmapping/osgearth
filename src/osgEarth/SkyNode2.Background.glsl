in vec2 oe_s2_uv;
out vec4 oe_s2_result;
uniform sampler2D oe_sky2_stars;
uniform mat3 oe_sky2_earthToECI;
uniform vec4 oe_sky2_moon; // direction and angular radius

// Composites catalog stars, the solar disk and a phase-lit lunar sphere behind the atmosphere.
void main()
{
    vec3 direction = normalize(oe_sky2_viewToEarth*oe_s2_viewRay(oe_s2_uv));
    vec3 radiance = vec3(0.0), transmission = vec3(1.0);
    vec2 ground = oe_s2_sphere(oe_sky2_eye,direction,oe_s2_radius);
    bool blocked = ground.x > 0.0;
    if (!blocked)
    {
        vec3 star = oe_sky2_earthToECI*direction;
        vec2 uv = vec2(atan(star.y,star.x)/(2.0*oe_s2_pi),acos(clamp(star.z,-1.0,1.0))/oe_s2_pi);
        radiance = texture(oe_sky2_stars,uv).rgb*oe_sky2_flags.w*0.08;
        float sunAngle = acos(clamp(dot(direction,oe_sky2_sun),-1.0,1.0));
        float aa = max(fwidth(sunAngle),1e-5);
        float sunDisk = 1.0-smoothstep(0.00465-aa,0.00465+aa,sunAngle);
        radiance += oe_sky2_solarIrradiance*2.0*oe_sky2_flags.y*sunDisk;
        float moonAngle = acos(clamp(dot(direction,oe_sky2_moon.xyz),-1.0,1.0));
        float moonRadius = oe_sky2_moon.w;
        float moonAA = max(fwidth(moonAngle),1e-5);
        float moonDisk = 1.0-smoothstep(moonRadius-moonAA,moonRadius+moonAA,moonAngle);
        if (moonDisk > 0.0 && oe_sky2_flags.z > 0.5)
        {
            vec3 offset = (direction-oe_sky2_moon.xyz*dot(direction,oe_sky2_moon.xyz))/moonRadius;
            vec3 normal = offset-oe_sky2_moon.xyz*sqrt(max(0.0,1.0-dot(offset,offset)));
            float phase = max(0.0,dot(normal,oe_sky2_sun));
            // Low-frequency lunar albedo; no external asset or failed texture load at startup.
            float maria = 0.65+0.15*sin(offset.x*19.0+sin(offset.z*13.0))*sin(offset.y*17.0);
            radiance = mix(radiance,oe_sky2_solarIrradiance*(maria*(phase*0.08+0.0008)),moonDisk);
        }
    }
#ifdef OE_SKY2_ATMOSPHERE
    if (oe_sky2_flags.x > 0.5)
    {
        // Extinction along the entire celestial ray; outside the shell it starts at the entry point.
        vec2 shell = oe_s2_sphere(oe_sky2_eye,direction,oe_s2_top);
        if (shell.y > max(shell.x,0.0))
        {
            vec3 origin = oe_sky2_eye+direction*max(0.0,shell.x+0.001);
            transmission = oe_s2_transmittance(origin,direction);
        }
        radiance = radiance*transmission+oe_s2_sky(direction);
    }
#endif
    oe_s2_result = vec4(oe_s2_output(radiance),1.0);
}

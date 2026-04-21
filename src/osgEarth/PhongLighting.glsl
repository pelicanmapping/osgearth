#pragma vp_name       Phong Lighting Vertex Stage
#pragma vp_entryPoint oe_phong_vertex
#pragma vp_location   vertex_view

#pragma import_defines(OE_LIGHTING)


out vec3 oe_phong_vertexView3;

void oe_phong_vertex(inout vec4 VertexVIEW)
{
#ifndef OE_LIGHTING
    return;
#endif

    oe_phong_vertexView3 = VertexVIEW.xyz / VertexVIEW.w;
}


[break]

#pragma vp_name       Phong Lighting Fragment Stage
#pragma vp_entryPoint oe_phong_fragment
#pragma vp_location   fragment_lighting

#pragma import_defines(OE_LIGHTING)
#pragma import_defines(OE_NUM_LIGHTS)

#ifdef OE_LIGHTING

in vec3 oe_phong_vertexView3; 

// stage global
in vec3 vp_Normal;

// Parameters of each light:
struct osg_LightSourceParameters 
{   
   vec4 ambient;
   vec4 diffuse;
   vec4 specular;
   vec4 position;
   vec3 spotDirection;
   float spotExponent;
   float spotCutoff;
   float spotCosCutoff;
   float constantAttenuation;
   float linearAttenuation;
   float quadraticAttenuation;

   bool enabled;
};  
uniform osg_LightSourceParameters osg_LightSource[OE_NUM_LIGHTS];

// Surface material:
struct osg_MaterialParameters  
{   
   vec4 emission;    // Ecm   
   vec4 ambient;     // Acm   
   vec4 diffuse;     // Dcm   
   vec4 specular;    // Scm   
   float shininess;  // Srm  
};  
uniform osg_MaterialParameters osg_FrontMaterial; 

void oe_phong_fragment(inout vec4 color) 
{
    // See:
    // https://en.wikipedia.org/wiki/Phong_reflection_model
    // https://www.opengl.org/sdk/docs/tutorials/ClockworkCoders/lighting.php
    // https://en.wikibooks.org/wiki/GLSL_Programming/GLUT/Multiple_Lights

    vec3 N = normalize(vp_Normal);
    // V is the normalized vertex-to-camera vector.
    vec3 V = -normalize(oe_phong_vertexView3);

    float shine = clamp(osg_FrontMaterial.shininess, 1.0, 128.0);
    vec3 surfaceSpecularity = osg_FrontMaterial.specular.rgb;

    // Accumulate the lighting, starting with material emission.
    vec3 totalDiffuse = vec3(0.0);
    vec3 totalAmbient = vec3(0.0);
    vec3 totalSpecular = vec3(0.0);
    
    int numLights = OE_NUM_LIGHTS;

    for (int i=0; i<numLights; ++i)
    {
        if (osg_LightSource[i].enabled)
        {
            float attenuation = 1.0;

            // L is the normalized camera-to-light vector.
            vec3 L = normalize(osg_LightSource[i].position.xyz);

            // point or spot light:
            if (osg_LightSource[i].position.w != 0.0)
            {
                // calculate VL, the vertex-to-light vector:
                vec4 VL = vec4(oe_phong_vertexView3, 1.0) * osg_LightSource[i].position.w;
                vec4 VL4 = osg_LightSource[i].position - VL;
                L = normalize(VL4.xyz);

                // calculate attenuation:
                float distance = length(VL4);
                attenuation = 1.0 / (
                    osg_LightSource[i].constantAttenuation +
                    osg_LightSource[i].linearAttenuation * distance +
                    osg_LightSource[i].quadraticAttenuation * distance * distance);

                // for a spot light, the attenuation help form the cone:
                if (osg_LightSource[i].spotCutoff <= 90.0)
                {
                    vec3 D = normalize(osg_LightSource[i].spotDirection);
                    float clampedCos = max(0.0, dot(-L,D));
                    attenuation = clampedCos < osg_LightSource[i].spotCosCutoff ?
                        0.0 :
                        attenuation * pow(clampedCos, osg_LightSource[i].spotExponent);
                }
            }

            vec3 ambientReflection =
                attenuation *
                osg_LightSource[i].ambient.rgb;

            float NdotL = max(dot(N,L), 0.0); 

            vec3 diffuseReflection =
                attenuation
                * osg_LightSource[i].diffuse.rgb
                * NdotL;
                
            vec3 specularReflection = vec3(0.0);
            if (NdotL > 0.0)
            {
                vec3 H = reflect(-L,N); 
                float HdotV = max(dot(H,V), 0.0); 

                specularReflection =
                    attenuation
                    * osg_LightSource[i].specular.rgb
                    * surfaceSpecularity
                    * pow(HdotV, shine);
            }

            totalDiffuse += diffuseReflection;
            totalAmbient += ambientReflection;
            totalSpecular += specularReflection;
        }
    }

    vec3 lightColor =
        osg_FrontMaterial.emission.rgb +
        totalDiffuse * osg_FrontMaterial.diffuse.rgb +
        totalAmbient * osg_FrontMaterial.ambient.rgb;

    color.rgb =
        color.rgb * lightColor +
        totalSpecular;
}

#else

// nop
void oe_phong_fragment(inout vec4 color) { }

#endif

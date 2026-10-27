// A host module may not declare a resource: compile-host refuses it, naming the parameter.
Texture2D<float4> u_Albedo;

export __extern_cpp float Brightness(float x)
{
    return x;
}

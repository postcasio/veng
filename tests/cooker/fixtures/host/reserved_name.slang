// A struct named as the generated header's prepared-parameter handle, which the compile refuses.
struct Prepared
{
    float Scale;
};

export __extern_cpp float Apply(Prepared prepared, float x)
{
    return prepared.Scale * x;
}

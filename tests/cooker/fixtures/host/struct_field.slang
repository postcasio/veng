// A struct crosses only when every field does: compile-host refuses one holding a bool, naming the
// field.
struct Visibility
{
    float Distance;
    bool Visible;
};

export __extern_cpp float Fade(Visibility visibility)
{
    return visibility.Visible ? visibility.Distance : 0.0;
}

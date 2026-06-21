# Game systems

Gameplay logic lives in scene systems. A `SceneSystem` operates over a `Scene` and
runs each frame, and the same systems run both in your application and in the
editor's Play mode.

## Writing a system

Subclass `SceneSystem` and implement `OnUpdate`. `OnStart` and `OnStop` are
optional and default to doing nothing.

```cpp
class SpinnerSystem final : public SceneSystem
{
public:
    void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override
    {
        scene.Each<Transform, Spinner>(
            [delta](Entity, Transform& transform, Spinner& spinner)
            {
                const quat step = glm::angleAxis(spinner.Speed * delta, vec3(0, 1, 0));
                transform.Rotation = glm::normalize(step * transform.Rotation);
            });
    }
};
```

`OnUpdate` receives the scene, the time since the last frame, and a
`SystemContext` giving access to the asset manager (`context.Assets`) and input
(`context.Input`).

## Registering systems

Register your systems in `VengModuleRegister`. They run in registration order.

```cpp
host->Systems.Register<SpinnerSystem>();
```

## Running them

To run the registered systems, create a `SceneSimulation` from the system registry
and drive it over your scene — start it once, update it each frame, and stop it
when the session ends:

```cpp
void OnInitialize() override
{
    // ... create m_Scene ...
    m_Simulation = CreateUnique<SceneSimulation>(GetSystemRegistry());
    m_Simulation->Start(*m_Scene, { .Assets = GetAssetManager(), .Input = GetInput() });
}

void OnUpdate(f32 delta) override
{
    m_Simulation->Update(*m_Scene, delta, { .Assets = GetAssetManager(), .Input = GetInput() });
}
```

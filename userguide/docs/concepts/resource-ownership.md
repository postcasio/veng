# Resource lifetime

Two things about resource lifetime are specific to veng.

## Dropping a resource mid-frame is safe

Destroying a resource doesn't free its GPU memory immediately. The handle is
retired and freed later, once the GPU has finished the frame that used it. So you
can drop a resource at any time without checking whether it's still in use:

```cpp
m_Image.reset();   // safe even if the GPU is still using it
```

## Release resources in `OnDispose`

Release everything you created in `Application::OnDispose()`. A resource that
outlives the engine fails on destruction.

# Bindless resources

veng binds resources by handle rather than per draw. Textures, samplers, storage
images, and material parameter blocks all live in one global table (descriptor set
0), and a shader selects what it needs with a small integer index.

## Handles come from assets

Most of the time you get handles from the asset system without thinking about the
table. Loading a `Texture` gives you a `TextureHandle` to use in a draw, and
building a `Material` registers its parameters and resolves its texture handles for
you. See [Shaders & materials](materials.md).

If you manage a raw GPU resource yourself — an app-owned render target you want to
sample, say — register it with the bindless registry to get a handle:

```cpp
TextureHandle handle = Context.GetBindlessRegistry().Register(myImageView, mySampler);
```

`Register` returns a typed handle (`TextureHandle`, `SamplerHandle`,
`StorageImageHandle`, `MaterialHandle`); `Release` frees the slot.

## Set 0 is reserved

The engine uses descriptor set 0 for this table in every pipeline. If you write
your own shaders, your descriptor sets start at set 1 — set 0 is taken.

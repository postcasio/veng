add_subdirectory(hello-triangle)

# examples/template is a standalone find_package(veng) consumer, not part of the engine
# build graph — the out-of-tree exemplar the SDK conformance test configures against an
# install prefix or the build-tree export. Only hello-triangle stays in-tree.

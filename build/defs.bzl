"""Common dependencies for k-vm native C modules."""

def vm_library(name, deps = [], copts = [], **kwargs):
    native.cc_library(
        name = name,
        # Upstream calls required filesystem operations inside assert().
        copts = copts + ["-UNDEBUG"],
        deps = ["//build:config", "//src/include:headers"] + deps,
        **kwargs
    )

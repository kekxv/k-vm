"""Common dependencies for k-vm native C modules."""

load("@rules_cc//cc:cc_library.bzl", "cc_library")

def vm_library(name, deps = [], copts = [], **kwargs):
    cc_library(
        name = name,
        # Upstream calls required filesystem operations inside assert().
        copts = copts + ["-UNDEBUG"],
        deps = ["//build:config", "//src/include:headers"] + deps,
        **kwargs
    )

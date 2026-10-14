"""Expose Homebrew headers and libraries as declared Bazel inputs."""

def _macos_dependencies_impl(ctx):
    if ctx.os.name != "mac os x":
        ctx.file("BUILD.bazel", "")
        return
    brew = ctx.which("brew")
    if not brew:
        fail("Homebrew is required for macOS system builds")
    result = ctx.execute([brew, "--prefix", ctx.attr.formula])
    if result.return_code:
        fail("brew install " + ctx.attr.formula + " is required: " + result.stderr)
    prefix = result.stdout.strip()
    ctx.symlink(prefix + "/include", "include")
    ctx.symlink(prefix + "/lib/" + ctx.attr.library, ctx.attr.library)
    ctx.file("BUILD.bazel", """
load("@rules_cc//cc:cc_import.bzl", "cc_import")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
package(default_visibility = ["//visibility:public"])
cc_import(name = "lib", shared_library = "{library}")
cc_library(name = "headers", hdrs = glob(["include/**/*.h"]),
           includes = ["include"], deps = [":lib"])
""".format(library = ctx.attr.library))

macos_dependencies = repository_rule(
    implementation = _macos_dependencies_impl,
    attrs = {"formula": attr.string(mandatory = True), "library": attr.string(mandatory = True)},
    local = True,
)
